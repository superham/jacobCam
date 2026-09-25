// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bayer demosaic and colour conversion.

#include "qcam/decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "qcam/log.h"

namespace qcam {
namespace {

inline uint8_t ClampU8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}
inline uint8_t ClampU8f(float v) {
    return ClampU8(static_cast<int>(v + 0.5f));
}

// Colour index (0=R, 1=G, 2=B) of each site in the 2x2 CFA quad, in raster
// order, for every supported phase.
constexpr uint8_t kPhaseQuad[4][4] = {
    /* GRBG */ {1, 0, 2, 1},
    /* RGGB */ {0, 1, 1, 2},
    /* BGGR */ {2, 1, 1, 0},
    /* GBRG */ {1, 2, 0, 1},
};

// --- Malvar-He-Cutler 5x5 kernels, coefficients scaled by 2 (divisor 16) ---
constexpr int kMalvarGreenAtRB[25] = {
     0,  0, -2,  0,  0,
     0,  0,  4,  0,  0,
    -2,  4,  8,  4, -2,
     0,  0,  4,  0,  0,
     0,  0, -2,  0,  0,
};
// Chroma at a green site whose horizontal neighbours share that chroma.
constexpr int kMalvarChromaRow[25] = {
     0,  0,  1,  0,  0,
     0, -2,  0, -2,  0,
    -2,  8, 10,  8, -2,
     0, -2,  0, -2,  0,
     0,  0,  1,  0,  0,
};
// Chroma at a green site whose vertical neighbours share that chroma.
constexpr int kMalvarChromaCol[25] = {
     0,  0, -2,  0,  0,
     0, -2,  8, -2,  0,
     1,  0, 10,  0,  1,
     0, -2,  8, -2,  0,
     0,  0, -2,  0,  0,
};
// The opposite chroma at an R or B site.
constexpr int kMalvarChromaDiag[25] = {
     0,  0, -3,  0,  0,
     0,  4,  0,  4,  0,
    -3,  0, 12,  0, -3,
     0,  4,  0,  4,  0,
     0,  0, -3,  0,  0,
};

struct Mosaic {
    const uint8_t* data;
    int w;
    int h;

    // Clamped sample, so edge pixels mirror rather than wrap.
    inline int At(int x, int y) const {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return data[static_cast<size_t>(y) * w + x];
    }
};

inline int Convolve5(const Mosaic& m, int x, int y, const int (&k)[25]) {
    int acc = 0;
    for (int dy = -2; dy <= 2; ++dy) {
        const int* row = &k[(dy + 2) * 5];
        for (int dx = -2; dx <= 2; ++dx) {
            const int c = row[dx + 2];
            if (c) acc += c * m.At(x + dx, y + dy);
        }
    }
    return acc / 16;
}

void DemosaicNearest(const Mosaic& m, BayerPhase phase, uint8_t* rgb) {
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            // Snap to the top-left of the containing 2x2 quad and read each
            // channel from its own site.
            const int qx = x & ~1;
            const int qy = y & ~1;
            int c[3] = {0, 0, 0};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int idx = BayerColorAt(phase, qx + dx, qy + dy);
                    const int v = m.At(qx + dx, qy + dy);
                    if (idx == 1) c[1] += v / 2;  // two greens per quad
                    else c[idx] = v;
                }
            }
            uint8_t* out = rgb + (static_cast<size_t>(y) * m.w + x) * 3;
            out[0] = ClampU8(c[0]);
            out[1] = ClampU8(c[1]);
            out[2] = ClampU8(c[2]);
        }
    }
}

void DemosaicBilinear(const Mosaic& m, BayerPhase phase, uint8_t* rgb) {
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            const int self = BayerColorAt(phase, x, y);
            const int v    = m.At(x, y);
            int r = 0, g = 0, b = 0;

            if (self == 1) {
                // Green site. One axis carries red, the other blue; which is
                // which depends on the row parity.
                g = v;
                const int horiz = BayerColorAt(phase, x + 1, y);
                const int h_avg = (m.At(x - 1, y) + m.At(x + 1, y)) / 2;
                const int v_avg = (m.At(x, y - 1) + m.At(x, y + 1)) / 2;
                if (horiz == 0) { r = h_avg; b = v_avg; }
                else            { b = h_avg; r = v_avg; }
            } else {
                // Red or blue site: green from the four orthogonal
                // neighbours, opposite chroma from the four diagonals.
                g = (m.At(x - 1, y) + m.At(x + 1, y) +
                     m.At(x, y - 1) + m.At(x, y + 1)) / 4;
                const int diag = (m.At(x - 1, y - 1) + m.At(x + 1, y - 1) +
                                  m.At(x - 1, y + 1) + m.At(x + 1, y + 1)) / 4;
                if (self == 0) { r = v; b = diag; }
                else           { b = v; r = diag; }
            }

            uint8_t* out = rgb + (static_cast<size_t>(y) * m.w + x) * 3;
            out[0] = ClampU8(r);
            out[1] = ClampU8(g);
            out[2] = ClampU8(b);
        }
    }
}

