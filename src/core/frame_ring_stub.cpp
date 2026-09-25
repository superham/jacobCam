// SPDX-License-Identifier: GPL-2.0-or-later
//
// Non-Windows stand-in for the shared-memory frame ring, so qcamctl and the
// core library build and link on a developer machine. The real implementation
// is src/win/frame_ring.cpp.

#include "qcam/ring.h"

#include "qcam/log.h"

namespace qcam {

struct FrameRingWriter::Impl {};
struct FrameRingReader::Impl {};

FrameRingWriter::FrameRingWriter() = default;
FrameRingWriter::~FrameRingWriter() = default;
bool FrameRingWriter::IsOpen() const { return false; }

Status FrameRingWriter::Create(const RingConfig&) {
    QCAM_LOGE("the frame ring is only implemented on Windows");
    return Status::Unsupported;
}
void FrameRingWriter::Close() {}
Status FrameRingWriter::Publish(const uint8_t*, size_t, uint64_t, uint64_t) {
    return Status::Unsupported;
}

struct FrameDemand::Impl {};

FrameDemand::FrameDemand() = default;
FrameDemand::~FrameDemand() = default;
Status FrameDemand::Create() { return Status::Unsupported; }
void FrameDemand::Close() {}
void* FrameDemand::wait_handle() const { return nullptr; }

FrameRingReader::FrameRingReader() = default;
FrameRingReader::~FrameRingReader() = default;
bool FrameRingReader::IsOpen() const { return false; }

Status FrameRingReader::Open() {
    QCAM_LOGE("the frame ring is only implemented on Windows");
    return Status::Unsupported;
}
void FrameRingReader::Close() {}
Status FrameRingReader::GetConfig(RingConfig*) const { return Status::Unsupported; }
Status FrameRingReader::Read(std::vector<uint8_t>*, FrameMeta*, uint32_t) {
    return Status::Unsupported;
}

struct PictureControlHost::Impl { PictureControls current; };

PictureControlHost::PictureControlHost() : impl_(new Impl()) {}
PictureControlHost::~PictureControlHost() = default;
Status PictureControlHost::Create(const PictureControls& initial) {
    impl_->current = initial;
    return Status::Unsupported;
}
void PictureControlHost::Close() {}
PictureControls PictureControlHost::Current() const { return impl_->current; }
bool PictureControlHost::Poll(PictureControls*) { return false; }

struct PictureControlClient::Impl {};

PictureControlClient::PictureControlClient() = default;
PictureControlClient::~PictureControlClient() = default;
Status PictureControlClient::Open() { return Status::Unsupported; }
bool PictureControlClient::IsOpen() const { return false; }
PictureControls PictureControlClient::Get() const { return PictureControls{}; }
Status PictureControlClient::Set(const PictureControls&) { return Status::Unsupported; }

}  // namespace qcam
