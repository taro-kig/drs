// LookMath.h - the look model behind Look Explorer (no OFX dependency, tested alone)
//
// A look is a vector of kDims numbers, each roughly in [-1, 1], 0 = neutral.
// The user never sees these: they pick among 3x3 variations and the vector follows.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ColorMath.h"

namespace drs {

enum LookDim {
    kWarm,       // + warmer (red up / blue down, in stops)
    kTint,       // + magenta / - green
    kContrast,   // + more contrast around 18% grey
    kFilmSat,    // interimage: + richer separation between channels
    kShoulder,   // + softer highlight roll-off
    kFade,       // + lifted (matte) blacks / - crushed blacks
    kSat,        // overall saturation
    kShadowA,    // shadow tint, Oklab a (+ magenta / - green)
    kShadowB,    // shadow tint, Oklab b (+ yellow / - blue)
    kHighA,      // highlight tint, Oklab a
    kHighB,      // highlight tint, Oklab b
    kHueSkin,    // rotate oranges/skin (+ toward yellow / - toward red)
    kHueGreen,   // rotate greens (+ toward teal / - toward yellow)
    kHueBlue,    // rotate blues (+ toward purple / - toward teal)
    kDye,        // + saturated colors get denser (darker), like print film
    kDims
};

struct Look {
    float p[kDims] = {};
};

// Round 0: hand-made starting points. Cell 5 (index 4) is neutral.
// clang-format off
//                    warm  tint  con   fsat  shld  fade  sat   shA   shB   hiA   hiB   hSkn  hGrn  hBlu  dye
static const float kArchetypes[9][kDims] = {
    /* 1 Warm Print */ {0.5f, 0.1f, 0.3f, 0.5f, 0.6f, 0.2f, 0.1f, 0.2f,-0.4f, 0.2f, 0.6f, 0.1f,-0.4f,-0.3f, 0.5f},
    /* 2 Teal/Orange*/ {0.1f, 0.0f, 0.4f, 0.3f, 0.3f, 0.0f, 0.2f,-0.6f,-0.5f, 0.3f, 0.6f, 0.2f, 0.6f,-0.5f, 0.3f},
    /* 3 Faded Matte*/ {0.2f, 0.1f,-0.3f, 0.1f, 0.8f, 0.9f,-0.3f, 0.1f,-0.2f, 0.1f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f},
    /* 4 Slide Film */ {-0.1f,0.0f, 0.7f, 0.6f, 0.2f,-0.5f, 0.4f, 0.0f,-0.3f, 0.0f, 0.2f,-0.1f, 0.2f, 0.4f, 0.6f},
    /* 5 Neutral    */ {0},
    /* 6 Bleach     */ {-0.1f,0.0f, 0.8f,-0.4f, 0.3f,-0.3f,-0.8f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.3f},
    /* 7 Pastel     */ {0.3f, 0.3f,-0.5f,-0.2f, 1.0f, 0.5f,-0.2f, 0.3f, 0.1f, 0.2f, 0.3f, 0.1f, 0.5f,-0.2f,-0.3f},
    /* 8 Moody Green*/ {-0.3f,-0.4f,0.3f, 0.4f, 0.5f, 0.3f,-0.3f,-0.3f, 0.2f, 0.1f, 0.3f, 0.2f, 0.6f, 0.0f, 0.4f},
    /* 9 Nordic     */ {-0.6f,0.1f, 0.1f, 0.2f, 0.6f, 0.3f,-0.4f,-0.2f,-0.4f, 0.0f,-0.1f, 0.0f, 0.3f,-0.3f, 0.2f},
};
// clang-format on
static const char* const kArchetypeNames[9] = {"Warm Print", "Teal Orange", "Faded Matte", "Slide Film", "Neutral",
                                               "Bleach",     "Pastel",      "Moody Green", "Nordic"};

// deterministic hash -> [-1, 1]
inline float hashUnit(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) / float(0xFFFFFF) * 2.0f - 1.0f;
}

// How far variations reach from the center in a given round (1, 2, ...).
inline float spreadFor(int round, float variety)
{
    return variety * std::pow(0.72f, float(std::max(round - 1, 0)));
}

// Variation `cell` (0..8) of a round. Round 0 shows the archetypes; later rounds show
// the center (cell 4) surrounded by 8 random moves, each touching about half the dims.
inline Look variation(const Look& center, int round, uint32_t seed, int cell, float variety)
{
    Look out = center;
    if (round <= 0) {
        for (int d = 0; d < kDims; ++d) out.p[d] = kArchetypes[cell][d];
        return out;
    }
    if (cell == 4) return center;
    float s = spreadFor(round, variety);
    for (int d = 0; d < kDims; ++d) {
        float gate = hashUnit(seed, uint32_t(cell), uint32_t(d) + 1000u);
        if (gate < 0.0f) continue; // leave this dim alone in this variation
        float u = hashUnit(seed, uint32_t(cell), uint32_t(d));
        out.p[d] = std::clamp(center.p[d] + s * 1.6f * u, -1.5f, 1.5f);
    }
    return out;
}

// Finishing controls on top of the chosen look.
inline Look finish(Look l, float amount, float warmth, float punch)
{
    for (float& v : l.p) v *= amount;
    l.p[kWarm] += warmth;
    l.p[kContrast] += 0.8f * punch;
    l.p[kSat] += 0.3f * punch;
    l.p[kFilmSat] += 0.3f * punch;
    return l;
}

