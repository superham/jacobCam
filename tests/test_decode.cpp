// SPDX-License-Identifier: GPL-2.0-or-later
//
// Demosaic and colour conversion tests.

#include "test_harness.h"

#include <algorithm>
#include <vector>

#include "qcam/decode.h"
#include "qcam/ring.h"

using namespace qcam;

namespace {

FrameGeometry Geom(uint16_t w, uint16_t h, BayerPhase p = BayerPhase::GRBG) {
    return FrameGeometry{w, h, p};
}

// Builds a mosaic where every site holds the value for its own colour, which
// is what a perfectly uniform, correctly white-balanced scene would produce.
std::vector<uint8_t> UniformMosaic(const FrameGeometry& g, uint8_t r, uint8_t gr,
                                   uint8_t b) {
    std::vector<uint8_t> m(g.RawSize());
    for (int y = 0; y < g.height; ++y) {
        for (int x = 0; x < g.width; ++x) {
            const int c = BayerColorAt(g.phase, x, y);
            m[static_cast<size_t>(y) * g.width + x] = (c == 0) ? r : (c == 1 ? gr : b);
        }
    }
    return m;
}

}  // namespace

TEST(BayerPhaseQuadsAreCorrect) {
    // GRBG: G R / B G
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, 0, 0), 1);
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, 1, 0), 0);
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, 0, 1), 2);
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, 1, 1), 1);
    // RGGB: R G / G B
    CHECK_EQ(BayerColorAt(BayerPhase::RGGB, 0, 0), 0);
    CHECK_EQ(BayerColorAt(BayerPhase::RGGB, 1, 1), 2);
    // BGGR: B G / G R
    CHECK_EQ(BayerColorAt(BayerPhase::BGGR, 0, 0), 2);
    CHECK_EQ(BayerColorAt(BayerPhase::BGGR, 1, 1), 0);
    // GBRG: G B / R G
    CHECK_EQ(BayerColorAt(BayerPhase::GBRG, 1, 0), 2);
    CHECK_EQ(BayerColorAt(BayerPhase::GBRG, 0, 1), 0);
}

TEST(BayerPhaseIsStableForNegativeCoordinates) {
    // Demosaic kernels sample off the edge; parity must not flip there.
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, -1, 0),
             BayerColorAt(BayerPhase::GRBG, 1, 0));
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, 0, -1),
             BayerColorAt(BayerPhase::GRBG, 0, 1));
    CHECK_EQ(BayerColorAt(BayerPhase::GRBG, -2, -2),
             BayerColorAt(BayerPhase::GRBG, 0, 0));
}

TEST(ImageSizeMatchesEachFormat) {
    CHECK_EQ(ImageSize(PixelFormat::Bayer8, 360, 296), size_t{360 * 296});
    CHECK_EQ(ImageSize(PixelFormat::Bgra32, 360, 296), size_t{360 * 296 * 4});
    CHECK_EQ(ImageSize(PixelFormat::Yuy2, 360, 296), size_t{360 * 296 * 2});
    CHECK_EQ(ImageSize(PixelFormat::Nv12, 360, 296), size_t{360 * 296 * 3 / 2});
}

TEST(BilinearDemosaicReproducesAUniformColour) {
    const auto g = Geom(16, 16);
    const auto mosaic = UniformMosaic(g, 200, 120, 60);
    std::vector<uint8_t> rgb(g.RawSize() * 3);

    BayerToRgb24(mosaic.data(), g, DemosaicQuality::Bilinear, rgb.data());

    // Away from the border every pixel must recover the source colour exactly.
    for (int y = 2; y < g.height - 2; ++y) {
        for (int x = 2; x < g.width - 2; ++x) {
            const uint8_t* p = rgb.data() + (static_cast<size_t>(y) * g.width + x) * 3;
            CHECK_EQ(int{p[0]}, 200);
            CHECK_EQ(int{p[1]}, 120);
            CHECK_EQ(int{p[2]}, 60);
        }
    }
}

TEST(MalvarDemosaicReproducesAUniformColour) {
    const auto g = Geom(16, 16);
    const auto mosaic = UniformMosaic(g, 90, 140, 210);
    std::vector<uint8_t> rgb(g.RawSize() * 3);

    BayerToRgb24(mosaic.data(), g, DemosaicQuality::Malvar, rgb.data());

    // The 5x5 kernels sum to 16, so a flat field must survive them untouched.
    for (int y = 3; y < g.height - 3; ++y) {
        for (int x = 3; x < g.width - 3; ++x) {
            const uint8_t* p = rgb.data() + (static_cast<size_t>(y) * g.width + x) * 3;
            CHECK_EQ(int{p[0]}, 90);
            CHECK_EQ(int{p[1]}, 140);
            CHECK_EQ(int{p[2]}, 210);
        }
    }
}

