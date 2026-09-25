// SPDX-License-Identifier: GPL-2.0-or-later
//
// Isochronous chunk framer.
//
// The STV06xx wraps image data in a trivial chunk layer. Every isochronous
// packet holds an integral number of chunks; each chunk is a 4-byte header
// (16-bit big-endian id, 16-bit big-endian payload length) followed by its
// payload. The framer turns that stream back into whole Bayer frames.

#ifndef QCAM_FRAMER_H_
#define QCAM_FRAMER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "qcam/types.h"
#include "qcam/usb.h"

namespace qcam {

namespace chunk {

// Start of frame. The four encodings differ only in bits the ASIC uses for
// its own bookkeeping; all of them mean "discard what you have and begin".
constexpr uint16_t kSof0 = 0x8001;
constexpr uint16_t kSof1 = 0x8005;
constexpr uint16_t kSof2 = 0xc001;
constexpr uint16_t kSof3 = 0xc005;

// End of frame.
constexpr uint16_t kEof0 = 0x8002;
constexpr uint16_t kEof1 = 0x8006;
constexpr uint16_t kEof2 = 0xc002;

// Payload.
constexpr uint16_t kData0 = 0x0200;
constexpr uint16_t kData1 = 0x4200;

// Chunks that carry no image data and are safely ignored.
constexpr uint16_t kUnknown11Bytes = 0x0005;
constexpr uint16_t kUnknown2Bytes  = 0x0100;
constexpr uint16_t kSt6422Special  = 0x42ff;

constexpr size_t kHeaderLen = 4;

bool IsStartOfFrame(uint16_t id);
bool IsEndOfFrame(uint16_t id);
bool IsFrameData(uint16_t id, Bridge bridge);

}  // namespace chunk

struct FramerStats {
    uint64_t packets          = 0;
    uint64_t chunks           = 0;
    uint64_t frames_complete  = 0;
    uint64_t frames_short     = 0;  // EOF arrived before the frame filled
    uint64_t frames_overrun   = 0;  // more data than the geometry allows
    uint64_t frames_no_sof    = 0;  // data before any SOF; dropped
    uint64_t unknown_chunks   = 0;
    uint64_t truncated_chunks = 0;  // header claimed more than the packet held
    uint64_t bytes_dropped    = 0;
};

class ChunkFramer : public IIsoSink {
public:
    using FrameHandler = std::function<void(const RawFrame&)>;

    ChunkFramer();

    // Geometry must be set before the first packet; it sizes the accumulator.
    void Configure(const FrameGeometry& geom, Bridge bridge);
    void SetFrameHandler(FrameHandler handler) { handler_ = std::move(handler); }

    // The ST6422 emits four lines of garbage at the top of every frame; the
    // framer skips that many bytes after each SOF.
    void SetLeadingSkip(size_t bytes) { configured_skip_ = bytes; }

    // Emit frames that ended early, padded with mid-grey, instead of dropping
    // them. Useful when bandwidth is tight and you would rather have a
    // partial picture than a stall.
    void SetEmitShortFrames(bool on) { emit_short_ = on; }

    // IIsoSink
    void OnIsoPacket(const uint8_t* data, size_t len) override;
    void OnIsoError(Status status) override;

    void Reset();

    const FramerStats& stats() const { return stats_; }
    const FrameGeometry& geometry() const { return geom_; }

    // Exposed for tests: parse one packet without any device attached.
    void FeedPacket(const uint8_t* data, size_t len) { OnIsoPacket(data, len); }

private:
    void BeginFrame();
    void EndFrame();
    void AppendData(const uint8_t* data, size_t len);

    FrameGeometry        geom_{0, 0, BayerPhase::GRBG};
    Bridge               bridge_ = Bridge::Stv0600;
    std::vector<uint8_t> accum_;
    size_t               filled_          = 0;
    size_t               configured_skip_ = 0;
    size_t               skip_remaining_  = 0;
    bool                 in_frame_        = false;
    bool                 overran_         = false;
    bool                 emit_short_      = true;
    uint64_t             sequence_        = 0;
    FramerStats          stats_;
    FrameHandler         handler_;
};

}  // namespace qcam

#endif  // QCAM_FRAMER_H_
