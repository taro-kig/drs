// ColorLookMath.h - "pick colors, get a whole look" (no OFX dependency, tested alone)
//
// Seven color slots, one per Color Mixer band (Red, Orange, Yellow, Green, Aqua, Blue,
// Magenta). Each starts at a reference color for its band. Turn a slot on and pick a color:
//   1. that band of the picture moves to the picked hue, saturation and lightness
//   2. all picked colors together (the palette) set the whole look:
//      lightest color -> highlight tint      darkest color -> shadow tint
//      (one color only: highlights lean to it, shadows to its opposite, amber -> teal)
//      average hue    -> overall warmth / tint
//      average chroma -> richness (film-style separation, dye density, saturation)
//      average lightness and dark/light spread -> mood: deep and punchy, or soft and airy
// Auto Tone measures the frame (median and 5-95% range of luminance, in stops) and sets
// exposure and contrast so the picture lands where the mood wants it.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ColorMath.h"
#include "HeroMath.h"
#include "LookMath.h"
#include "MixerMath.h"

namespace drs {

struct KeyInfo {
    float L, C, h; // Oklab lightness, chroma, hue (radians)
};

inline KeyInfo keyFromSrgb(V3 srgb)
{
    V3 lab = xyzToOklab(mul(kSrgbToXyz, {srgbToLin(srgb.x), srgbToLin(srgb.y), srgbToLin(srgb.z)}));
    return {lab.x, std::sqrt(lab.y * lab.y + lab.z * lab.z), std::atan2(lab.z, lab.y)};
}

inline float wrapPi(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

inline float colorfulness(const KeyInfo& k) { return std::clamp(k.C / 0.12f, 0.0f, 1.0f); }

// Mood: +1 = dark, deep, punchy palette; -1 = light, airy palette.
inline float paletteMood(const std::vector<KeyInfo>& keys)
{
    if (keys.empty()) return 0.0f;
    float sumL = 0;
    for (const KeyInfo& k : keys) sumL += k.L;
    return std::clamp((0.62f - sumL / keys.size()) / 0.25f, -1.0f, 1.0f);
}

inline Look lookFromPalette(const std::vector<V3>& srgb)
{
    Look l;
    if (srgb.empty()) return l;
    const float deg = 0.01745329f;
    std::vector<KeyInfo> keys;
    for (V3 c : srgb) keys.push_back(keyFromSrgb(c));
    float* p = l.p;

    // highlight / shadow tints
    const KeyInfo* hiK = &keys[0];
    const KeyInfo* loK = &keys[0];
    for (const KeyInfo& k : keys) {
        if (k.L > hiK->L) hiK = &k;
        if (k.L < loK->L) loK = &k;
    }
    float hs = colorfulness(*hiK);
    p[kHighA] = 1.4f * hs * std::cos(hiK->h);
    p[kHighB] = 1.4f * hs * std::sin(hiK->h);
    if (keys.size() == 1) {
        float sh = hiK->h + 165.0f * deg; // opposite, a bit toward cyan
        p[kShadowA] = 1.1f * hs * std::cos(sh);
        p[kShadowB] = 1.1f * hs * std::sin(sh);
    } else {
        float ls = colorfulness(*loK);
        p[kShadowA] = 1.5f * ls * std::cos(loK->h);
        p[kShadowB] = 1.5f * ls * std::sin(loK->h);
    }

    // overall cast from the chroma-weighted average hue
    float ax = 0, ay = 0, sumC = 0, sumL = 0, minL = 1, maxL = 0;
    for (const KeyInfo& k : keys) {
        float s = colorfulness(k);
        ax += s * std::cos(k.h);
        ay += s * std::sin(k.h);
        sumC += k.C;
        sumL += k.L;
        minL = std::min(minL, k.L);
        maxL = std::max(maxL, k.L);
    }
    float n = float(keys.size());
    float castS = std::min(std::sqrt(ax * ax + ay * ay) / n, 1.0f);
    float castH = std::atan2(ay, ax);
    p[kWarm] = 0.45f * castS * std::cos(castH - 60.0f * deg);
    p[kTint] = 0.25f * castS * std::cos(castH - 350.0f * deg);

    // richness
    float vivid = std::clamp((sumC / n - 0.07f) / 0.08f, -1.0f, 1.0f);
    float rich = std::clamp((sumC / n) / 0.12f, 0.0f, 1.0f);
    p[kSat] = 0.35f * vivid;
    p[kFilmSat] = 0.25f + 0.35f * rich;
    p[kDye] = 0.25f + 0.45f * rich;

    // mood: average lightness, plus contrast from how far apart dark and light colors are
    float mood = paletteMood(keys);
    float spread = keys.size() > 1 ? (maxL - minL) : 0.35f;
    p[kContrast] = 0.6f * mood + 0.8f * (spread - 0.35f);
    p[kFade] = -0.5f * mood + (mood < 0.0f ? 0.3f * -mood : 0.0f);
    p[kShoulder] = 0.5f - 0.3f * mood;

    // a single warm color: greens get the filmic teal lean
    if (keys.size() == 1) p[kHueGreen] = 0.35f * hs * std::cos(hiK->h - 60.0f * deg);
    return l;
}

// Luminance statistics of the frame, in stops from 18% grey.
struct ToneStats {
    float median = 0.0f, p05 = -4.0f, p95 = 3.0f;
};

constexpr int kHistBins = 256;
constexpr float kHistLo = -12.0f, kHistHi = 12.0f;

inline int histBin(float stops)
{
    int b = int((stops - kHistLo) / (kHistHi - kHistLo) * kHistBins);
    return std::clamp(b, 0, kHistBins - 1);
}

inline float lumaStops(V3 di)
{
    float Y = 0.27411851f * diToLin(di.x) + 0.87363190f * diToLin(di.y) - 0.14775041f * diToLin(di.z);
    return std::log2(std::max(Y, 1.0e-6f) / 0.18f);
}

inline ToneStats statsFromHistogram(const std::vector<uint32_t>& hist)
{
    ToneStats t;
    uint64_t total = 0;
    for (uint32_t v : hist) total += v;
    if (total == 0) return t;
    auto pct = [&](double q) {
        uint64_t target = uint64_t(q * double(total)), acc = 0;
        for (int b = 0; b < int(hist.size()); ++b) {
            acc += hist[b];
            if (acc > target) return kHistLo + (b + 0.5f) * (kHistHi - kHistLo) / kHistBins;
        }
        return kHistHi;
    };
    t.p05 = pct(0.05);
    t.median = pct(0.5);
    t.p95 = pct(0.95);
    return t;
}

// Per-pixel tone remap from Auto Tone (equal in all channels, so hues are kept).
struct ToneMap {
    float median = 0.0f, target = 0.0f, k = 1.0f, strength = 0.0f;
};

inline ToneMap autoTone(const ToneStats& st, float mood, float strength, float brightness)
{
    ToneMap m;
    if (strength <= 0.0f) { // Brightness alone still works without Auto Tone
        m.target = brightness;
        m.strength = 1.0f;
        return m;
    }
    m.median = st.median;
    // where the middle of the picture should sit: lower for a dark palette, higher for a light one
    m.target = -0.5f - 0.6f * mood + brightness;
    float range = std::max(st.p95 - st.p05, 1.0f);
    m.k = std::clamp(6.5f / range, 0.75f, 1.6f); // aim for about 6.5 stops between 5% and 95%
    m.strength = std::min(strength, 1.0f);
    return m;
}

inline V3 applyTone(V3 di, const ToneMap& m)
{
    if (m.strength <= 0.0f || (m.k == 1.0f && m.target == m.median)) return di;
    const float eps = 0.0005f, g0 = 0.18f + eps;
    float in[3] = {di.x, di.y, di.z}, out[3];
    for (int c = 0; c < 3; ++c) {
        float lin = diToLin(in[c]);
        float x = std::log2(std::max(lin + eps, 1.0e-6f) / g0);
        float y = m.target + (x - m.median) * m.k;
        x = x + m.strength * (y - x);
        out[c] = linToDi(g0 * std::exp2(x) - eps);
    }
    return {out[0], out[1], out[2]};
}

// --- the seven band slots ---

constexpr float kRefL = 0.68f, kRefC = 0.11f; // reference color of each band (Oklab)

inline float srgbEncode(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

// display sRGB of a band's reference color (the slot's starting swatch)
inline V3 bandReferenceSrgb(int band)
{
    static const float kXyzToSrgb[9] = {3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
                                        0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f};
    float h = kMixCenters[band] * 0.01745329f;
    V3 lin = mul(kXyzToSrgb, oklabToXyz({kRefL, kRefC * std::cos(h), kRefC * std::sin(h)}));
    return {srgbEncode(lin.x), srgbEncode(lin.y), srgbEncode(lin.z)};
}

// Color Mixer moves that take a band from its reference color to the picked one
inline void bandMoveFromPick(int band, V3 pickSrgb, MixerSettings& m)
{
    KeyInfo k = keyFromSrgb(pickSrgb);
    float dh = wrapPi(k.h - kMixCenters[band] * 0.01745329f) * 57.29578f;
    m.hue[band] = std::clamp(dh, -60.0f, 60.0f) / 25.0f; // mixer: 25 degrees per unit
    m.sat[band] = std::clamp(k.C / kRefC - 1.0f, -1.0f, 1.5f);
    m.lum[band] = std::clamp(3.0f * std::log2(std::max(k.L, 0.05f) / kRefL), -1.5f, 1.5f);
}

struct ColorLookSettings {
    MixerSettings mixer;
    LookParams look;
    ToneMap tone;
};

// hero[i]: how much each color leads the look (0..1), decided from the frame stats `cs`.
// on[i] / pick[i]: the seven manual slots (display sRGB swatches).
inline ColorLookSettings makeColorLook(const float hero[kMixColors], const ColorStats& cs, const bool on[kMixColors],
                                       const V3 pick[kMixColors], float amount, float contrast, float brightness,
                                       float autoStrength, const ToneStats& stats, int placement = kPlaceAuto)
{
    ColorLookSettings s;
    const HeroPlan plan = planHero(hero, cs, placement);
    heroMixer(plan, hero, cs, s.mixer);
    std::vector<V3> palette;
    std::vector<KeyInfo> keys;
    for (int i = 0; i < kMixColors; ++i) {
        if (!on[i]) continue;
        palette.push_back(pick[i]);
        keys.push_back(keyFromSrgb(pick[i]));
        bandMoveFromPick(i, pick[i], s.mixer);
    }
    float a = std::clamp(amount, 0.0f, 1.5f);
    for (int i = 0; i < kMixColors; ++i) {
        s.mixer.hue[i] *= a;
        s.mixer.sat[i] *= a;
        s.mixer.lum[i] *= a;
    }
    Look base = lookFromPalette(palette);
    heroLookDims(plan, base);
    Look l = finish(base, amount, 0.0f, 0.0f);
    l.p[kContrast] += contrast;
    s.look = prepare(l, amount);
    heroTints(plan, amount, s.look);
    s.tone = autoTone(stats, paletteMood(keys), autoStrength, brightness);
    return s;
}

// manual slots only (Color Look 1.0 behavior)
inline ColorLookSettings makeColorLook(const bool on[kMixColors], const V3 pick[kMixColors], float amount,
                                       float contrast, float brightness, float autoStrength, const ToneStats& stats)
{
    const float none[kMixColors] = {};
    return makeColorLook(none, ColorStats{}, on, pick, amount, contrast, brightness, autoStrength, stats);
}

inline V3 colorLookPixel(V3 di, const ColorLookSettings& s)
{
    V3 t = applyTone(di, s.tone);
    if (!mixerIsIdentity(s.mixer)) t = mixPixel(t, s.mixer);
    return applyLook(t, s.look);
}

} // namespace drs