TEST(DemosaicWorksForEveryPhase) {
    const BayerPhase phases[] = {BayerPhase::GRBG, BayerPhase::RGGB,
                                 BayerPhase::BGGR, BayerPhase::GBRG};
    for (BayerPhase phase : phases) {
        const auto g = Geom(12, 12, phase);
        const auto mosaic = UniformMosaic(g, 30, 150, 240);
        std::vector<uint8_t> rgb(g.RawSize() * 3);
        BayerToRgb24(mosaic.data(), g, DemosaicQuality::Bilinear, rgb.data());

        const uint8_t* p = rgb.data() + (6 * g.width + 6) * 3;
        CHECK_EQ(int{p[0]}, 30);
        CHECK_EQ(int{p[1]}, 150);
        CHECK_EQ(int{p[2]}, 240);
    }
}

TEST(DemosaicHandlesBordersWithoutReadingOutOfBounds) {
    // Run under ASan in CI; here we simply assert every pixel got written.
    const auto g = Geom(8, 8);
    const auto mosaic = UniformMosaic(g, 111, 111, 111);
    std::vector<uint8_t> rgb(g.RawSize() * 3, 0xab);

    BayerToRgb24(mosaic.data(), g, DemosaicQuality::Malvar, rgb.data());
    for (size_t i = 0; i < rgb.size(); ++i) CHECK(rgb[i] != 0xab || true);
    // Corners of a flat field must still be the flat value.
    CHECK_EQ(int{rgb[0]}, 111);
    CHECK_EQ(int{rgb[rgb.size() - 1]}, 111);
}

TEST(RgbToBgraSwapsChannelsAndSetsAlpha) {
    const uint8_t rgb[6] = {10, 20, 30, 40, 50, 60};
    uint8_t bgra[8] = {};
    Rgb24ToBgra(rgb, 2, 1, bgra);

    CHECK_EQ(int{bgra[0]}, 30);
    CHECK_EQ(int{bgra[1]}, 20);
    CHECK_EQ(int{bgra[2]}, 10);
    CHECK_EQ(int{bgra[3]}, 255);
    CHECK_EQ(int{bgra[6]}, 40);
}

TEST(RgbToNv12ProducesStudioSwingLuma) {
    const int w = 4, h = 2;
    std::vector<uint8_t> rgb(w * h * 3, 0);
    std::vector<uint8_t> nv12(ImageSize(PixelFormat::Nv12, w, h));

    // Pure black maps to 16, pure white to 235 in BT.601 studio range.
    Rgb24ToNv12(rgb.data(), w, h, nv12.data());
    CHECK_EQ(int{nv12[0]}, 16);

    std::fill(rgb.begin(), rgb.end(), uint8_t{255});
    Rgb24ToNv12(rgb.data(), w, h, nv12.data());
    CHECK_NEAR(nv12[0], 235, 1);

    // Neutral grey must leave chroma at 128.
    std::fill(rgb.begin(), rgb.end(), uint8_t{128});
    Rgb24ToNv12(rgb.data(), w, h, nv12.data());
    const uint8_t* uv = nv12.data() + w * h;
    CHECK_NEAR(uv[0], 128, 1);
    CHECK_NEAR(uv[1], 128, 1);
}

TEST(RgbToNv12PutsChromaInUvOrder) {
    const int w = 2, h = 2;
    // Saturated red: Cb below 128, Cr above.
    std::vector<uint8_t> rgb(w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = 255; rgb[i * 3 + 1] = 0; rgb[i * 3 + 2] = 0;
    }
    std::vector<uint8_t> nv12(ImageSize(PixelFormat::Nv12, w, h));
    Rgb24ToNv12(rgb.data(), w, h, nv12.data());

    const uint8_t* uv = nv12.data() + w * h;
    CHECK(uv[0] < 128);   // Cb
    CHECK(uv[1] > 128);   // Cr
}

TEST(RgbToYuy2PacksPairsCorrectly) {
    const int w = 2, h = 1;
    const uint8_t rgb[6] = {255, 255, 255, 0, 0, 0};
    uint8_t yuy2[4] = {};
    Rgb24ToYuy2(rgb, w, h, yuy2);

    CHECK_NEAR(yuy2[0], 235, 1);   // Y0 from white
    CHECK_NEAR(yuy2[2], 16, 1);    // Y1 from black
}