void DemosaicMalvar(const Mosaic& m, BayerPhase phase, uint8_t* rgb) {
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            const int self = BayerColorAt(phase, x, y);
            const int v    = m.At(x, y);
            int r = 0, g = 0, b = 0;

            if (self == 1) {
                g = v;
                const int horiz = BayerColorAt(phase, x + 1, y);
                const int along = Convolve5(m, x, y, kMalvarChromaRow);
                const int across = Convolve5(m, x, y, kMalvarChromaCol);
                if (horiz == 0) { r = along; b = across; }
                else            { b = along; r = across; }
            } else {
                g = Convolve5(m, x, y, kMalvarGreenAtRB);
                const int other = Convolve5(m, x, y, kMalvarChromaDiag);
                if (self == 0) { r = v; b = other; }
                else           { b = v; r = other; }
            }

            uint8_t* out = rgb + (static_cast<size_t>(y) * m.w + x) * 3;
            out[0] = ClampU8(r);
            out[1] = ClampU8(g);
            out[2] = ClampU8(b);
        }
    }
}

}  // namespace

int BayerColorAt(BayerPhase phase, int x, int y) {
    // Negative coordinates appear when a caller probes a neighbour off the
    // edge; parity has to stay stable there.
    const int px = ((x % 2) + 2) % 2;
    const int py = ((y % 2) + 2) % 2;
    return kPhaseQuad[static_cast<int>(phase)][py * 2 + px];
}

const char* PixelFormatName(PixelFormat f) {
    switch (f) {
        case PixelFormat::Bayer8: return "BAYER8";
        case PixelFormat::Bgra32: return "BGRA32";
        case PixelFormat::Nv12:   return "NV12";
        case PixelFormat::Yuy2:   return "YUY2";
    }
    return "?";
}

size_t ImageSize(PixelFormat f, uint16_t width, uint16_t height) {
    const size_t px = static_cast<size_t>(width) * height;
    switch (f) {
        case PixelFormat::Bayer8: return px;
        case PixelFormat::Bgra32: return px * 4;
        case PixelFormat::Nv12:   return px + (px / 2);
        case PixelFormat::Yuy2:   return px * 2;
    }
    return 0;
}

void BayerToRgb24(const uint8_t* bayer, const FrameGeometry& geom,
                  DemosaicQuality quality, uint8_t* rgb) {
    const Mosaic m{bayer, geom.width, geom.height};
    switch (quality) {
        case DemosaicQuality::Nearest:  DemosaicNearest(m, geom.phase, rgb);  break;
        case DemosaicQuality::Bilinear: DemosaicBilinear(m, geom.phase, rgb); break;
        case DemosaicQuality::Malvar:   DemosaicMalvar(m, geom.phase, rgb);   break;
    }
}

// --- Colour space conversion (BT.601 studio swing) -------------------------

void Rgb24ToNv12(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* nv12) {
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + static_cast<size_t>(w) * h;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = rgb + (static_cast<size_t>(y) * w + x) * 3;
            const int r = p[0], g = p[1], b = p[2];
            y_plane[static_cast<size_t>(y) * w + x] =
                ClampU8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    // Chroma is sampled from the 2x2 average so the subsampling does not
    // alias on the Bayer grid.
    for (int y = 0; y + 1 < h; y += 2) {
        for (int x = 0; x + 1 < w; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p =
                        rgb + (static_cast<size_t>(y + dy) * w + (x + dx)) * 3;
                    r += p[0]; g += p[1]; b += p[2];
                }
            }
            r /= 4; g /= 4; b /= 4;
            uint8_t* uv = uv_plane + (static_cast<size_t>(y / 2) * w) + x;
            uv[0] = ClampU8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[1] = ClampU8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

void Rgb24ToBgra(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* bgra) {
    const size_t px = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < px; ++i) {
        bgra[i * 4 + 0] = rgb[i * 3 + 2];
        bgra[i * 4 + 1] = rgb[i * 3 + 1];
        bgra[i * 4 + 2] = rgb[i * 3 + 0];
        bgra[i * 4 + 3] = 0xff;
    }
}

