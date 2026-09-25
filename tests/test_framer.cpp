// SPDX-License-Identifier: GPL-2.0-or-later
//
// Chunk framer tests.

#include "test_harness.h"

#include <vector>

#include "qcam/framer.h"
#include "qcam/mock.h"

using namespace qcam;

namespace {

constexpr uint16_t kW = 8;
constexpr uint16_t kH = 4;
constexpr size_t   kFrameBytes = kW * kH;

FrameGeometry Geom() { return FrameGeometry{kW, kH, BayerPhase::GRBG}; }

struct Capture {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<bool>                 complete;

    ChunkFramer::FrameHandler Handler() {
        return [this](const RawFrame& f) {
            frames.emplace_back(f.data, f.data + f.size);
            complete.push_back(f.complete);
        };
    }
};

std::vector<uint8_t> Ramp(size_t n, uint8_t start = 0) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(start + i);
    return v;
}

}  // namespace

TEST(FramerAssemblesSingleFrameAcrossPackets) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const std::vector<uint8_t> payload = Ramp(kFrameBytes);
    for (const auto& packet : BuildFramePackets(payload, 7))
        framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    CHECK_EQ(cap.complete[0], true);
    CHECK(cap.frames[0] == payload);
    CHECK_EQ(framer.stats().frames_complete, uint64_t{1});
}

TEST(FramerHandlesSeveralChunksInOnePacket) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    // A real packet carries SOF, data and EOF back to back when the frame is
    // small enough to fit.
    const std::vector<uint8_t> payload = Ramp(kFrameBytes, 1);
    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, payload},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    CHECK(cap.frames[0] == payload);
}

TEST(FramerAcceptsEveryStartAndEndEncoding) {
    const uint16_t sofs[] = {chunk::kSof0, chunk::kSof1, chunk::kSof2, chunk::kSof3};
    const uint16_t eofs[] = {chunk::kEof0, chunk::kEof1, chunk::kEof2};

    for (uint16_t sof : sofs) {
        for (uint16_t eof : eofs) {
            ChunkFramer framer;
            Capture cap;
            framer.Configure(Geom(), Bridge::Stv0600);
            framer.SetFrameHandler(cap.Handler());

            const auto packet = BuildIsoPacket({
                {sof, {}},
                {chunk::kData0, Ramp(kFrameBytes)},
                {eof, {}},
            });
            framer.FeedPacket(packet.data(), packet.size());
            CHECK_EQ(cap.frames.size(), size_t{1});
        }
    }
}

TEST(FramerAcceptsBothDataChunkIds) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(16)},
        {chunk::kData1, Ramp(16, 16)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    CHECK_EQ(int{cap.frames[0][0]}, 0);
    CHECK_EQ(int{cap.frames[0][16]}, 16);
}

TEST(FramerDropsDataArrivingBeforeFirstStartOfFrame) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    // Joining a running stream mid-frame is the normal case at startup.
    const auto orphan = BuildIsoPacket({{chunk::kData0, Ramp(16)}});
    framer.FeedPacket(orphan.data(), orphan.size());
    CHECK_EQ(cap.frames.size(), size_t{0});
    CHECK(framer.stats().frames_no_sof > 0);

    // The next clean frame still comes through.
    for (const auto& p : BuildFramePackets(Ramp(kFrameBytes), 32))
        framer.FeedPacket(p.data(), p.size());
    CHECK_EQ(cap.frames.size(), size_t{1});
}

TEST(FramerPadsShortFrameWhenAsked) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetEmitShortFrames(true);
    framer.SetFrameHandler(cap.Handler());

    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(10)},   // frame wants 32 bytes
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    CHECK_EQ(cap.complete[0], false);
    CHECK_EQ(cap.frames[0].size(), kFrameBytes);
    CHECK_EQ(int{cap.frames[0][9]}, 9);
    CHECK_EQ(int{cap.frames[0][10]}, 0x80);   // padded with mid-grey
    CHECK_EQ(framer.stats().frames_short, uint64_t{1});
}

TEST(FramerDropsShortFrameWhenConfiguredTo) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetEmitShortFrames(false);
    framer.SetFrameHandler(cap.Handler());

    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(10)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{0});
    CHECK_EQ(framer.stats().frames_short, uint64_t{1});
}

TEST(FramerDropsOverrunFrame) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(kFrameBytes + 12)},  // more than the geometry
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{0});
    CHECK_EQ(framer.stats().frames_overrun, uint64_t{1});
    CHECK_EQ(framer.stats().bytes_dropped, uint64_t{12});
}

