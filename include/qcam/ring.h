// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared-memory frame ring.
//
// qcamsvc owns the USB device and is the only writer. Readers are the Media
// Foundation virtual camera (loaded into the Frame Server process) and
// qcamctl when it attaches to a running service.
//
// Each slot is protected by a seqlock rather than a mutex: the writer must
// never be able to stall because a reader was suspended mid-frame, and a
// reader that loses a race can simply retry or skip to the next frame. A
// dropped frame on a 7.5 fps camera is far better than a stalled capture
// pipeline inside the Frame Server.

#ifndef QCAM_RING_H_
#define QCAM_RING_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "qcam/decode.h"
#include "qcam/types.h"

namespace qcam {

constexpr uint32_t kRingMagic   = 0x4d434351;  // 'QCCM'
constexpr uint32_t kRingVersion = 1;
constexpr uint32_t kRingSlots   = 4;

// Layout is shared across process boundaries, so everything here is a
// fixed-size type with explicit alignment and no virtuals.
struct RingSlotHeader {
    std::atomic<uint32_t> version;   // even = stable, odd = being written
    uint32_t              size;      // payload bytes actually valid
    uint64_t              sequence;
    uint64_t              timestamp_100ns;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "the ring needs lock-free atomics to be safe across processes");

struct RingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_bytes;      // payload capacity of each slot

    uint32_t width;
    uint32_t height;
    uint32_t format;          // PixelFormat
    uint32_t fps_numerator;
    uint32_t fps_denominator;
    uint32_t reserved0;

    std::atomic<uint64_t> write_sequence;   // frames published so far
    std::atomic<uint32_t> writer_alive;
    uint32_t reserved1;
};

struct FrameMeta {
    uint64_t    sequence        = 0;
    uint64_t    timestamp_100ns = 0;
    uint32_t    width           = 0;
    uint32_t    height          = 0;
    PixelFormat format          = PixelFormat::Nv12;
};

struct RingConfig {
    uint32_t    width  = 0;
    uint32_t    height = 0;
    PixelFormat format = PixelFormat::Nv12;
    uint32_t    fps_numerator   = 15;
    uint32_t    fps_denominator = 2;   // 7.5 fps
};

// Writer side; lives in qcamsvc.
class FrameRingWriter {
public:
    FrameRingWriter();
    ~FrameRingWriter();

    FrameRingWriter(const FrameRingWriter&) = delete;
    FrameRingWriter& operator=(const FrameRingWriter&) = delete;

    Status Create(const RingConfig& config);
    void   Close();
    bool   IsOpen() const;

    // Copies one frame into the next slot and signals waiting readers.
    Status Publish(const uint8_t* data, size_t size, uint64_t sequence,
                   uint64_t timestamp_100ns);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Service side of on-demand streaming. Readers signal it every time they open
// the ring or wait for a frame, so the service can leave the camera closed
// until something actually wants frames, and close it again once nothing has
// asked for a while.
class FrameDemand {
public:
    FrameDemand();
    ~FrameDemand();

    FrameDemand(const FrameDemand&) = delete;
    FrameDemand& operator=(const FrameDemand&) = delete;

    Status Create();
    void   Close();

    // The auto-reset event readers signal: a HANDLE on Windows, null
    // elsewhere or before Create() succeeds.
    void*  wait_handle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reader side; lives in the virtual camera and in qcamctl. Open() and Read()
// both tell the service a reader wants frames (see FrameDemand), so Open()
// failing with NoDevice may simply mean the camera is still starting.
class FrameRingReader {
public:
    FrameRingReader();
    ~FrameRingReader();

    FrameRingReader(const FrameRingReader&) = delete;
    FrameRingReader& operator=(const FrameRingReader&) = delete;

    Status Open();
    void   Close();
    bool   IsOpen() const;

    // Format the writer is publishing. Valid once Open() succeeds.
    Status GetConfig(RingConfig* config) const;

    // Waits for a frame newer than the last one this reader returned and
    // copies it into `out`. Returns Timeout if none arrived in time, and
    // NoDevice if the writer went away.
    Status Read(std::vector<uint8_t>* out, FrameMeta* meta, uint32_t timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Picture controls apps change through the virtual camera, in the units of
// the standard VideoProcAmp properties they arrive as. The defaults match
// ColorSettings' defaults.
struct PictureControls {
    int32_t brightness = 0;    // -100 .. 100
    int32_t contrast   = 100;  // percent, 20 .. 200
    int32_t saturation = 115;  // percent, 0 .. 200
    int32_t gamma      = 65;   // gamma x 100, 20 .. 300
};

// Maps the controls onto the decoder's colour settings, leaving everything
// else (white balance, flips) as it was.
inline void ApplyPictureControls(const PictureControls& p, ColorSettings* c) {
    // Full slider travel is half the decoder's range: +-1.0 there is solid
    // white or black, which no one wants from a brightness slider.
    c->brightness = static_cast<float>(p.brightness) / 200.0f;
    c->contrast   = static_cast<float>(p.contrast) / 100.0f;
    c->saturation = static_cast<float>(p.saturation) / 100.0f;
    c->gamma      = static_cast<float>(p.gamma) / 100.0f;
}

constexpr uint32_t kControlMagic   = 0x4c544351;  // 'QCTL'
constexpr uint32_t kControlVersion = 1;

struct ControlBlock {
    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> generation;   // bumped after every change
    std::atomic<int32_t>  brightness;
    std::atomic<int32_t>  contrast;
    std::atomic<int32_t>  saturation;
    std::atomic<int32_t>  gamma;
};

// Service side of the picture controls. Created once for the service's
// lifetime, not per stream, so a setting survives the camera being closed
// and reopened between apps.
class PictureControlHost {
public:
    PictureControlHost();
    ~PictureControlHost();

    PictureControlHost(const PictureControlHost&) = delete;
    PictureControlHost& operator=(const PictureControlHost&) = delete;

    Status Create(const PictureControls& initial);
    void   Close();

    PictureControls Current() const;
    // True, with the new values, when a client changed them since the last
    // call. Cheap enough to call on every frame.
    bool Poll(PictureControls* out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Virtual camera side: reads the current values for apps' settings panels
// and writes their changes.
class PictureControlClient {
public:
    PictureControlClient();
    ~PictureControlClient();

    PictureControlClient(const PictureControlClient&) = delete;
    PictureControlClient& operator=(const PictureControlClient&) = delete;

    // NoDevice when the service is not running.
    Status Open();
    bool   IsOpen() const;

    PictureControls Get() const;
    Status Set(const PictureControls& controls);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qcam

#endif  // QCAM_RING_H_
