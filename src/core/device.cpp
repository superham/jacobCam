// SPDX-License-Identifier: GPL-2.0-or-later
//
// Camera facade.

#include "qcam/device.h"

#include <chrono>
#include <thread>

#include "qcam/log.h"

namespace qcam {

uint64_t NowIn100ns() {
    using namespace std::chrono;
    const auto now = steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(duration_cast<nanoseconds>(now).count() / 100);
}

Camera::Camera() = default;

Camera::~Camera() { Close(); }

Status Camera::Open(std::unique_ptr<IUsbTransport> transport, const CameraConfig& cfg) {
    if (!transport) return Status::InvalidArg;

    std::lock_guard<std::mutex> lock(mutex_);
    if (sensor_) return Status::Busy;

    const UsbDeviceInfo& info = transport->Info();
    const DeviceId* id = LookupDevice(info.vid, info.pid);
    if (!id) {
        QCAM_LOGE("USB %04x:%04x is not a device this driver supports",
                  info.vid, info.pid);
        return Status::Unsupported;
    }
    QCAM_LOGI("opening %s (%04x:%04x, %s bridge)", id->marketing_name, id->vid,
              id->pid, BridgeName(id->bridge));

    cfg_ = cfg;
    transport_ = std::move(transport);
    bridge_.reset(new Stv06xxBridge(*transport_, id->bridge));

    // The device needs a moment after enumeration before the ASIC will answer
    // I2C reliably.
    if (cfg_.settle_ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.settle_ms));

    Status st = ProbeSensor(*bridge_, &sensor_);
    if (Failed(st)) {
        bridge_.reset();
        transport_.reset();
        return st;
    }

    st = sensor_->Init(*bridge_);
    if (Failed(st)) {
        QCAM_LOGE("sensor init failed: %s", StatusName(st));
        sensor_.reset();
        bridge_.reset();
        transport_.reset();
        return st;
    }

    const FrameGeometry geom = sensor_->Geometry();
    QCAM_LOGI("sensor %s, %ux%u Bayer, nominal %.1f fps", sensor_->Name(),
              geom.width, geom.height, sensor_->NominalFps());

    framer_.Configure(geom, id->bridge);
    framer_.SetEmitShortFrames(cfg_.emit_short_frames);
    framer_.SetFrameHandler([this](const RawFrame& f) { OnRawFrame(f); });
    // The ST6422 prefixes every frame with four lines of garbage.
    if (id->bridge == Bridge::St6422)
        framer_.SetLeadingSkip(static_cast<size_t>(geom.width) * 4);

    decoder_.Configure(geom);
    decoder_.SetQuality(cfg_.quality);
    decoder_.SetColor(cfg_.color);
    {
        std::lock_guard<std::mutex> slock(settings_mutex_);
        pending_color_ = cfg_.color;
        color_dirty_.store(false, std::memory_order_release);
    }
    decoder_.SetOutputSize(cfg_.out_width, cfg_.out_height);

    auto_exp_.SetConfig(cfg_.auto_exposure);
    auto_exp_.Reset();

    out_buffer_.assign(
        ImageSize(cfg_.format, decoder_.out_width(), decoder_.out_height()), 0);

    stats_ = CameraStats{};
    stats_.exposure = auto_exp_.state().exposure;
    stats_.gain     = auto_exp_.state().gain;
    return Status::Ok;
}

Status Camera::OpenFirst(const CameraConfig& cfg) {
    std::vector<UsbDeviceInfo> devices;
    QCAM_TRY(EnumerateDevices(&devices));
    if (devices.empty()) {
        QCAM_LOGE("no supported camera found");
        return Status::NoDevice;
    }
    std::unique_ptr<IUsbTransport> transport;
    QCAM_TRY(OpenDevice(devices.front().device_path, &transport));
    return Open(std::move(transport), cfg);
}

void Camera::Close() {
    Stop();

    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (sensor_ && bridge_) sensor_->Stop(*bridge_);
    sensor_.reset();
    bridge_.reset();
    transport_.reset();
}

