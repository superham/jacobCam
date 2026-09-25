// SPDX-License-Identifier: GPL-2.0-or-later
//
// qcamctl - diagnostics and capture tool for the qcam stack.
//
// This exists so the hardware can be brought up one layer at a time: does the
// device enumerate, does the sensor answer on I2C, do frames arrive, do they
// decode. When something is wrong, the answer to "which of those failed" is
// what you need first, and it is much easier to get from a console tool than
// from a camera that is merely absent in Teams.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "qcam/decode.h"
#include "qcam/device.h"
#include "qcam/log.h"
#include "qcam/mock.h"
#include "qcam/ring.h"
#include "qcam/sensor.h"
#include "qcam/usb.h"

using namespace qcam;

namespace {

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

// 24-bit BMP: no dependencies, and it opens with a double click on Windows.
bool WriteBmp(const std::string& path, const uint8_t* rgb, uint16_t width,
              uint16_t height) {
    const uint32_t row_bytes = static_cast<uint32_t>(width) * 3;
    const uint32_t padding   = (4 - (row_bytes % 4)) % 4;
    const uint32_t stride    = row_bytes + padding;
    const uint32_t pixels    = stride * height;
    const uint32_t offset    = 14 + 40;
    const uint32_t file_size = offset + pixels;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<char*>(&v), 2); };
    auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<char*>(&v), 4); };

    out.write("BM", 2);
    u32(file_size); u16(0); u16(0); u32(offset);
    u32(40); u32(width); u32(height); u16(1); u16(24);
    u32(0); u32(pixels); u32(2835); u32(2835); u32(0); u32(0);

    // BMP rows run bottom-up and store BGR.
    std::vector<uint8_t> row(stride, 0);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* src = rgb + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 3 + 2];
            row[x * 3 + 1] = src[x * 3 + 1];
            row[x * 3 + 2] = src[x * 3 + 0];
        }
        out.write(reinterpret_cast<char*>(row.data()), stride);
    }
    return out.good();
}

bool WriteRaw(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

void LogToStderr(LogLevel level, const char* msg) {
    static const char* kNames[] = {"error", "warn", "info", "debug", "trace"};
    int idx = static_cast<int>(level);
    if (idx < 0 || idx > 4) idx = 2;
    std::fprintf(stderr, "%-5s %s\n", kNames[idx], msg);
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct Args {
    std::string command;
    std::vector<std::string> positional;

    bool     verbose      = false;
    int      count        = 1;
    double   seconds      = 5.0;
    std::string output    = "qcam";
    std::string format    = "bmp";
    int      exposure     = -1;
    int      gain         = -1;
    bool     no_awb       = false;
    bool     malvar       = false;
    uint16_t out_width    = 0;
    uint16_t out_height   = 0;
    uint16_t packet_size  = 0;
    std::string device;

    bool Has(const std::string& flag) const {
        return std::find(positional.begin(), positional.end(), flag) !=
               positional.end();
    }
};

int ParseInt(const char* s, int fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 0);
    return (end && *end == '\0') ? static_cast<int>(v) : fallback;
}

bool ParseArgs(int argc, char** argv, Args* args) {
    if (argc < 2) return false;
    args->command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](void) -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };

        if (arg == "-v" || arg == "--verbose")      args->verbose = true;
        else if (arg == "-n" || arg == "--count")   args->count = ParseInt(next(), 1);
        else if (arg == "-t" || arg == "--seconds") args->seconds = ParseInt(next(), 5);
        else if (arg == "-o" || arg == "--output")  { const char* v = next(); if (v) args->output = v; }
        else if (arg == "-f" || arg == "--format")  { const char* v = next(); if (v) args->format = v; }
        else if (arg == "--exposure")               args->exposure = ParseInt(next(), -1);
        else if (arg == "--gain")                   args->gain = ParseInt(next(), -1);
        else if (arg == "--no-awb")                 args->no_awb = true;
        else if (arg == "--malvar")                 args->malvar = true;
        else if (arg == "--packet-size")            args->packet_size = static_cast<uint16_t>(ParseInt(next(), 0));
        else if (arg == "--device")                 { const char* v = next(); if (v) args->device = v; }
        else if (arg == "--size") {
            const char* v = next();
            if (v) {
                int w = 0, h = 0;
                if (std::sscanf(v, "%dx%d", &w, &h) == 2) {
                    args->out_width  = static_cast<uint16_t>(w);
                    args->out_height = static_cast<uint16_t>(h);
                }
            }
        } else {
            args->positional.push_back(arg);
        }
    }
    return true;
}

