// SPDX-License-Identifier: GPL-2.0-or-later
//
// Isochronous chunk framer. See docs/protocol.md for the chunk layout.

#include "qcam/framer.h"

#include <cstring>

#include "qcam/device.h"
#include "qcam/log.h"

namespace qcam {

namespace chunk {

bool IsStartOfFrame(uint16_t id) {
    return id == kSof0 || id == kSof1 || id == kSof2 || id == kSof3;
}

bool IsEndOfFrame(uint16_t id) {
    return id == kEof0 || id == kEof1 || id == kEof2;
}

bool IsFrameData(uint16_t id, Bridge bridge) {
    if (id == kData0 || id == kData1) return true;
    // The ST6422 varies the low byte of the data chunk id per packet; only
    // the high byte is meaningful there.
    if (bridge == Bridge::St6422 && (id & 0xff00) == 0x0200) return true;
    return false;
}

}  // namespace chunk

ChunkFramer::ChunkFramer() = default;

void ChunkFramer::Configure(const FrameGeometry& geom, Bridge bridge) {
    geom_   = geom;
    bridge_ = bridge;
    accum_.assign(geom.RawSize(), 0);
    Reset();
}

void ChunkFramer::Reset() {
    filled_         = 0;
    in_frame_       = false;
    overran_        = false;
    skip_remaining_ = 0;
    stats_          = FramerStats{};
}

void ChunkFramer::BeginFrame() {
    filled_         = 0;
    in_frame_       = true;
    overran_        = false;
    skip_remaining_ = configured_skip_;
}

void ChunkFramer::AppendData(const uint8_t* data, size_t len) {
    if (!in_frame_) {
        // Data before the first SOF. Normal at stream start: we join the
        // stream mid-frame and wait for a clean boundary.
        stats_.frames_no_sof++;
        stats_.bytes_dropped += len;
        return;
    }

    if (skip_remaining_) {
        const size_t skip = (skip_remaining_ < len) ? skip_remaining_ : len;
        data += skip;
        len  -= skip;
        skip_remaining_ -= skip;
        if (len == 0) return;
    }

    const size_t room = accum_.size() - filled_;
    if (len > room) {
        // More data than the geometry allows. On real hardware this happens
        // when exposure is reprogrammed mid-frame, and the extra bytes are not
        // just a tail: clipping yields a frame shifted by an odd byte count,
        // which shows as a vertical seam with swapped Bayer colours. EndFrame
        // drops it, as gspca does.
        if (!overran_) stats_.frames_overrun++;
        overran_ = true;
        stats_.bytes_dropped += len - room;
        len = room;
    }
    if (len) {
        std::memcpy(accum_.data() + filled_, data, len);
        filled_ += len;
    }
}

void ChunkFramer::EndFrame() {
    if (!in_frame_) return;
    in_frame_ = false;

    const size_t want = accum_.size();

    if (overran_) {
        QCAM_LOGD("framer: dropping overrun frame");
        overran_ = false;
        filled_  = 0;
        return;
    }

    const bool complete = (filled_ == want);

    if (!complete) {
        stats_.frames_short++;
        if (!emit_short_) {
            QCAM_LOGD("framer: dropping short frame (%zu/%zu bytes)", filled_, want);
            filled_ = 0;
            return;
        }
        // Pad the tail with mid-grey so a dropped tail reads as a grey band
        // rather than as whatever the previous frame left behind.
        std::memset(accum_.data() + filled_, 0x80, want - filled_);
    } else {
        stats_.frames_complete++;
    }

    if (handler_) {
        RawFrame frame;
        frame.data            = accum_.data();
        frame.size            = want;
        frame.sequence        = sequence_++;
        frame.timestamp_100ns = NowIn100ns();
        frame.complete        = complete;
        handler_(frame);
    }
    filled_ = 0;
}

void ChunkFramer::OnIsoPacket(const uint8_t* data, size_t len) {
    if (accum_.empty()) return;  // not configured yet
    stats_.packets++;

    // A packet holds an integral number of chunks; walk them in order.
    while (len) {
        if (len < chunk::kHeaderLen) {
            // A trailing stub is not something the ASIC should emit.
            stats_.truncated_chunks++;
            stats_.bytes_dropped += len;
            return;
        }

        const uint16_t id  = static_cast<uint16_t>((data[0] << 8) | data[1]);
        size_t chunk_len   = static_cast<size_t>((data[2] << 8) | data[3]);

        data += chunk::kHeaderLen;
        len  -= chunk::kHeaderLen;
        stats_.chunks++;

        if (chunk_len > len) {
            // Header claims more payload than the packet carries. The frame in
            // flight is no longer trustworthy.
            QCAM_LOGD("framer: chunk 0x%04x claims %zu bytes, packet has %zu",
                      id, chunk_len, len);
            stats_.truncated_chunks++;
            stats_.bytes_dropped += len;
            in_frame_ = false;
            filled_   = 0;
            return;
        }

        if (chunk::IsFrameData(id, bridge_)) {
            AppendData(data, chunk_len);
        } else if (chunk::IsStartOfFrame(id)) {
            if (chunk_len)
                QCAM_LOGD("framer: SOF 0x%04x carried %zu bytes", id, chunk_len);
            BeginFrame();
        } else if (chunk::IsEndOfFrame(id)) {
            if (chunk_len)
                QCAM_LOGD("framer: EOF 0x%04x carried %zu bytes", id, chunk_len);
            EndFrame();
        } else {
            switch (id) {
                case chunk::kUnknown11Bytes:
                case chunk::kUnknown2Bytes:
                case chunk::kSt6422Special:
                    // Known-benign, seen in compressed mode and on the ST6422.
                    break;
                default:
                    stats_.unknown_chunks++;
                    QCAM_LOGT("framer: unknown chunk 0x%04x (%zu bytes)", id,
                              chunk_len);
                    break;
            }
        }

        data += chunk_len;
        len  -= chunk_len;
    }
}

void ChunkFramer::OnIsoError(Status status) {
    QCAM_LOGW("framer: isochronous stream error: %s", StatusName(status));
    in_frame_ = false;
    filled_   = 0;
}

}  // namespace qcam
