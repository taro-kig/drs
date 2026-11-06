// FrameMath.h - layout, colors and resampling for Photo Frame (no OFX dependency)
//
// Coordinates are image buffer pixels, y up (row 0 = bottom), like OFX.
// The photo keeps its aspect ratio and is scaled to fit inside Size x the canvas, centered
// horizontally; Position splits the free vertical space (0 = photo at the top edge,
// 0.5 = centered, default leaves a wider strip at the bottom for the caption).
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "ColorMath.h"

namespace drs {

enum FrameColor { kFrameWhite = 0, kFrameBlack = 1, kFramePaper = 2, kFrameCustom = 3 };
enum FrameSpace { kSpaceDI = 0, kSpaceDisplay = 1 };
enum TextAlign { kAlignLeft = 0, kAlignCenter = 1, kAlignRight = 2 };

// display (Rec.709 / sRGB-ish code values) colors
inline V3 frameDisplayColor(int mode, V3 custom)
{
    switch (mode) {
    case kFrameWhite: return {0.97f, 0.97f, 0.965f};
    case kFrameBlack: return {0.035f, 0.035f, 0.035f};
    case kFramePaper: return {0.925f, 0.906f, 0.882f}; // warm, slightly dull paper
    default: return custom;
    }
}

inline float luminance709(V3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// caption color that reads well on the frame
inline V3 autoTextColor(V3 bg) { return luminance709(bg) > 0.5f ? V3{0.10f, 0.10f, 0.10f} : V3{0.85f, 0.85f, 0.85f}; }

// thin line around the photo: a little darker on light frames, lighter on dark ones
inline V3 edgeColor(V3 bg)
{
    if (luminance709(bg) > 0.5f) return {bg.x * 0.8f, bg.y * 0.8f, bg.z * 0.8f};
    return {bg.x + 0.12f, bg.y + 0.12f, bg.z + 0.12f};
}

// Display code value -> what to write into the image.
// Display space: as is. DaVinci Intermediate: undo an approximate output tone curve
// (display linear d = x / (x + 1.04), which puts 18% grey near code 0.45), convert
// Rec.709 primaries to DaVinci Wide Gamut, shift by `stops`, encode DI.
inline V3 frameOutputColor(V3 display, int space, float stops)
{
    if (space == kSpaceDisplay) {
        float k = std::exp2(stops / 2.4f); // brightness fine-tune in display terms
        return {display.x * k, display.y * k, display.z * k};
    }
    static const float kRec709ToXyz[9] = {0.4124564f, 0.3575761f, 0.1804375f, 0.2126729f, 0.7151522f,
                                          0.0721750f, 0.0193339f, 0.1191920f, 0.9503041f};
    float c[3] = {display.x, display.y, display.z}, x[3];
    for (int i = 0; i < 3; ++i) {
        float d = std::pow(std::clamp(c[i], 0.0f, 1.0f), 2.4f);
        d = std::min(d, 0.985f);
        x[i] = 1.04f * d / (1.0f - d) * std::exp2(stops);
    }
    V3 dwg = mul(kXyzToDwg, mul(kRec709ToXyz, {x[0], x[1], x[2]}));
    return {linToDi(dwg.x), linToDi(dwg.y), linToDi(dwg.z)};
}

struct FrameRect {
    float x0 = 0, y0 = 0, w = 0, h = 0; // y0 = bottom edge (y up)
};

struct FrameLayout {
    FrameRect photo;
    int textX = 0, textTop = 0; // top-left of the caption bitmap, y up (textTop = highest row)
};

// canvas W x H; the photo's content is cw x ch source pixels; caption bitmap tw x th
inline FrameLayout computeLayout(int W, int H, float cw, float ch, float size, float position, int align, int tw,
                                 float gapPx)
{
    FrameLayout L;
    cw = std::max(cw, 1.0f);
    ch = std::max(ch, 1.0f);
    float s = std::min(size * W / cw, size * H / ch);
    L.photo.w = cw * s;
    L.photo.h = ch * s;
    L.photo.x0 = (W - L.photo.w) * 0.5f;
    float top = (H - L.photo.h) * std::clamp(position, 0.0f, 1.0f);
    L.photo.y0 = H - top - L.photo.h;
    float tx = L.photo.x0;
    if (align == kAlignCenter) tx = L.photo.x0 + (L.photo.w - tw) * 0.5f;
    if (align == kAlignRight) tx = L.photo.x0 + L.photo.w - tw;
    L.textX = int(std::lround(tx));
    L.textTop = int(std::lround(L.photo.y0 - gapPx));
    return L;
}

// Content of the source frame without the black bars Resolve adds when the still's
// aspect differs from the timeline. colMax / rowMax: largest |value| per column / row.
inline void contentRect(const std::vector<float>& colMax, const std::vector<float>& rowMax, int& x0, int& y0, int& w,
                        int& h)
{
    const float eps = 1.0e-5f;
    int W = int(colMax.size()), H = int(rowMax.size());
    int l = 0, r = W - 1, b = 0, t = H - 1;
    while (l < r && colMax[l] < eps) ++l;
    while (r > l && colMax[r] < eps) --r;
    while (b < t && rowMax[b] < eps) ++b;
    while (t > b && rowMax[t] < eps) --t;
    if (r - l < 8 || t - b < 8) { // all black (or nearly): use the whole frame
        l = 0, r = W - 1, b = 0, t = H - 1;
    }
    x0 = l, y0 = b, w = r - l + 1, h = t - b + 1;
}

inline float catmullRom(float t, float a, float b, float c, float d)
{
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

// Bicubic (Catmull-Rom) sample at continuous source position (sx, sy), clamped to the
// content rectangle. at(x, y) returns a pointer to RGBA floats.
template <class At>
V3 sampleBicubic(const At& at, float sx, float sy, int x0, int y0, int w, int h)
{
    float fx = sx - 0.5f, fy = sy - 0.5f;
    int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    float tx = fx - ix, ty = fy - iy;
    float rows[4][3];
    for (int j = 0; j < 4; ++j) {
        int y = std::clamp(iy - 1 + j, y0, y0 + h - 1);
        float p[4][3];
        for (int i = 0; i < 4; ++i) {
            int x = std::clamp(ix - 1 + i, x0, x0 + w - 1);
            const float* s = at(x, y);
            for (int c = 0; c < 3; ++c) p[i][c] = s ? s[c] : 0.0f;
        }
        for (int c = 0; c < 3; ++c) rows[j][c] = catmullRom(tx, p[0][c], p[1][c], p[2][c], p[3][c]);
    }
    float o[3];
    for (int c = 0; c < 3; ++c) o[c] = catmullRom(ty, rows[0][c], rows[1][c], rows[2][c], rows[3][c]);
    return {o[0], o[1], o[2]};
}

// Everything one output pixel needs, shared by the CPU loop and (mirrored) the GPU kernel.
struct FrameDraw {
    FrameLayout layout;
    int cx0 = 0, cy0 = 0, cw = 1, ch = 1; // source content rect
    V3 bg, text, edge;                    // already in output encoding
    float edgeWidth = 0;                  // pixels
    const unsigned char* mask = nullptr;  // caption alpha, tw x th, row 0 = top
    int tw = 0, th = 0;
};

template <class At>
V3 framePixel(const At& at, int x, int y, const FrameDraw& d)
{
    const FrameRect& p = d.layout.photo;
    float u = x + 0.5f, v = y + 0.5f;
    bool inPhoto = u >= p.x0 && u < p.x0 + p.w && v >= p.y0 && v < p.y0 + p.h;
    if (inPhoto) {
        float sx = d.cx0 + (u - p.x0) / p.w * d.cw;
        float sy = d.cy0 + (v - p.y0) / p.h * d.ch;
        return sampleBicubic(at, sx, sy, d.cx0, d.cy0, d.cw, d.ch);
    }
    if (d.edgeWidth > 0.0f) {
        float e = d.edgeWidth;
        if (u >= p.x0 - e && u < p.x0 + p.w + e && v >= p.y0 - e && v < p.y0 + p.h + e) return d.edge;
    }
    if (d.mask) {
        int mx = x - d.layout.textX, my = d.layout.textTop - y; // row 0 = top
        if (mx >= 0 && mx < d.tw && my >= 0 && my < d.th) {
            float a = d.mask[my * d.tw + mx] / 255.0f;
            if (a > 0.0f)
                return {d.bg.x + (d.text.x - d.bg.x) * a, d.bg.y + (d.text.y - d.bg.y) * a,
                        d.bg.z + (d.text.z - d.bg.z) * a};
        }
    }
    return d.bg;
}

} // namespace drs