void Rgb24ToYuy2(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t* yuy2) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x + 1 < w; x += 2) {
            const uint8_t* p0 = rgb + (static_cast<size_t>(y) * w + x) * 3;
            const uint8_t* p1 = p0 + 3;
            const int y0 = ((66 * p0[0] + 129 * p0[1] + 25 * p0[2] + 128) >> 8) + 16;
            const int y1 = ((66 * p1[0] + 129 * p1[1] + 25 * p1[2] + 128) >> 8) + 16;
            const int r = (p0[0] + p1[0]) / 2;
            const int g = (p0[1] + p1[1]) / 2;
            const int b = (p0[2] + p1[2]) / 2;
            uint8_t* out = yuy2 + (static_cast<size_t>(y) * w + x) * 2;
            out[0] = ClampU8(y0);
            out[1] = ClampU8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out[2] = ClampU8(y1);
            out[3] = ClampU8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

// --- White balance ---------------------------------------------------------

void WhiteBalance::Reset() {
    red_gain_  = 1.0f;
    blue_gain_ = 1.0f;
}

void WhiteBalance::Update(const uint8_t* bayer, const FrameGeometry& geom) {
    // Grey world, measured straight off the mosaic: no demosaic needed, and
    // each channel is sampled at its own sites.
    uint64_t sum[3] = {0, 0, 0};
    uint64_t cnt[3] = {0, 0, 0};

    // Step by 2 in both axes and visit the whole quad, so every channel gets
    // an unbiased sample set even on a subsampled pass.
    const int step = (geom.width > 320) ? 2 : 1;
    for (int y = 0; y + 1 < geom.height; y += 2 * step) {
        for (int x = 0; x + 1 < geom.width; x += 2 * step) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int c = BayerColorAt(geom.phase, x + dx, y + dy);
                    sum[c] += bayer[static_cast<size_t>(y + dy) * geom.width + x + dx];
                    cnt[c]++;
                }
            }
        }
    }
    if (!cnt[0] || !cnt[1] || !cnt[2]) return;

    const float avg_r = static_cast<float>(sum[0]) / cnt[0];
    const float avg_g = static_cast<float>(sum[1]) / cnt[1];
    const float avg_b = static_cast<float>(sum[2]) / cnt[2];

    // Too dark to estimate anything: hold the current gains rather than
    // chasing sensor noise.
    if (avg_r < 2.0f || avg_g < 2.0f || avg_b < 2.0f) return;

    const float want_r = std::clamp(avg_g / avg_r, 0.4f, 4.0f);
    const float want_b = std::clamp(avg_g / avg_b, 0.4f, 4.0f);

    red_gain_  += (want_r - red_gain_) * adapt_rate_;
    blue_gain_ += (want_b - blue_gain_) * adapt_rate_;
}

// --- Decoder ---------------------------------------------------------------

Decoder::Decoder() { RebuildToneCurve(); }

void Decoder::Configure(const FrameGeometry& geom) {
    geom_ = geom;
    rgb_.assign(geom.RawSize() * 3, 0);
    if (out_w_ == 0 || out_h_ == 0) {
        out_w_ = geom.width;
        out_h_ = geom.height;
    }
    scaled_.assign(static_cast<size_t>(out_w_) * out_h_ * 3, 0);
    wb_.Reset();
}

void Decoder::SetOutputSize(uint16_t width, uint16_t height) {
    if (width == 0 || height == 0) {
        out_w_ = geom_.width;
        out_h_ = geom_.height;
    } else {
        out_w_ = width;
        out_h_ = height;
    }
    scaled_.assign(static_cast<size_t>(out_w_) * out_h_ * 3, 0);
}