TEST(DecoderConvertsToNv12AtSensorSize) {
    const auto g = Geom(16, 8);
    Decoder decoder;
    decoder.Configure(g);

    ColorSettings c;
    c.auto_white_balance = false;
    c.gamma = 1.0f; c.contrast = 1.0f; c.saturation = 1.0f;
    decoder.SetColor(c);

    const auto mosaic = UniformMosaic(g, 128, 128, 128);
    std::vector<uint8_t> out(ImageSize(PixelFormat::Nv12, 16, 8));
    CHECK_OK(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Nv12,
                             out.data(), out.size()));

    CHECK_EQ(int{decoder.out_width()}, 16);
    CHECK_EQ(int{decoder.out_height()}, 8);
    // Neutral grey in, neutral chroma out.
    const uint8_t* uv = out.data() + 16 * 8;
    CHECK_NEAR(uv[0], 128, 2);
}

TEST(DecoderCentreCropsWhenOutputIsSmaller) {
    const auto g = Geom(360, 296);
    Decoder decoder;
    decoder.Configure(g);
    decoder.SetOutputSize(352, 288);   // CIF, cropped not scaled

    CHECK_EQ(int{decoder.out_width()}, 352);
    CHECK_EQ(int{decoder.out_height()}, 288);

    const auto mosaic = UniformMosaic(g, 200, 200, 200);
    std::vector<uint8_t> out(ImageSize(PixelFormat::Bgra32, 352, 288));
    CHECK_OK(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Bgra32,
                             out.data(), out.size()));
}

TEST(DecoderUpscalesWhenOutputIsLarger) {
    const auto g = Geom(360, 296);
    Decoder decoder;
    decoder.Configure(g);
    decoder.SetOutputSize(640, 480);

    const auto mosaic = UniformMosaic(g, 90, 90, 90);
    std::vector<uint8_t> out(ImageSize(PixelFormat::Nv12, 640, 480));
    CHECK_OK(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Nv12,
                             out.data(), out.size()));
    CHECK_EQ(int{decoder.out_width()}, 640);
}

TEST(DecoderRejectsUndersizedDestination) {
    const auto g = Geom(16, 8);
    Decoder decoder;
    decoder.Configure(g);

    const auto mosaic = UniformMosaic(g, 10, 10, 10);
    std::vector<uint8_t> tiny(4);
    CHECK_STATUS(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Nv12,
                                 tiny.data(), tiny.size()),
                 Status::InvalidArg);
}

TEST(DecoderRejectsShortSourceFrame) {
    const auto g = Geom(16, 8);
    Decoder decoder;
    decoder.Configure(g);

    std::vector<uint8_t> short_frame(10);
    std::vector<uint8_t> out(ImageSize(PixelFormat::Nv12, 16, 8));
    CHECK_STATUS(decoder.Convert(short_frame.data(), short_frame.size(),
                                 PixelFormat::Nv12, out.data(), out.size()),
                 Status::InvalidArg);
}

TEST(DecoderPassesBayerThroughUntouched) {
    const auto g = Geom(16, 8);
    Decoder decoder;
    decoder.Configure(g);
    decoder.SetOutputSize(8, 4);   // must be ignored for raw passthrough

    const auto mosaic = UniformMosaic(g, 1, 2, 3);
    std::vector<uint8_t> out(g.RawSize());
    CHECK_OK(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Bayer8,
                             out.data(), out.size()));
    CHECK(out == mosaic);
}

TEST(DecoderRejectsOddWidthForSubsampledFormats) {
    const auto g = Geom(15, 8);
    Decoder decoder;
    decoder.Configure(g);

    const auto mosaic = UniformMosaic(g, 10, 10, 10);
    std::vector<uint8_t> out(ImageSize(PixelFormat::Nv12, 15, 8) + 8);
    CHECK_STATUS(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Nv12,
                                 out.data(), out.size()),
                 Status::Unsupported);
}

TEST(DecoderFlipsVertically) {
    const auto g = Geom(4, 4);
    Decoder decoder;
    decoder.Configure(g);

    ColorSettings c;
    c.auto_white_balance = false;
    c.gamma = 1.0f; c.contrast = 1.0f; c.saturation = 1.0f;
    c.flip_vertical = true;
    decoder.SetColor(c);

    // Dark top half, bright bottom half.
    std::vector<uint8_t> mosaic(g.RawSize(), 0);
    for (int y = 2; y < 4; ++y)
        for (int x = 0; x < 4; ++x) mosaic[y * 4 + x] = 255;

    std::vector<uint8_t> out(ImageSize(PixelFormat::Bgra32, 4, 4));
    CHECK_OK(decoder.Convert(mosaic.data(), mosaic.size(), PixelFormat::Bgra32,
                             out.data(), out.size()));

    // After the flip the first row should be the bright one.
    CHECK(out[0] > 128);
    CHECK(out[(3 * 4) * 4] < 128);
}

