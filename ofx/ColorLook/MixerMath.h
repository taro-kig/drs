// MixerMath.h - per-band HSL moves used by Color Look (no OFX dependency, tested alone)
//
// A photo-style HSL mixer: Hue / Saturation / Luminance for 7 colors, picked by Oklab
// hue so the bands follow how colors look (skin = Orange, foliage = Green).
// Neighboring bands cross-fade (weights always sum to 1), so there are no seams.
// Greys and blacks are left alone. Working space: DaVinci Wide Gamut / Intermediate.
#pragma once

#include <algorithm>
#include <cmath>

#include "ColorMath.h"

namespace drs {

constexpr int kMixColors = 7;
// band centers, Oklab hue in degrees (ascending)
static const float kMixCenters[kMixColors] = {25.0f, 60.0f, 100.0f, 140.0f, 195.0f, 260.0f, 325.0f};
static const char* const kMixNames[kMixColors] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Magenta"};

struct MixerSettings {
    float hue[kMixColors] = {}; // -1..1, x 25 degrees
    float sat[kMixColors] = {}; // -1..1, chroma x (1 + v)
    float lum[kMixColors] = {}; // -1..1, about +-1 stop
    int solo = -1;              // >= 0: show the mask of that color
    float mix = 1.0f;
};

// Cross-fade weights of the 7 bands for a hue in degrees; sum to 1.
inline void mixWeights(float hdeg, float w[kMixColors])
{
    for (int i = 0; i < kMixColors; ++i) w[i] = 0.0f;
    float h = std::fmod(hdeg, 360.0f);
    if (h < 0.0f) h += 360.0f;
    int i = kMixColors - 1; // between the last center and the first + 360
    for (int k = 0; k < kMixColors - 1; ++k)
        if (h >= kMixCenters[k] && h < kMixCenters[k + 1]) i = k;
    int j = (i + 1) % kMixColors;
    float a = kMixCenters[i];
    float b = kMixCenters[j] + (j == 0 ? 360.0f : 0.0f);
    float hh = (i == kMixColors - 1 && h < a) ? h + 360.0f : h;
    float t = std::clamp((hh - a) / (b - a), 0.0f, 1.0f);
    float c = std::cos(1.5707963f * t);
    w[i] = c * c;
    w[j] = 1.0f - c * c;
}

inline bool mixerIsIdentity(const MixerSettings& s)
{
    if (s.solo >= 0) return false;
    if (s.mix <= 0.0f) return true;
    for (int i = 0; i < kMixColors; ++i)
        if (s.hue[i] != 0.0f || s.sat[i] != 0.0f || s.lum[i] != 0.0f) return false;
    return true;
}

// Input and output are DWG / DI code values.
inline V3 mixPixel(V3 di, const MixerSettings& s)
{
    V3 lin = {diToLin(di.x), diToLin(di.y), diToLin(di.z)};
    V3 lab = xyzToOklab(mul(kDwgToXyz, lin));
    float L = lab.x;
    float C = std::sqrt(lab.y * lab.y + lab.z * lab.z);

    // greys and near-blacks have no reliable hue: fade the effect out there
    float gate = smoothstep(0.0f, 0.05f, C) * smoothstep(0.02f, 0.1f, L);
    if (gate <= 0.0f || C < 1.0e-6f) {
        if (s.solo >= 0) return {0, 0, 0};
        return di;
    }

    float h = std::atan2(lab.z, lab.y) * 57.29578f;
    float w[kMixColors];
    mixWeights(h, w);

    if (s.solo >= 0) {
        float m = w[s.solo] * gate;
        return {m, m, m};
    }

    float dh = 0, ds = 0, dl = 0;
    for (int i = 0; i < kMixColors; ++i) {
        dh += w[i] * s.hue[i];
        ds += w[i] * s.sat[i];
        dl += w[i] * s.lum[i];
    }
    dh *= gate;
    ds *= gate;
    dl *= gate;

    float h2 = (h + 25.0f * dh) / 57.29578f;
    float C2 = C * std::max(1.0f + ds, 0.0f);
    float L2 = L * std::exp2(dl / 3.0f); // Oklab L ~ cube root of luminance: 1/3 = 1 stop

    V3 out = mul(kXyzToDwg, oklabToXyz({L2, C2 * std::cos(h2), C2 * std::sin(h2)}));
    V3 o = {linToDi(out.x), linToDi(out.y), linToDi(out.z)};
    return {di.x + (o.x - di.x) * s.mix, di.y + (o.y - di.y) * s.mix, di.z + (o.z - di.z) * s.mix};
}

} // namespace drs