void Decoder::RebuildToneCurve() {
    const float gamma    = std::clamp(color_.gamma, 0.1f, 4.0f);
    const float contrast = std::clamp(color_.contrast, 0.2f, 4.0f);
    const float bright   = std::clamp(color_.brightness, -1.0f, 1.0f);

    for (int i = 0; i < 256; ++i) {
        float v = static_cast<float>(i) / 255.0f;
        v = std::pow(v, gamma);
        v = (v - 0.5f) * contrast + 0.5f;
        v += bright;
        tone_[i] = ClampU8f(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    }
    tone_gamma_    = gamma;
    tone_contrast_ = contrast;
    tone_bright_   = bright;
}

void Decoder::DemosaicToRgb(const uint8_t* bayer) {
    BayerToRgb24(bayer, geom_, quality_, rgb_.data());
}

void Decoder::ApplyTone() {
    if (tone_gamma_ != std::clamp(color_.gamma, 0.1f, 4.0f) ||
        tone_contrast_ != std::clamp(color_.contrast, 0.2f, 4.0f) ||
        tone_bright_ != std::clamp(color_.brightness, -1.0f, 1.0f)) {
        RebuildToneCurve();
    }

    const float rg = color_.red_gain   * (color_.auto_white_balance ? wb_.red_gain()  : 1.0f);
    const float bg = color_.blue_gain  * (color_.auto_white_balance ? wb_.blue_gain() : 1.0f);
    const float gg = color_.green_gain;
    const float sat = std::clamp(color_.saturation, 0.0f, 3.0f);
    const bool do_sat = std::fabs(sat - 1.0f) > 0.01f;

    const size_t px = static_cast<size_t>(geom_.width) * geom_.height;
    for (size_t i = 0; i < px; ++i) {
        uint8_t* p = rgb_.data() + i * 3;

        // Channel gains first: white balance belongs in linear-ish space,
        // before the tone curve compresses the highlights.
        int r = static_cast<int>(p[0] * rg + 0.5f);
        int g = static_cast<int>(p[1] * gg + 0.5f);
        int b = static_cast<int>(p[2] * bg + 0.5f);

        r = tone_[ClampU8(r)];
        g = tone_[ClampU8(g)];
        b = tone_[ClampU8(b)];

        if (do_sat) {
            const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
            r = static_cast<int>(luma + (r - luma) * sat + 0.5f);
            g = static_cast<int>(luma + (g - luma) * sat + 0.5f);
            b = static_cast<int>(luma + (b - luma) * sat + 0.5f);
        }

        p[0] = ClampU8(r);
        p[1] = ClampU8(g);
        p[2] = ClampU8(b);
    }
}

void Decoder::ScaleOrCrop() {
    const int sw = geom_.width, sh = geom_.height;
    const int dw = out_w_, dh = out_h_;
    const bool flip_h = color_.flip_horizontal;
    const bool flip_v = color_.flip_vertical;

    if (dw <= sw && dh <= sh) {
        // Centre crop: no resampling, so the picture stays as sharp as the
        // sensor made it.
        const int ox = (sw - dw) / 2;
        const int oy = (sh - dh) / 2;
        for (int y = 0; y < dh; ++y) {
            const int sy = flip_v ? (oy + dh - 1 - y) : (oy + y);
            for (int x = 0; x < dw; ++x) {
                const int sx = flip_h ? (ox + dw - 1 - x) : (ox + x);
                const uint8_t* src = rgb_.data() + (static_cast<size_t>(sy) * sw + sx) * 3;
                uint8_t* dst = scaled_.data() + (static_cast<size_t>(y) * dw + x) * 3;
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            }
        }
        return;
    }

    // Bilinear resample to the requested size.
    const float x_ratio = (dw > 1) ? static_cast<float>(sw - 1) / (dw - 1) : 0.0f;
    const float y_ratio = (dh > 1) ? static_cast<float>(sh - 1) / (dh - 1) : 0.0f;

    for (int y = 0; y < dh; ++y) {
        const int yy = flip_v ? (dh - 1 - y) : y;
        const float fy = yy * y_ratio;
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float wy = fy - y0;

        for (int x = 0; x < dw; ++x) {
            const int xx = flip_h ? (dw - 1 - x) : x;
            const float fx = xx * x_ratio;
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, sw - 1);
            const float wx = fx - x0;

            const uint8_t* p00 = rgb_.data() + (static_cast<size_t>(y0) * sw + x0) * 3;
            const uint8_t* p01 = rgb_.data() + (static_cast<size_t>(y0) * sw + x1) * 3;
            const uint8_t* p10 = rgb_.data() + (static_cast<size_t>(y1) * sw + x0) * 3;
            const uint8_t* p11 = rgb_.data() + (static_cast<size_t>(y1) * sw + x1) * 3;
            uint8_t* dst = scaled_.data() + (static_cast<size_t>(y) * dw + x) * 3;

            for (int c = 0; c < 3; ++c) {
                const float top    = p00[c] + (p01[c] - p00[c]) * wx;
                const float bottom = p10[c] + (p11[c] - p10[c]) * wx;
                dst[c] = ClampU8f(top + (bottom - top) * wy);
            }
        }
    }
}