Status Camera::Start(FrameHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (!sensor_ || !transport_) return Status::NoDevice;
    if (streaming_.load()) return Status::Busy;

    handler_ = std::move(handler);
    framer_.Reset();

    // Switch to the alternate setting that reserves isochronous bandwidth.
    QCAM_TRY(transport_->SetAltSetting(kAltStreaming));

    uint16_t packet = cfg_.iso_packet_size ? cfg_.iso_packet_size
                                           : sensor_->PreferredPacketSize();
    uint16_t actual = 0;
    Status st = transport_->GetIsoMaxPacketSize(kAltStreaming, &actual);
    if (Succeeded(st) && actual && actual < packet) {
        // The host controller gave us less bandwidth than we asked for. Tell
        // the ASIC the truth or it will over-run every packet.
        QCAM_LOGW("iso endpoint negotiated down to %u bytes (wanted %u)", actual,
                  packet);
        packet = actual;
    }

    QCAM_TRY(bridge_->SetIsoPacketSize(packet));
    QCAM_TRY(sensor_->Start(*bridge_));
    QCAM_TRY(bridge_->EnableIso(true));

    st = transport_->StartIso(&framer_);
    if (Failed(st)) {
        bridge_->EnableIso(false);
        sensor_->Stop(*bridge_);
        transport_->SetAltSetting(kAltIdle);
        return st;
    }

    streaming_.store(true);
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        fps_window_start_ns_ = NowIn100ns();
        fps_window_frames_   = 0;
    }
    QCAM_LOGI("streaming started (%u byte packets, %s %ux%u)", packet,
              PixelFormatName(cfg_.format), decoder_.out_width(),
              decoder_.out_height());
    return Status::Ok;
}

Status Camera::Stop() {
    if (!streaming_.exchange(false)) return Status::Ok;

    // Join the streaming thread *before* taking any lock. The frame callback
    // runs on that thread and takes ctrl_mutex_; holding a lock across the
    // join would deadlock against it. Open()/Close() cannot race with this
    // because Close() calls Stop() first, so the member pointers are stable.
    if (transport_) transport_->StopIso();

    // No callback can be in flight past this point.
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    // No LED writes anywhere: on the V-UB2, writing 1 to the LED register
    // (0x1445) wedges the bridge until the camera is replugged, streaming or
    // not, and there is no LED fitted to drive. Writing 0 was harmless, but
    // it does nothing either.
    if (bridge_) bridge_->EnableIso(false);
    if (sensor_ && bridge_) sensor_->Stop(*bridge_);
    if (transport_) transport_->SetAltSetting(kAltIdle);
    handler_ = nullptr;
    QCAM_LOGI("streaming stopped");
    return Status::Ok;
}

void Camera::OnRawFrame(const RawFrame& raw) {
    // Runs on the transport's streaming thread.
    if (!streaming_.load()) return;

    if (color_dirty_.exchange(false, std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        decoder_.SetColor(pending_color_);
    }

    Status st = decoder_.Convert(raw.data, raw.size, cfg_.format,
                                 out_buffer_.data(), out_buffer_.size());
    if (Failed(st)) {
        QCAM_LOGW("decode failed: %s", StatusName(st));
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.frames_dropped++;
        return;
    }

    if (handler_) {
        DecodedFrame frame;
        frame.data            = out_buffer_.data();
        frame.size            = out_buffer_.size();
        frame.format          = cfg_.format;
        frame.width           = decoder_.out_width();
        frame.height          = decoder_.out_height();
        frame.sequence        = raw.sequence;
        frame.timestamp_100ns = raw.timestamp_100ns;
        frame.complete        = raw.complete;
        handler_(frame);
    }

    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.frames_delivered++;
        stats_.framer = framer_.stats();
        fps_window_frames_++;
        const uint64_t now = raw.timestamp_100ns;
        const uint64_t elapsed = now - fps_window_start_ns_;
        if (elapsed >= 10'000'000ull) {  // one second in 100 ns units
            stats_.measured_fps =
                static_cast<double>(fps_window_frames_) * 10'000'000.0 / elapsed;
            fps_window_start_ns_ = now;
            fps_window_frames_   = 0;
        }
    }

    ApplyAutoExposure(raw);
}

