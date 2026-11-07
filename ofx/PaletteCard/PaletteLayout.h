// PaletteLayout.h - where Palette Card draws its swatches (no OFX dependency)
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "FrameColor.h"

namespace drs {

enum PalettePos { kPosBottom = 0, kPosRight = 1, kPosFull = 2 };

struct PaletteDraw {
    std::vector<float> rects;  // x0 y0 x1 y1, in pixels with y = stored row (0 = bottom)
    std::vector<float> colors; // DI rgb per rect
};

inline V3 swatchDi(const Swatch& s)
{
    V3 l = labToLin(s.lab);
    return linToDi3({std::max(l.x, 0.0f), std::max(l.y, 0.0f), std::max(l.z, 0.0f)});
}

// size: strip thickness as a share of the frame height (bottom) or width (right)
inline PaletteDraw layoutPalette(const std::vector<Swatch>& pal, int W, int H, int pos, float size, bool proportional)
{
    PaletteDraw d;
    if (pal.empty() || W <= 0 || H <= 0) return d;
    const float bg = linToDi(0.02f); // dark band behind the swatches
    float bx0 = 0, by0 = 0, bx1 = float(W), by1 = float(H);
    size = std::clamp(size, 0.03f, 0.5f);
    if (pos == kPosBottom) by1 = std::round(H * size);
    else if (pos == kPosRight) bx0 = std::round(W * (1.0f - size));
    d.rects.insert(d.rects.end(), {bx0, by0, bx1, by1});
    d.colors.insert(d.colors.end(), {bg, bg, bg});

    const bool vertical = pos == kPosRight; // swatches stacked top to bottom
    float len = vertical ? by1 - by0 : bx1 - bx0, thick = vertical ? bx1 - bx0 : by1 - by0;
    float pad = std::max(1.0f, std::round(std::min(W, H) * 0.012f));
    float gap = std::max(1.0f, std::round(pad * 0.5f));
    float usable = len - 2 * pad - gap * float(pal.size() - 1);
    float total = 0;
    for (const Swatch& s : pal) total += proportional ? std::max(s.share, 0.02f) : 1.0f;
    float at = pad;
    for (const Swatch& s : pal) {
        float w = usable * (proportional ? std::max(s.share, 0.02f) : 1.0f) / total;
        float a0 = std::round(at), a1 = std::round(at + w);
        float t0 = pad, t1 = thick - pad;
        if (vertical) // first swatch at the top
            d.rects.insert(d.rects.end(), {bx0 + t0, by1 - a1, bx0 + t1, by1 - a0});
        else
            d.rects.insert(d.rects.end(), {bx0 + a0, by0 + t0, bx0 + a1, by0 + t1});
        V3 c = swatchDi(s);
        d.colors.insert(d.colors.end(), {c.x, c.y, c.z});
        at += w + gap;
    }
    return d;
}

} // namespace drs