Status Decoder::Convert(const uint8_t* bayer, size_t bayer_len,
                        PixelFormat fmt, uint8_t* dst, size_t dst_len) {
    if (!bayer || !dst) return Status::InvalidArg;
    if (geom_.width == 0 || geom_.height == 0) return Status::InvalidArg;
    if (bayer_len < geom_.RawSize()) return Status::InvalidArg;
    if (dst_len < ImageSize(fmt, out_w_, out_h_)) return Status::InvalidArg;

    if (fmt == PixelFormat::Bayer8) {
        // Raw passthrough ignores the colour pipeline and the output size by
        // definition; callers asking for Bayer want exactly what the sensor
        // produced.
        if (dst_len < geom_.RawSize()) return Status::InvalidArg;
        std::memcpy(dst, bayer, geom_.RawSize());
        return Status::Ok;
    }

    if ((fmt == PixelFormat::Nv12 || fmt == PixelFormat::Yuy2) && (out_w_ & 1))
        return Status::Unsupported;
    if (fmt == PixelFormat::Nv12 && (out_h_ & 1)) return Status::Unsupported;

    if (color_.auto_white_balance) wb_.Update(bayer, geom_);

    DemosaicToRgb(bayer);
    ApplyTone();
    ScaleOrCrop();

    switch (fmt) {
        case PixelFormat::Bgra32: Rgb24ToBgra(scaled_.data(), out_w_, out_h_, dst); break;
        case PixelFormat::Nv12:   Rgb24ToNv12(scaled_.data(), out_w_, out_h_, dst); break;
        case PixelFormat::Yuy2:   Rgb24ToYuy2(scaled_.data(), out_w_, out_h_, dst); break;
        default: return Status::Unsupported;
    }
    return Status::Ok;
}

namespace {

// Bilinear resample of one plane of `channels` interleaved bytes per sample,
// reading the window (x0, y0, cw, ch) of a `stride`-byte-wide source.
void ResamplePlane(const uint8_t* src, int stride, int x0, int y0, int cw, int ch,
                   uint8_t* dst, int dw, int dh, int channels) {
    for (int y = 0; y < dh; ++y) {
        // Pixel-centre mapping, so the image does not drift by half a pixel.
        const float fy = std::clamp((y + 0.5f) * ch / dh - 0.5f, 0.0f, ch - 1.0f);
        const int   iy = static_cast<int>(fy);
        const int   iy1 = std::min(iy + 1, ch - 1);
        const float wy = fy - iy;
        const uint8_t* r0 = src + static_cast<size_t>(y0 + iy) * stride;
        const uint8_t* r1 = src + static_cast<size_t>(y0 + iy1) * stride;
        uint8_t* out = dst + static_cast<size_t>(y) * dw * channels;
        for (int x = 0; x < dw; ++x) {
            const float fx = std::clamp((x + 0.5f) * cw / dw - 0.5f, 0.0f, cw - 1.0f);
            const int   ix = static_cast<int>(fx);
            const int   ix1 = std::min(ix + 1, cw - 1);
            const float wx = fx - ix;
            const int a = (x0 + ix) * channels, b = (x0 + ix1) * channels;
            for (int c = 0; c < channels; ++c) {
                const float top = r0[a + c] + (r0[b + c] - r0[a + c]) * wx;
                const float bot = r1[a + c] + (r1[b + c] - r1[a + c]) * wx;
                out[x * channels + c] = ClampU8f(top + (bot - top) * wy);
            }
        }
    }
}

}  // namespace

void ScaleNv12(const uint8_t* src, uint16_t src_w, uint16_t src_h,
               uint8_t* dst, uint16_t dst_w, uint16_t dst_h) {
    if (src_w == dst_w && src_h == dst_h) {
        std::memcpy(dst, src, static_cast<size_t>(src_w) * src_h * 3 / 2);
        return;
    }

    // Largest window of the source with the destination's aspect ratio,
    // centred. Kept even so it lines up with the 2x2 chroma sites.
    int cw = src_w, ch = src_h;
    if (static_cast<long>(src_w) * dst_h > static_cast<long>(src_h) * dst_w)
        cw = static_cast<int>(static_cast<long>(src_h) * dst_w / dst_h);
    else
        ch = static_cast<int>(static_cast<long>(src_w) * dst_h / dst_w);
    cw &= ~1;
    ch &= ~1;
    const int x0 = ((src_w - cw) / 2) & ~1;
    const int y0 = ((src_h - ch) / 2) & ~1;

    ResamplePlane(src, src_w, x0, y0, cw, ch, dst, dst_w, dst_h, 1);
    ResamplePlane(src + static_cast<size_t>(src_w) * src_h, src_w, x0 / 2, y0 / 2,
                  cw / 2, ch / 2, dst + static_cast<size_t>(dst_w) * dst_h,
                  dst_w / 2, dst_h / 2, 2);
}

}  // namespace qcam