Status Camera::ApplyAutoExposure(const RawFrame& raw) {
    // Runs on the streaming thread. auto_exp_ is only ever touched under
    // ctrl_mutex_, which is also what the manual control setters take.
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (!auto_exp_.enabled()) return Status::Ok;
    if (!sensor_ || !bridge_) return Status::NoDevice;

    const bool changed = auto_exp_.Update(raw.data, sensor_->Geometry());
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.luma     = auto_exp_.state().measured_luma;
        stats_.exposure = auto_exp_.state().exposure;
        stats_.gain     = auto_exp_.state().gain;
    }
    if (!changed) return Status::Ok;

    // Short vendor control transfers on endpoint 0; they do not disturb the
    // isochronous pipeline.
    QCAM_TRY(sensor_->SetExposure(*bridge_, auto_exp_.state().exposure));
    return sensor_->SetGain(*bridge_, auto_exp_.state().gain);
}

Status Camera::SetExposure(int value) {
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (!sensor_ || !bridge_) return Status::NoDevice;
    auto_exp_.SetManual(value, auto_exp_.state().gain);
    QCAM_TRY(sensor_->SetExposure(*bridge_, value));
    std::lock_guard<std::mutex> slock(stats_mutex_);
    stats_.exposure = auto_exp_.state().exposure;
    return Status::Ok;
}

Status Camera::SetGain(int value) {
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (!sensor_ || !bridge_) return Status::NoDevice;
    auto_exp_.SetManual(auto_exp_.state().exposure, value);
    QCAM_TRY(sensor_->SetGain(*bridge_, value));
    std::lock_guard<std::mutex> slock(stats_mutex_);
    stats_.gain = auto_exp_.state().gain;
    return Status::Ok;
}

Status Camera::SetAutoExposure(bool on) {
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    auto_exp_.SetEnabled(on);
    return Status::Ok;
}

bool Camera::auto_exposure() const {
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    return auto_exp_.enabled();
}

void Camera::SetColorSettings(const ColorSettings& c) {
    // The decoder belongs to the streaming thread; stage the change and let
    // OnRawFrame pick it up at a frame boundary.
    std::lock_guard<std::mutex> lock(settings_mutex_);
    pending_color_ = c;
    color_dirty_.store(true, std::memory_order_release);
}

ColorSettings Camera::color_settings() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return pending_color_;
}

Status Camera::SetLed(bool on) {
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (!bridge_) return Status::NoDevice;
    return bridge_->SetLed(on);
}

const char* Camera::sensor_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sensor_ ? sensor_->Name() : "(none)";
}

FrameGeometry Camera::sensor_geometry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sensor_ ? sensor_->Geometry() : FrameGeometry{0, 0, BayerPhase::GRBG};
}

uint16_t Camera::out_width() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_.out_width();
}

uint16_t Camera::out_height() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_.out_height();
}

double Camera::nominal_fps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sensor_ ? sensor_->NominalFps() : 0.0;
}

const UsbDeviceInfo& Camera::device_info() const {
    static const UsbDeviceInfo kEmpty{};
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_ ? transport_->Info() : kEmpty;
}

CameraStats Camera::stats() const {
    std::lock_guard<std::mutex> slock(stats_mutex_);
    return stats_;
}

Status Camera::DumpRegisters() {
    std::lock_guard<std::mutex> ctrl(ctrl_mutex_);
    if (!sensor_ || !bridge_) return Status::NoDevice;

    QCAM_LOGI("bridge register dump (0x%04x..0x%04x):", reg::kDumpFirst,
              reg::kDumpLast);
    for (uint16_t a = reg::kDumpFirst; a <= reg::kDumpLast; ++a) {
        uint8_t v = 0;
        if (Succeeded(bridge_->ReadReg(a, &v)))
            QCAM_LOGI("  0x%04x = 0x%02x", a, v);
    }
    return sensor_->DumpRegisters(*bridge_);
}

}  // namespace qcam