TEST(FramerRecoversAfterOverrunFrame) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const auto bad = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(kFrameBytes + 12)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(bad.data(), bad.size());

    const auto good = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(kFrameBytes)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(good.data(), good.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    CHECK_EQ(cap.complete[0], true);
    CHECK_EQ(framer.stats().frames_overrun, uint64_t{1});
}

TEST(FramerRejectsChunkLongerThanItsPacket) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    // Hand-build a header claiming 0x0100 bytes with nothing behind it.
    const uint8_t bad[] = {0x02, 0x00, 0x01, 0x00, 0xaa, 0xbb};
    framer.FeedPacket(bad, sizeof(bad));

    CHECK_EQ(cap.frames.size(), size_t{0});
    CHECK_EQ(framer.stats().truncated_chunks, uint64_t{1});
}

TEST(FramerIgnoresTrailingStubTooSmallForAHeader) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const uint8_t stub[] = {0x02, 0x00};
    framer.FeedPacket(stub, sizeof(stub));
    CHECK_EQ(framer.stats().truncated_chunks, uint64_t{1});
}

TEST(FramerSkipsKnownBenignChunks) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kUnknown2Bytes, {0xde, 0xad}},
        {chunk::kData0, Ramp(kFrameBytes)},
        {chunk::kUnknown11Bytes, std::vector<uint8_t>(11, 0)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    // Benign chunks must not be counted as unknown, or the log fills up.
    CHECK_EQ(framer.stats().unknown_chunks, uint64_t{0});
    if (cap.frames.empty()) return;
    CHECK(cap.frames[0] == Ramp(kFrameBytes));
}

TEST(FramerCountsGenuinelyUnknownChunks) {
    ChunkFramer framer;
    framer.Configure(Geom(), Bridge::Stv0600);

    const auto packet = BuildIsoPacket({{0x1234, {1, 2, 3}}});
    framer.FeedPacket(packet.data(), packet.size());
    CHECK_EQ(framer.stats().unknown_chunks, uint64_t{1});
}

TEST(FramerSt6422TreatsAnyLowByteAsData) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::St6422);
    framer.SetFrameHandler(cap.Handler());

    // The ST6422 varies the low byte of its data chunk id.
    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {0x0207, Ramp(kFrameBytes)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());
    CHECK_EQ(cap.frames.size(), size_t{1});
}

TEST(FramerSt6422LowByteIsNotDataOnStv0600) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {0x0207, Ramp(kFrameBytes)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    // Nothing accumulated, so the frame is entirely padding.
    CHECK_EQ(framer.stats().unknown_chunks, uint64_t{1});
    CHECK_EQ(framer.stats().frames_short, uint64_t{1});
}

TEST(FramerAppliesLeadingSkip) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::St6422);
    framer.SetLeadingSkip(kW * 2);   // two rows of garbage
    framer.SetFrameHandler(cap.Handler());

    const auto packets = BuildFramePackets(Ramp(kFrameBytes + kW * 2), 9);
    for (const auto& p : packets) framer.FeedPacket(p.data(), p.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    // The first kept byte is the one just past the skipped rows.
    CHECK_EQ(int{cap.frames[0][0]}, kW * 2);
    CHECK_EQ(cap.complete[0], true);
}

TEST(FramerSequenceNumbersIncrease) {
    ChunkFramer framer;
    std::vector<uint64_t> seqs;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler([&](const RawFrame& f) { seqs.push_back(f.sequence); });

    for (int i = 0; i < 3; ++i)
        for (const auto& p : BuildFramePackets(Ramp(kFrameBytes), 32))
            framer.FeedPacket(p.data(), p.size());

    CHECK_EQ(seqs.size(), size_t{3});
    if (seqs.size() == 3) {
        CHECK(seqs[1] == seqs[0] + 1);
        CHECK(seqs[2] == seqs[1] + 1);
    }
}

TEST(FramerNewStartOfFrameAbandonsPartialFrame) {
    ChunkFramer framer;
    Capture cap;
    framer.Configure(Geom(), Bridge::Stv0600);
    framer.SetFrameHandler(cap.Handler());

    // A SOF with no intervening EOF means the previous frame was lost.
    const auto packet = BuildIsoPacket({
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(12)},
        {chunk::kSof0, {}},
        {chunk::kData0, Ramp(kFrameBytes, 100)},
        {chunk::kEof0, {}},
    });
    framer.FeedPacket(packet.data(), packet.size());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;
    CHECK_EQ(int{cap.frames[0][0]}, 100);
}
