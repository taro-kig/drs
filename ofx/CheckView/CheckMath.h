// CheckMath.h - Check View pixel math (no OFX dependency). The OpenCL kernel `checkView` in
// common/ClLut.cpp mirrors checkPixel(); the zone colors and gamut matrix come from here.
#pragma once

#include <algorithm>
#include <cmath>

#include "ColorMath.h"

namespace drs {

enum CheckMode { kCheckFalseColor = 0, kCheckSkin = 1, kCheckGamut709 = 2, kCheckGamutP3 = 3 };

// False color: exposure in stops from 18% grey (DWG luminance). Zones outside these bands
// show the picture in grey.
constexpr int kZones = 8;
static const float kZoneLo[kZones] = {-99.0f, -6.0f, -0.5f, 1.0f, 3.0f, 4.5f, 6.0f, 99.0f};
static const float kZoneHi[kZones] = {-6.0f, -4.0f, 0.5f, 1.8f, 4.5f, 6.0f, 99.0f, 99.0f};
// display-ish sRGB for each zone: crushed, deep shadow, middle grey, bright skin,
// highlights, near clip, clipped; the last entry is the gamut alert
static const float kZoneSrgb[kZones][3] = {{0.45f, 0.0f, 0.65f}, {0.1f, 0.3f, 0.95f}, {0.15f, 0.8f, 0.2f},
                                           {0.95f, 0.55f, 0.7f},  {0.95f, 0.85f, 0.1f}, {1.0f, 0.5f, 0.05f},
                                           {0.95f, 0.05f, 0.05f}, {1.0f, 0.0f, 1.0f}};

struct CheckSettings {
    int mode = kCheckFalseColor;
    float zone[kZones * 3] = {}; // zone colors as DI code values; the last one = gamut alert (magenta)
    float gamut[9] = {};         // linear DWG -> linear target gamut (Rec.709 or P3-D65)
};

inline CheckSettings makeCheck(int mode)
{
    CheckSettings s;
    s.mode = mode;
    for (int i = 0; i < kZones; ++i) {
        V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(kZoneSrgb[i][0]), srgbToLin(kZoneSrgb[i][1]),
                                               srgbToLin(kZoneSrgb[i][2])}));
        s.zone[i * 3] = linToDi(d.x), s.zone[i * 3 + 1] = linToDi(d.y), s.zone[i * 3 + 2] = linToDi(d.z);
    }
    static const float kXyzTo709[9] = {3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
                                       0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f};
    static const float kXyzToP3[9] = {2.4934969f, -0.9313836f, -0.4027108f, -0.8294890f, 1.7626641f,
                                      0.0236247f, 0.0358458f,  -0.0761724f, 0.9568845f};
    const float* t = mode == kCheckGamutP3 ? kXyzToP3 : kXyzTo709;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += t[r * 3 + k] * kDwgToXyz[k * 3 + c];
            s.gamut[r * 3 + c] = v;
        }
    return s;
}

inline float checkLuma(V3 l) { return 0.27411851f * l.x + 0.87363190f * l.y - 0.14775041f * l.z; }

// grey version of the picture (DI code value of its luminance), dimmed
inline V3 checkGrey(float Y, float dim)
{
    float g = linToDi(std::max(Y, 0.0f) * dim);
    return {g, g, g};
}

inline V3 checkPixel(V3 di, const CheckSettings& s)
{
    V3 l = {diToLin(di.x), diToLin(di.y), diToLin(di.z)};
    float Y = checkLuma(l);
    if (s.mode == kCheckFalseColor) {
        float st = std::log2(std::max(Y, 1.0e-9f) / 0.18f);
        for (int i = 0; i < kZones - 1; ++i)
            if (st >= kZoneLo[i] && st < kZoneHi[i]) return {s.zone[i * 3], s.zone[i * 3 + 1], s.zone[i * 3 + 2]};
        return checkGrey(Y, 1.0f);
    }
    if (s.mode == kCheckSkin) {
        // skin: Oklab hue 30..80 deg (center 55), chroma .025..0.18, not too dark or bright
        V3 lab = xyzToOklab(mul(kDwgToXyz, l));
        float C = std::sqrt(lab.y * lab.y + lab.z * lab.z);
        float h = std::atan2(lab.z, lab.y) * 57.29578f;
        float dh = std::fabs(h - 55.0f);
        float w = (1.0f - smoothstep(18.0f, 28.0f, dh)) * smoothstep(0.02f, 0.035f, C) *
                  (1.0f - smoothstep(0.16f, 0.2f, C)) * smoothstep(0.22f, 0.3f, lab.x) * (1.0f - smoothstep(0.92f, 0.97f, lab.x));
        if (!(w == w)) w = 0.0f;
        V3 g = checkGrey(Y, 0.5f);
        return {g.x + (di.x - g.x) * w, g.y + (di.y - g.y) * w, g.z + (di.z - g.z) * w};
    }
    // gamut: colors the target gamut cannot show (a channel below zero) in magenta
    V3 t = mul(s.gamut, l);
    float mx = std::max(t.x, std::max(t.y, t.z)), mn = std::min(t.x, std::min(t.y, t.z));
    const int a = (kZones - 1) * 3;
    if (mx > 1.0e-6f && mn < -0.002f * mx) return {s.zone[a], s.zone[a + 1], s.zone[a + 2]};
    return checkGrey(Y, 1.0f);
}

} // namespace drs