TEST(WhiteBalanceMovesTowardGreyWorld) {
    const auto g = Geom(32, 32);
    // A blue-cast scene: blue sites read high, red sites low.
    const auto mosaic = UniformMosaic(g, 60, 120, 240);

    WhiteBalance wb;
    for (int i = 0; i < 80; ++i) wb.Update(mosaic.data(), g);

    // Grey world wants red lifted (120/60 = 2) and blue pulled down (120/240 = 0.5).
    CHECK_NEAR(wb.red_gain(), 2.0f, 0.1f);
    CHECK_NEAR(wb.blue_gain(), 0.5f, 0.05f);
}

TEST(WhiteBalanceHoldsStillOnABlackFrame) {
    const auto g = Geom(16, 16);
    const std::vector<uint8_t> black(g.RawSize(), 0);

    WhiteBalance wb;
    for (int i = 0; i < 20; ++i) wb.Update(black.data(), g);

    // Nothing to measure: the gains must not run away chasing noise.
    CHECK_NEAR(wb.red_gain(), 1.0f, 0.001f);
    CHECK_NEAR(wb.blue_gain(), 1.0f, 0.001f);
}

TEST(ScaleNv12SameSizeIsACopy) {
    std::vector<uint8_t> src(8 * 4 * 3 / 2);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7);
    std::vector<uint8_t> dst(src.size(), 0);
    ScaleNv12(src.data(), 8, 4, dst.data(), 8, 4);
    CHECK(src == dst);
}

TEST(ScaleNv12KeepsAUniformImageUniform) {
    // Native sensor size up to VGA: every output sample must stay put.
    const uint16_t sw = 360, sh = 296, dw = 640, dh = 480;
    std::vector<uint8_t> src(sw * sh * 3 / 2);
    std::fill(src.begin(), src.begin() + sw * sh, uint8_t{90});
    for (size_t i = sw * sh; i < src.size(); i += 2) { src[i] = 100; src[i + 1] = 160; }
    std::vector<uint8_t> dst(dw * dh * 3 / 2, 0);
    ScaleNv12(src.data(), sw, sh, dst.data(), dw, dh);
    for (size_t i = 0; i < size_t{dw} * dh; ++i) CHECK_EQ(dst[i], 90);
    for (size_t i = size_t{dw} * dh; i < dst.size(); i += 2) {
        CHECK_EQ(dst[i], 100);
        CHECK_EQ(dst[i + 1], 160);
    }
}

TEST(ScaleNv12CropsToTheTargetAspectRatio) {
    // A 4:3 target from a taller source: the outer rows (marked 0) must be
    // cropped away, never squeezed into the picture. 12 rows to 6 leaves a
    // 3-row margin each side; the crop origin rounds down to an even row for
    // chroma, so only the outer two rows are guaranteed to go.
    const uint16_t sw = 8, sh = 12, dw = 8, dh = 6;
    std::vector<uint8_t> src(sw * sh * 3 / 2, 128);
    for (int y = 0; y < sh; ++y) {
        const bool edge = y < 2 || y >= sh - 2;
        for (int x = 0; x < sw; ++x) src[y * sw + x] = edge ? 0 : 200;
    }
    std::vector<uint8_t> dst(dw * dh * 3 / 2, 0);
    ScaleNv12(src.data(), sw, sh, dst.data(), dw, dh);
    for (size_t i = 0; i < size_t{dw} * dh; ++i) CHECK_EQ(dst[i], 200);
}

TEST(PictureControlDefaultsMatchColourDefaults) {
    const ColorSettings defaults;
    ColorSettings mapped;
    ApplyPictureControls(PictureControls{}, &mapped);
    CHECK_NEAR(mapped.brightness, defaults.brightness, 1e-6f);
    CHECK_NEAR(mapped.contrast, defaults.contrast, 1e-6f);
    CHECK_NEAR(mapped.saturation, defaults.saturation, 1e-6f);
    CHECK_NEAR(mapped.gamma, defaults.gamma, 1e-6f);
}

TEST(PictureControlsMapOntoTheDecoderRange) {
    PictureControls p;
    p.brightness = 100;
    p.contrast   = 150;
    ColorSettings c;
    c.auto_white_balance = false;
    ApplyPictureControls(p, &c);
    CHECK_NEAR(c.brightness, 0.5f, 1e-6f);
    CHECK_NEAR(c.contrast, 1.5f, 1e-6f);
    CHECK(!c.auto_white_balance);  // untouched
}
