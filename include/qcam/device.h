// SPDX-License-Identifier: GPL-2.0-or-later
//
// Camera facade: transport + bridge + sensor + framer + decoder + AE, wired
// together and driven from one place. Both qcamctl and qcamsvc use this;
// nothing above it needs to know an STV0600 exists.

#ifndef QCAM_DEVICE_H_
#define QCAM_DEVICE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qcam/autoexp.h"
#include "qcam/bridge.h"
#include "qcam/decode.h"
#include "qcam/framer.h"
#include "qcam/sensor.h"
#include "qcam/types.h"
#include "qcam/usb.h"

namespace qcam {

struct CameraConfig {
    PixelFormat     format       = PixelFormat::Nv12;
    uint16_t        out_width    = 0;    // 0 => sensor native
    uint16_t        out_height   = 0;
    DemosaicQuality quality      = DemosaicQuality::Bilinear;
    ColorSettings   color;
    AutoExposureConfig auto_exposure;

    // Packet size to request from the isochronous endpoint. 0 => the sensor's
    // preferred size. Lowering it frees bus bandwidth at the cost of fps.
    uint16_t        iso_packet_size = 0;

    // Emit frames that arrived short rather than dropping them.
    bool            emit_short_frames = true;

    // How long to wait after enumeration before the first I2C transaction.
    // The ASIC needs a moment to settle; tests override this to zero.
    uint32_t        settle_ms = 250;
};

// A decoded, ready-to-present frame.
struct DecodedFrame {
    const uint8_t* data;
    size_t         size;
    PixelFormat    format;
    uint16_t       width;
    uint16_t       height;
    uint64_t       sequence;
    uint64_t       timestamp_100ns;
    bool           complete;         // false => short frame, tail padded grey
};

struct CameraStats {
    FramerStats framer;
    uint64_t    frames_delivered = 0;
    uint64_t    frames_dropped   = 0;   // handler could not keep up
    double      measured_fps     = 0.0;
    int         exposure         = 0;
    int         gain             = 0;
    int         luma             = 0;
};

// Thread safety: Start/Stop, the control setters and the accessors are safe
// to call from any thread while the camera is open. Open() and Close() are
// lifecycle calls and must come from a single thread - Stop() deliberately
// joins the streaming thread before taking any lock, which is only sound if
// nothing is concurrently tearing the device down underneath it.
class Camera {
public:
    using FrameHandler = std::function<void(const DecodedFrame&)>;

    Camera();
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // Takes ownership of an already-open transport. Probes the sensor and
    // runs the init sequence; the camera is idle but configured afterwards.
    Status Open(std::unique_ptr<IUsbTransport> transport, const CameraConfig& cfg);

    // Convenience: enumerate, pick the first known device, Open() it.
    Status OpenFirst(const CameraConfig& cfg);

    void Close();
    bool IsOpen() const { return sensor_ != nullptr; }

    Status Start(FrameHandler handler);
    Status Stop();
    bool   IsStreaming() const { return streaming_.load(); }

    // -- Controls ----------------------------------------------------------
    Status SetExposure(int value);   // 0..255
    Status SetGain(int value);       // 0..255
    Status SetAutoExposure(bool on);
    bool   auto_exposure() const;

    // Colour pipeline settings take effect on the next decoded frame.
    void SetColorSettings(const ColorSettings& c);
    ColorSettings color_settings() const;

    Status SetLed(bool on);

    // -- Introspection -----------------------------------------------------
    const char*   sensor_name() const;
    FrameGeometry sensor_geometry() const;
    uint16_t      out_width() const;
    uint16_t      out_height() const;
    PixelFormat   format() const { return cfg_.format; }
    double        nominal_fps() const;
    const UsbDeviceInfo& device_info() const;

    CameraStats  stats() const;

    // Dumps bridge and sensor registers through the log.
    Status DumpRegisters();

private:
    void OnRawFrame(const RawFrame& raw);
    Status ApplyAutoExposure(const RawFrame& raw);

    // Lock ordering, outermost first: mutex_ -> ctrl_mutex_ ->
    // {settings_mutex_, stats_mutex_}. Nothing may take them in any other
    // order, and no lock is ever held across IUsbTransport::StopIso(), which
    // joins the streaming thread.
    //
    // mutex_          device lifecycle and the member pointers below
    // ctrl_mutex_     register access (control transfers) and auto_exp_
    // settings_mutex_ staged colour settings handed to the streaming thread
    // stats_mutex_    counters
    mutable std::mutex             mutex_;
    mutable std::mutex             ctrl_mutex_;
    mutable std::mutex             settings_mutex_;
    std::unique_ptr<IUsbTransport> transport_;
    std::unique_ptr<Stv06xxBridge> bridge_;
    std::unique_ptr<ISensor>       sensor_;
    ChunkFramer                    framer_;
    Decoder                        decoder_;
    AutoExposure                   auto_exp_;
    CameraConfig                   cfg_;

    std::vector<uint8_t> out_buffer_;
    FrameHandler         handler_;
    std::atomic<bool>    streaming_{false};

    // Colour settings are owned by the streaming thread; callers stage a copy
    // here and the decoder picks it up at the next frame boundary.
    ColorSettings        pending_color_;
    std::atomic<bool>    color_dirty_{false};

    mutable std::mutex   stats_mutex_;
    CameraStats          stats_;
    uint64_t             fps_window_start_ns_ = 0;
    uint64_t             fps_window_frames_   = 0;
};

// Monotonic clock in 100 ns units, matching Media Foundation timestamps.
uint64_t NowIn100ns();

}  // namespace qcam

#endif  // QCAM_DEVICE_H_