CameraConfig ConfigFrom(const Args& args, PixelFormat format) {
    CameraConfig cfg;
    cfg.format      = format;
    cfg.out_width   = args.out_width;
    cfg.out_height  = args.out_height;
    cfg.quality     = args.malvar ? DemosaicQuality::Malvar : DemosaicQuality::Bilinear;
    cfg.color.auto_white_balance = !args.no_awb;
    cfg.iso_packet_size = args.packet_size;
    if (args.exposure >= 0 || args.gain >= 0) cfg.auto_exposure.enabled = false;
    return cfg;
}

Status OpenSelected(Camera* camera, const Args& args, const CameraConfig& cfg) {
    std::vector<UsbDeviceInfo> devices;
    QCAM_TRY(EnumerateDevices(&devices));
    if (devices.empty()) {
        std::fprintf(stderr,
                     "no camera found.\n"
                     "  - is it plugged in?\n"
                     "  - has qcamusb.inf been installed? "
                     "(Device Manager should show it under 'Universal Serial Bus devices')\n");
        return Status::NoDevice;
    }

    std::string path = devices.front().device_path;
    if (!args.device.empty()) {
        bool found = false;
        for (const auto& d : devices) {
            if (d.device_path.find(args.device) != std::string::npos) {
                path = d.device_path;
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "no device matching '%s'\n", args.device.c_str());
            return Status::NoDevice;
        }
    }

    std::unique_ptr<IUsbTransport> transport;
    QCAM_TRY(OpenDevice(path, &transport));
    return camera->Open(std::move(transport), cfg);
}

