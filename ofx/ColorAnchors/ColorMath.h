// ColorMath.h - pixel math for Color Anchors (no OFX dependency, so it can be tested alone)
//
// Working space: DaVinci Wide Gamut / DaVinci Intermediate (same as the DCTLs).
// Anchor colors are picked as display sRGB values and compared in Oklab, using
// lightness-normalized chroma (a/L, b/L) so a color matches regardless of exposure.
#pragma once

#include <algorithm>
#include <cmath>

namespace drs {

struct V3 {
    float x, y, z;
};

inline V3 mul(const float m[9], V3 v)
{
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// DaVinci Intermediate <-> linear
inline float diToLin(float t)
{
    if (t <= 0.02740668f) return t / 10.44426855f;
    return std::exp2(t / 0.07329248f - 7.0f) - 0.0075f;
}

inline float linToDi(float x)
{
    if (x <= 0.00262409f) return x * 10.44426855f;
    return (std::log2(x + 0.0075f) + 7.0f) * 0.07329248f;
}

inline float srgbToLin(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// clang-format off
static const float kDwgToXyz[9] = { 0.70062239f,  0.14877482f,  0.10105872f,
                                    0.27411851f,  0.87363190f, -0.14775041f,
                                   -0.09896291f, -0.13789533f,  1.32591599f};
static const float kXyzToDwg[9] = { 1.51667204f, -0.28147805f, -0.14696363f,
                                   -0.46491710f,  1.25142378f,  0.17488461f,
                                    0.06484905f,  0.10913934f,  0.76141462f};
static const float kSrgbToXyz[9] = {0.4124564f, 0.3575761f, 0.1804375f,
                                    0.2126729f, 0.7151522f, 0.0721750f,
                                    0.0193339f, 0.1191920f, 0.9503041f};
// Oklab (Ottosson), XYZ D65 based
static const float kOkM1[9] = {0.8189330101f, 0.3618667424f, -0.1288597137f,
                               0.0329845436f, 0.9293118715f,  0.0361456387f,
                               0.0482003018f, 0.2643662691f,  0.6338517070f};
static const float kOkM2[9] = {0.2104542553f,  0.7936177850f, -0.0040720468f,
                               1.9779984951f, -2.4285922050f,  0.4505937099f,
                               0.0259040371f,  0.7827717662f, -0.8086757660f};
static const float kOkM2Inv[9] = {1.0f,  0.3963377774f,  0.2158037573f,
                                  1.0f, -0.1055613458f, -0.0638541728f,
                                  1.0f, -0.0894841775f, -1.2914855480f};
static const float kOkM1Inv[9] = { 1.2270138511f, -0.5577999807f,  0.2812561490f,
                                  -0.0405801784f,  1.1122568696f, -0.0716766787f,
                                  -0.0763812845f, -0.4214819784f,  1.5861632204f};
// clang-format on

inline V3 xyzToOklab(V3 xyz)
{
    V3 lms = mul(kOkM1, xyz);
    return mul(kOkM2, {std::cbrt(lms.x), std::cbrt(lms.y), std::cbrt(lms.z)});
}

inline V3 oklabToXyz(V3 lab)
{
    V3 l = mul(kOkM2Inv, lab);
    return mul(kOkM1Inv, {l.x * l.x * l.x, l.y * l.y * l.y, l.z * l.z * l.z});
}

inline float smoothstep(float e0, float e1, float x)
{
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// One "from -> to" pair, precomputed once per render
struct Anchor {
    bool on = false;
    float su = 0, sv = 0, sL = 1; // source: normalized chroma + lightness
    float du = 0, dv = 0;         // chroma move
    float lnRatio = 0;            // lightness move, ln(L_to / L_from)
    float invRange2 = 1;
    float strength = 1;
    float matchL = 0;
};

inline Anchor makeAnchor(bool on, V3 fromSrgb, V3 toSrgb, float range, float strength, float matchL)
{
    auto toLab = [](V3 c) {
        return xyzToOklab(mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)}));
    };
    V3 f = toLab(fromSrgb);
    V3 t = toLab(toSrgb);
    float fL = std::max(f.x, 1.0e-3f);
    float tL = std::max(t.x, 1.0e-3f);

    Anchor a;
    a.on = on;
    a.su = f.y / fL;
    a.sv = f.z / fL;
    a.sL = fL;
    a.du = t.y / tL - a.su;
    a.dv = t.z / tL - a.sv;
    a.lnRatio = std::log(tL / fL);
    float r = std::max(range, 1.0e-3f);
    a.invRange2 = 1.0f / (r * r);
    a.strength = strength;
    a.matchL = matchL;
    return a;
}

constexpr int kMaxAnchors = 6;

struct Settings {
    Anchor anchors[kMaxAnchors];
    float protect = 0.03f; // neutral protection, in normalized chroma
    float mix = 1.0f;
    bool showMask = false;
};

// Input and output are DWG / DI code values.
inline V3 processPixel(V3 di, const Settings& s)
{
    V3 lin = {diToLin(di.x), diToLin(di.y), diToLin(di.z)};
    V3 lab = xyzToOklab(mul(kDwgToXyz, lin));
    float L = lab.x;

    float sumW = 0, sumDu = 0, sumDv = 0, sumLn = 0;
    if (L > 1.0e-4f) {
        float u = lab.y / L;
        float v = lab.z / L;
        // fade out in deep blacks (chroma there is mostly noise) and on neutrals
        float gate = smoothstep(0.02f, 0.1f, L);
        if (s.protect > 0.0f) gate *= smoothstep(0.0f, s.protect, std::sqrt(u * u + v * v));

        for (const Anchor& a : s.anchors) {
            if (!a.on || a.strength <= 0.0f) continue;
            float eu = u - a.su;
            float ev = v - a.sv;
            float lt = std::log(L / a.sL) / 0.35f; // ~1.5 stops per unit
            float d2 = (eu * eu + ev * ev) * a.invRange2 + a.matchL * lt * lt;
            float w = a.strength * gate * std::exp(-d2);
            sumW += w;
            sumDu += w * a.du;
            sumDv += w * a.dv;
            sumLn += w * a.lnRatio;
        }
    }

    if (s.showMask) {
        float m = std::min(sumW, 1.0f);
        return {m, m, m};
    }
    if (sumW <= 0.0f) return di;

    // overlapping anchors share instead of stacking
    float k = sumW > 1.0f ? 1.0f / sumW : 1.0f;
    float u2 = lab.y / L + k * sumDu;
    float v2 = lab.z / L + k * sumDv;
    float L2 = L * std::exp(k * sumLn);

    V3 xyz = oklabToXyz({L2, u2 * L2, v2 * L2});
    V3 out = mul(kXyzToDwg, xyz);
    V3 o = {linToDi(out.x), linToDi(out.y), linToDi(out.z)};
    return {di.x + (o.x - di.x) * s.mix, di.y + (o.y - di.y) * s.mix, di.z + (o.z - di.z) * s.mix};
}

} // namespace drs
