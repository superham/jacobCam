// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bayer demosaic and colour pipeline.
//
// The sensor hands us 8-bit GRBG mosaic data. Apps want NV12 (what Media
// Foundation and every conferencing app prefer) or BGRA (for stills and
// preview). This module owns that conversion plus the small amount of colour
// correction a 1999 CMOS sensor needs to look presentable.

#ifndef QCAM_DECODE_H_
#define QCAM_DECODE_H_

#include <cstdint>
#include <vector>

#include "qcam/types.h"

namespace qcam {

enum class PixelFormat : uint8_t {
    Bayer8,  // raw passthrough
    Bgra32,  // B,G,R,A byte order (Windows DIB / MFVideoFormat_RGB32)
    Nv12,    // Y plane then interleaved VU, 4:2:0
    Yuy2,    // packed 4:2:2
};

const char* PixelFormatName(PixelFormat f);
size_t ImageSize(PixelFormat f, uint16_t width, uint16_t height);

enum class DemosaicQuality : uint8_t {
    Nearest,   // cheapest; doubles pixels. Useful as a fallback.
    Bilinear,  // default
    Malvar,    // 5x5 gradient-corrected linear (Malvar-He-Cutler)
};

struct ColorSettings {
    // Per-channel gains applied before demosaic; the HDCS-1000 has no
    // on-sensor white balance at all, so this is where grey-world lands.
    float red_gain   = 1.0f;
    float green_gain = 1.0f;
    float blue_gain  = 1.0f;

    float gamma      = 0.65f;  // sensor is close to linear; lift the shadows
    float contrast   = 1.0f;   // 0.5 .. 2.0
    float brightness = 0.0f;   // -1.0 .. 1.0, added after gamma
    float saturation = 1.15f;  // mild boost; the CFA is not very selective

    bool  auto_white_balance = true;
    bool  flip_vertical      = false;
    bool  flip_horizontal    = false;
};

// Running grey-world white balance estimator. Kept separate from the
// converter so the gains persist across frames and move smoothly.
class WhiteBalance {
public:
    // Accumulates channel averages from a mosaic frame and eases the gains
    // toward the grey-world solution.
    void Update(const uint8_t* bayer, const FrameGeometry& geom);
    void Reset();

    float red_gain() const { return red_gain_; }
    float blue_gain() const { return blue_gain_; }

    void SetAdaptRate(float rate) { adapt_rate_ = rate; }

private:
    float red_gain_   = 1.0f;
    float blue_gain_  = 1.0f;
    float adapt_rate_ = 0.12f;
};

class Decoder {
public:
    Decoder();

    void Configure(const FrameGeometry& geom);
    void SetQuality(DemosaicQuality q) { quality_ = q; }
    void SetColor(const ColorSettings& c) { color_ = c; }
    const ColorSettings& color() const { return color_; }
    ColorSettings& mutable_color() { return color_; }

    // Converts one raw mosaic frame. `dst` must hold ImageSize(fmt, w, h)
    // bytes for the *output* geometry, which may differ from the sensor
    // geometry when a crop or scale is configured.
    Status Convert(const uint8_t* bayer, size_t bayer_len,
                   PixelFormat fmt, uint8_t* dst, size_t dst_len);

    // Output geometry. Defaults to the sensor geometry. Setting a different
    // size enables centre-crop (when smaller) or bilinear upscale (larger).
    void SetOutputSize(uint16_t width, uint16_t height);
    uint16_t out_width() const { return out_w_; }
    uint16_t out_height() const { return out_h_; }

    WhiteBalance& white_balance() { return wb_; }

private:
    void DemosaicToRgb(const uint8_t* bayer);
    void ApplyTone();
    void ScaleOrCrop();
    void RebuildToneCurve();

    FrameGeometry   geom_{0, 0, BayerPhase::GRBG};
    DemosaicQuality quality_ = DemosaicQuality::Bilinear;
    ColorSettings   color_;
    WhiteBalance    wb_;

    uint16_t out_w_ = 0;
    uint16_t out_h_ = 0;

    std::vector<uint8_t> rgb_;      // sensor-resolution RGB24 scratch
    std::vector<uint8_t> scaled_;   // output-resolution RGB24 scratch
    uint8_t              tone_[256] = {};
    float                tone_gamma_    = -1.0f;
    float                tone_contrast_ = -1.0f;
    float                tone_bright_   = -2.0f;
};

// Standalone helpers, also used by the tests.
void BayerToRgb24(const uint8_t* bayer, const FrameGeometry& geom,
                  DemosaicQuality quality, uint8_t* rgb);
void Rgb24ToNv12(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* nv12);
void Rgb24ToBgra(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* bgra);
void Rgb24ToYuy2(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* yuy2);

// Resizes an NV12 image, first centre-cropping the source to the
// destination's aspect ratio so nothing is stretched, then scaling both planes
// bilinearly. All dimensions must be even. Equal sizes are a plain copy.
void ScaleNv12(const uint8_t* src, uint16_t src_w, uint16_t src_h,
               uint8_t* dst, uint16_t dst_w, uint16_t dst_h);

// Returns the CFA colour (0=R, 1=G, 2=B) at (x, y) for a given phase.
int BayerColorAt(BayerPhase phase, int x, int y);

}  // namespace qcam

#endif  // QCAM_DECODE_H_