void PrintCameraSummary(const Camera& camera) {
    const FrameGeometry geom = camera.sensor_geometry();
    const UsbDeviceInfo& info = camera.device_info();
    std::printf("  device      : %04x:%04x  %s\n", info.vid, info.pid,
                info.friendly_name.c_str());
    std::printf("  sensor      : %s\n", camera.sensor_name());
    std::printf("  native      : %ux%u Bayer GRBG\n", geom.width, geom.height);
    std::printf("  output      : %ux%u %s\n", camera.out_width(),
                camera.out_height(), PixelFormatName(camera.format()));
    std::printf("  nominal fps : %.2f\n", camera.nominal_fps());
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CmdList(const Args&) {
    std::vector<UsbDeviceInfo> devices;
    const Status st = EnumerateDevices(&devices);
    if (Failed(st)) {
        std::fprintf(stderr, "enumeration failed: %s\n", StatusName(st));
        return 1;
    }

    if (devices.empty()) {
        std::printf("no qcam-bound cameras found.\n\n");
        std::printf("If the camera is plugged in but not listed, it is probably\n"
                    "not bound to WinUSB yet. Check Device Manager for an\n"
                    "unknown USB device with hardware id USB\\VID_046D&PID_xxxx\n"
                    "and install driver/qcamusb.inf (see docs/installing.md).\n\n");
        std::printf("Devices this build knows how to drive:\n");
        for (size_t i = 0; i < kKnownDeviceCount; ++i) {
            const DeviceId& d = kKnownDevices[i];
            std::printf("  %04x:%04x  %-28s %s\n", d.vid, d.pid, d.marketing_name,
                        BridgeName(d.bridge));
        }
        return 1;
    }

    std::printf("%zu camera(s):\n", devices.size());
    for (const auto& d : devices) {
        const DeviceId* id = LookupDevice(d.vid, d.pid);
        std::printf("  %04x:%04x  %-28s %s\n", d.vid, d.pid,
                    d.friendly_name.c_str(), id ? BridgeName(id->bridge) : "?");
        std::printf("             %s\n", d.device_path.c_str());
    }
    return 0;
}

int CmdProbe(const Args& args) {
    Camera camera;
    CameraConfig cfg = ConfigFrom(args, PixelFormat::Nv12);
    const Status st = OpenSelected(&camera, args, cfg);
    if (Failed(st)) {
        std::fprintf(stderr, "probe failed: %s\n", StatusName(st));
        return 1;
    }
    std::printf("camera opened.\n");
    PrintCameraSummary(camera);
    return 0;
}

int CmdRegDump(const Args& args) {
    Camera camera;
    CameraConfig cfg = ConfigFrom(args, PixelFormat::Nv12);
    if (Failed(OpenSelected(&camera, args, cfg))) return 1;

    SetLogLevel(LogLevel::Info);
    const Status st = camera.DumpRegisters();
    if (Failed(st)) {
        std::fprintf(stderr, "register dump failed: %s\n", StatusName(st));
        return 1;
    }
    return 0;
}

int CmdCapture(const Args& args) {
    const bool want_raw = (args.format == "raw" || args.format == "bayer");
    Camera camera;
    CameraConfig cfg = ConfigFrom(args, want_raw ? PixelFormat::Bayer8
                                                 : PixelFormat::Bgra32);
    if (Failed(OpenSelected(&camera, args, cfg))) return 1;

    PrintCameraSummary(camera);
    if (args.exposure >= 0) camera.SetExposure(args.exposure);
    if (args.gain >= 0)     camera.SetGain(args.gain);

    std::atomic<int> saved{0};
    const int want = std::max(1, args.count);
    // Auto exposure needs a handful of frames to settle before the picture is
    // worth keeping; skip those rather than writing black files.
    const int warmup = camera.auto_exposure() ? 8 : 1;
    std::atomic<int> seen{0};

    const Status st = camera.Start([&](const DecodedFrame& frame) {
        if (!frame.complete) return;
        const int n = ++seen;
        if (n <= warmup) return;
        if (saved.load() >= want) return;

        const int index = saved.fetch_add(1);
        char path[512];
        std::snprintf(path, sizeof(path), "%s_%03d.%s", args.output.c_str(), index,
                      want_raw ? "bayer" : "bmp");

        bool ok;
        if (want_raw) {
            ok = WriteRaw(path, frame.data, frame.size);
        } else {
            // BGRA back to RGB24 for the BMP writer.
            std::vector<uint8_t> rgb(static_cast<size_t>(frame.width) * frame.height * 3);
            for (size_t i = 0; i < rgb.size() / 3; ++i) {
                rgb[i * 3 + 0] = frame.data[i * 4 + 2];
                rgb[i * 3 + 1] = frame.data[i * 4 + 1];
                rgb[i * 3 + 2] = frame.data[i * 4 + 0];
            }
            ok = WriteBmp(path, rgb.data(), frame.width, frame.height);
        }
        std::printf("%s %s (%ux%u, %zu bytes)\n", ok ? "wrote" : "FAILED to write",
                    path, frame.width, frame.height, frame.size);
    });
    if (Failed(st)) {
        std::fprintf(stderr, "could not start streaming: %s\n", StatusName(st));
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (saved.load() < want && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    camera.Stop();
    if (saved.load() < want) {
        std::fprintf(stderr, "timed out: captured %d of %d frames\n", saved.load(),
                     want);
        return 1;
    }
    return 0;
}

int CmdStream(const Args& args) {
    Camera camera;
    CameraConfig cfg = ConfigFrom(args, PixelFormat::Nv12);
    if (Failed(OpenSelected(&camera, args, cfg))) return 1;

    PrintCameraSummary(camera);
    if (args.exposure >= 0) camera.SetExposure(args.exposure);
    if (args.gain >= 0)     camera.SetGain(args.gain);

    std::atomic<uint64_t> frames{0};
    const Status st = camera.Start([&](const DecodedFrame&) { frames++; });
    if (Failed(st)) {
        std::fprintf(stderr, "could not start streaming: %s\n", StatusName(st));
        return 1;
    }

    std::printf("\nstreaming for %.0f s; press Ctrl-C to stop early\n\n",
                args.seconds);
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(
                                      static_cast<long long>(args.seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const CameraStats s = camera.stats();
        std::printf("  %6llu frames  %5.2f fps  luma %3d  exp %3d  gain %3d  "
                    "short %llu  overrun %llu  unknown %llu\n",
                    static_cast<unsigned long long>(s.frames_delivered),
                    s.measured_fps, s.luma, s.exposure, s.gain,
                    static_cast<unsigned long long>(s.framer.frames_short),
                    static_cast<unsigned long long>(s.framer.frames_overrun),
                    static_cast<unsigned long long>(s.framer.unknown_chunks));
        std::fflush(stdout);
    }

    camera.Stop();
    const CameraStats s = camera.stats();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("\nsummary\n");
    std::printf("  frames delivered : %llu in %.1f s (%.2f fps)\n",
                static_cast<unsigned long long>(s.frames_delivered), elapsed,
                elapsed > 0 ? s.frames_delivered / elapsed : 0.0);
    std::printf("  iso packets      : %llu\n",
                static_cast<unsigned long long>(s.framer.packets));
    std::printf("  chunks           : %llu (%llu unknown)\n",
                static_cast<unsigned long long>(s.framer.chunks),
                static_cast<unsigned long long>(s.framer.unknown_chunks));
    std::printf("  complete / short : %llu / %llu\n",
                static_cast<unsigned long long>(s.framer.frames_complete),
                static_cast<unsigned long long>(s.framer.frames_short));
    std::printf("  bytes dropped    : %llu\n",
                static_cast<unsigned long long>(s.framer.bytes_dropped));

    if (s.frames_delivered == 0) {
        std::fprintf(stderr,
                     "\nNo frames arrived. If iso packets is also 0 the transfer\n"
                     "never started; if packets arrived but chunks are mostly\n"
                     "unknown, the chunk layer is not what this driver expects -\n"
                     "capture a trace and see docs/troubleshooting.md.\n");
        return 1;
    }
    return 0;
}

int CmdAttach(const Args& args) {
    FrameRingReader reader;
    // The service only opens the camera once a reader asks, and opening it
    // runs the sensor init sequence, so the ring may take a few seconds to
    // appear. Each Open() attempt repeats the request.
    Status st = reader.Open();
    for (int i = 0; st == Status::NoDevice && i < 50; ++i) {
        if (i == 0) std::printf("waiting for the service to start the camera...\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        st = reader.Open();
    }
    if (Failed(st)) {
        std::fprintf(stderr,
                     "could not attach to the frame ring: %s\n"
                     "  - is the qcam service running, and the camera plugged in?\n"
                     "  - reading frames needs an elevated (administrator) prompt;\n"
                     "    only the Windows camera service may read them otherwise\n",
                     StatusName(st));
        return 1;
    }

    RingConfig config;
    reader.GetConfig(&config);
    std::printf("attached: %ux%u %s at %.2f fps\n", config.width, config.height,
                PixelFormatName(config.format),
                config.fps_denominator
                    ? static_cast<double>(config.fps_numerator) / config.fps_denominator
                    : 0.0);

    std::vector<uint8_t> frame;
    FrameMeta meta;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<long long>(args.seconds * 1000));
    uint64_t count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const Status r = reader.Read(&frame, &meta, 2000);
        if (r == Status::Timeout) {
            std::printf("  (no frame in 2 s)\n");
            continue;
        }
        if (Failed(r)) {
            std::fprintf(stderr, "ring read failed: %s\n", StatusName(r));
            return 1;
        }
        if (++count % 8 == 0)
            std::printf("  frame %llu, %zu bytes\n",
                        static_cast<unsigned long long>(meta.sequence), frame.size());
    }
    std::printf("read %llu frames\n", static_cast<unsigned long long>(count));
    return 0;
}

// Runs the protocol stack end to end against the mock transport. Useful on a
// machine with no camera, and as a smoke test after a build.
int CmdSelfTest(const Args&) {
    std::printf("qcam self test (mock transport, no hardware needed)\n\n");

    std::unique_ptr<MockTransport> mock(new MockTransport());
    UsbDeviceInfo info;
    info.vid = kVendorLogitech;
    info.pid = 0x0840;
    info.device_path = "mock://qcam";
    info.friendly_name = "Mock QuickCam Express";
    mock->SetInfo(info);
    mock->SetSensorRegister(0x00, 0x08);   // HDCS-1000 identity register
    MockTransport* raw = mock.get();

    CameraConfig cfg;
    cfg.settle_ms = 0;
    cfg.format = PixelFormat::Nv12;

    Camera camera;
    Status st = camera.Open(std::move(mock), cfg);
    std::printf("  open + sensor probe ... %s\n", StatusName(st));
    if (Failed(st)) return 1;
    PrintCameraSummary(camera);

    uint64_t frames = 0;
    st = camera.Start([&](const DecodedFrame&) { frames++; });
    std::printf("  start streaming     ... %s\n", StatusName(st));
    if (Failed(st)) return 1;

    const FrameGeometry geom = camera.sensor_geometry();
    std::vector<uint8_t> synthetic(geom.RawSize());
    for (size_t i = 0; i < synthetic.size(); ++i)
        synthetic[i] = static_cast<uint8_t>((i * 7) & 0xff);
    for (auto& packet : BuildFramePackets(synthetic, 843))
        raw->QueueIsoPacket(std::move(packet));
    raw->PumpIso();

    camera.Stop();
    const CameraStats s = camera.stats();
    std::printf("  frames decoded      ... %llu\n",
                static_cast<unsigned long long>(frames));
    std::printf("  chunks parsed       ... %llu\n",
                static_cast<unsigned long long>(s.framer.chunks));

    const bool ok = (frames == 1) && (s.framer.frames_complete == 1) &&
                    (s.framer.unknown_chunks == 0);
    std::printf("\n%s\n", ok ? "self test PASSED" : "self test FAILED");
    return ok ? 0 : 1;
}

int Usage() {
    std::printf(
        "qcamctl - diagnostics for the Logitech QuickCam Express driver\n"
        "\n"
        "usage: qcamctl <command> [options]\n"
        "\n"
        "commands:\n"
        "  list                 list cameras bound to the qcam WinUSB driver\n"
        "  probe                open the camera and report what it is\n"
        "  regdump              dump bridge and sensor registers\n"
        "  capture              capture frames to disk\n"
        "  stream               stream and report throughput statistics\n"
        "  attach               read frames from the running qcam service\n"
        "  selftest             exercise the stack against a mock device\n"
        "\n"
        "options:\n"
        "  -n, --count N        frames to capture (default 1)\n"
        "  -t, --seconds N      how long to stream (default 5)\n"
        "  -o, --output PREFIX  output filename prefix (default 'qcam')\n"
        "  -f, --format FMT     'bmp' (default) or 'raw' for the Bayer mosaic\n"
        "      --size WxH       output size; smaller than native crops,\n"
        "                       larger scales (native is 360x296)\n"
        "      --exposure N     manual exposure, 0-255\n"
        "      --gain N         manual gain, 0-255\n"
        "      --no-awb         disable auto white balance\n"
        "      --malvar         higher quality demosaic\n"
        "      --packet-size N  override the isochronous packet size\n"
        "      --device SUBSTR  pick a device whose path contains SUBSTR\n"
        "  -v, --verbose        verbose logging\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, &args)) return Usage();

    SetLogSink(&LogToStderr);
    SetLogLevel(args.verbose ? LogLevel::Trace : LogLevel::Warn);

    if (args.command == "list")     return CmdList(args);
    if (args.command == "probe")    return CmdProbe(args);
    if (args.command == "regdump")  return CmdRegDump(args);
    if (args.command == "capture")  return CmdCapture(args);
    if (args.command == "stream")   return CmdStream(args);
    if (args.command == "attach")   return CmdAttach(args);
    if (args.command == "selftest") return CmdSelfTest(args);

    std::fprintf(stderr, "unknown command '%s'\n\n", args.command.c_str());
    return Usage();
}