// Precomputed per-render constants for one look.
struct LookParams {
    float off[3];        // per channel offset, stops
    float contrast;
    float interimage;
    float dye;
    float hi, base;      // shoulder size (stops) and how much of the curve is used
    float fadeLift, crush;
    float sat;
    float shA, shB, hiA, hiB;
    float rot[3];        // radians for skin / green / blue
    // used by Color Look only (0 in Look Explorer, which keeps it bit-identical):
    float castA = 0.0f, castB = 0.0f; // tint centered on the mid tones, Oklab a / b
    float tintProtect = 0.0f;         // 0..1: tints fade out on already-colorful pixels
};

inline LookParams prepare(const Look& l, float strength)
{
    const float* p = l.p;
    LookParams k;
    k.off[0] = 0.25f * p[kWarm];
    k.off[1] = -0.15f * p[kTint];
    k.off[2] = -0.25f * p[kWarm];
    k.contrast = 1.0f + 0.35f * p[kContrast];
    k.interimage = 0.3f * p[kFilmSat];
    k.dye = 0.4f * p[kDye];
    float soft = std::clamp(0.4f + 0.4f * p[kShoulder], 0.0f, 1.0f);
    k.hi = 9.0f + (3.5f - 9.0f) * soft;
    k.base = std::clamp(strength, 0.0f, 1.0f); // fades the filmic curve in with Amount
    k.fadeLift = 0.012f * std::max(p[kFade], 0.0f);
    k.crush = 0.01f * std::max(-p[kFade], 0.0f);
    k.sat = std::max(1.0f + 0.4f * p[kSat], 0.0f);
    k.shA = 0.025f * p[kShadowA];
    k.shB = 0.025f * p[kShadowB];
    k.hiA = 0.02f * p[kHighA];
    k.hiB = 0.02f * p[kHighB];
    k.rot[0] = 0.35f * p[kHueSkin];
    k.rot[1] = 0.35f * p[kHueGreen];
    k.rot[2] = 0.35f * p[kHueBlue];
    return k;
}

inline float tanhCurve(float x, float hi) { return hi * std::tanh(x / hi); }

inline float hueDist(float a, float b)
{
    float d = a - b;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d;
}

// Input and output are DWG / DI code values.
inline V3 applyLook(V3 di, const LookParams& k)
{
    // log with a soft toe: log2(lin + eps) keeps 18% grey at 0 but flattens out near
    // black instead of diving to -17 stops (which would amplify noise in dark channels)
    const float eps = 0.0005f, g0 = 0.18f + eps;
    float lin[3] = {diToLin(di.x), diToLin(di.y), diToLin(di.z)};
    float x[3];
    for (int c = 0; c < 3; ++c) x[c] = std::log2(std::max(lin[c] + eps, 1.0e-6f) / g0) + k.off[c];

    // interimage and dye density, both in log
    float mean = (x[0] + x[1] + x[2]) / 3.0f;
    for (int c = 0; c < 3; ++c) x[c] += k.interimage * (x[c] - mean);
    float spread = std::max(x[0], std::max(x[1], x[2])) - std::min(x[0], std::min(x[1], x[2]));
    float dk = 1.5f * k.dye * (1.0f - std::exp2(-spread));

    float o[3];
    for (int c = 0; c < 3; ++c) {
        float y = (x[c] - dk) * k.contrast;
        y = y + k.base * (tanhCurve(y, y >= 0.0f ? k.hi : 10.0f) - y);
        o[c] = g0 * std::exp2(y) - eps;
    }

    // color work in Oklab
    V3 lab = xyzToOklab(mul(kDwgToXyz, {o[0], o[1], o[2]}));
    float L = std::max(lab.x, 0.0f);
    float a = lab.y * k.sat, b = lab.z * k.sat;
    float C = std::sqrt(a * a + b * b);
    if (C > 1.0e-5f) {
        float h = std::atan2(b, a);
        // Oklab hue of skin/orange ~ 55 deg, green ~ 140 deg, blue ~ -95 deg
        const float centers[3] = {0.96f, 2.44f, -1.66f};
        float dh = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float d = hueDist(h, centers[i]) / 0.6f;
            dh += k.rot[i] * std::exp(-d * d);
        }
        dh *= smoothstep(0.0f, 0.04f, C); // leave near-neutrals alone
        float cs = std::cos(dh), sn = std::sin(dh);
        float a2 = a * cs - b * sn;
        b = a * sn + b * cs;
        a = a2;
    }
    // split toning: grey (0.18 linear) sits at Oklab L ~ 0.56
    float ws = 1.0f - smoothstep(0.15f, 0.6f, L);
    float wh = smoothstep(0.5f, 0.95f, L);
    float fadeC = smoothstep(0.0f, 0.06f, L); // no tint on pure black
    if (k.tintProtect > 0.0f) fadeC *= 1.0f - k.tintProtect * smoothstep(0.04f, 0.14f, std::sqrt(a * a + b * b));
    float wc = 1.0f - 0.75f * std::max(ws, wh); // the cast lives mostly in the mid tones
    a += (k.shA * ws + k.hiA * wh + k.castA * wc) * fadeC;
    b += (k.shB * ws + k.hiB * wh + k.castB * wc) * fadeC;

    V3 d = mul(kXyzToDwg, oklabToXyz({L, a, b}));
    float r[3] = {d.x, d.y, d.z};
    for (float& v : r) {
        if (k.crush > 0.0f) v = std::max(v, 0.0f) * std::max(v, 0.0f) / (std::max(v, 0.0f) + k.crush);
        v += k.fadeLift;
    }
    return {linToDi(r[0]), linToDi(r[1]), linToDi(r[2])};
}

} // namespace drs
