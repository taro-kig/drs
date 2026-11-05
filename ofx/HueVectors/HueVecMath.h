// HueVecMath.h - pixel math for Hue Vectors (no OFX dependency, so it can be tested alone)
//
// The three HueVec DCTLs (Density, Sat, Hue) in one chain, plus a master "Richness"
// control. Each stage is a straight port of the shared DCTL core, and the stages run in
// the recommended order Density -> Sat -> Hue, so the result matches three DCTL nodes.
//
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.
#pragma once

#include <algorithm>
#include <cmath>

#include "ColorMath.h"

namespace drs {
namespace huevec {

enum Kind { kDensity = 0, kSat = 1, kHue = 2 };

struct Vectors {
    float v[6] = {0, 0, 0, 0, 0, 0}; // Red, Yellow, Green, Cyan, Blue, Magenta
    float glob = 0.0f;               // all hues
    float range = 1.0f;              // 1 = seamless between neighbors, lower isolates

    bool any() const
    {
        if (glob != 0.0f) return true;
        for (float x : v)
            if (x != 0.0f) return true;
        return false;
    }
};

struct Settings {
    float richness = 0.0f; // master: -1..1
    float balance = 0.5f;  // 0 = saturation only, 0.5 = both in full, 1 = density only
    float deep = 0.0f;     // keep highlights clean, 0..1
    float mix = 1.0f;
    bool showDeep = false; // excluded highlights in magenta

    Vectors density;
    Vectors sat;
    Vectors hue; // glob unused
};

// Richness split into the global density and global saturation it adds.
// Balance 0.5 gives both in full, so Richness 1 = Global Den 1 + Global Sat 1.
inline void richnessSplit(float richness, float balance, float& den, float& sat)
{
    float b = std::min(std::max(balance, 0.0f), 1.0f);
    den = richness * std::min(1.0f, 2.0f * b);
    sat = richness * std::min(1.0f, 2.0f * (1.0f - b));
}

inline float lumaDwg(V3 c) { return 0.27411851f * c.x + 0.87363190f * c.y - 0.14775041f * c.z; }

inline float smooth01(float e0, float e1, float x)
{
    float t = std::min(std::max((x - e0) / (e1 - e0), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// weight of the vector at `center` (0..5) for hue h (0..6), raised-cosine window
inline float vecWeight(float h, float center, float range)
{
    float d = h - center;
    if (d > 3.0f) d -= 6.0f;
    if (d < -3.0f) d += 6.0f;
    float a = std::fabs(d) / range;
    if (a >= 1.0f) return 0.0f;
    float c = std::cos(1.5707963f * a);
    return c * c;
}

// hue (0..6) / saturation / value -> RGB; S may exceed 1 for out-of-gamut input
inline V3 hsvToRgb(float h, float s, float v)
{
    float c = v * s;
    float hm = h - 2.0f * std::floor(h * 0.5f);
    float x = c * (1.0f - std::fabs(hm - 1.0f));
    float m = v - c;
    V3 o;
    if (h < 1.0f)      o = {c, x, 0.0f};
    else if (h < 2.0f) o = {x, c, 0.0f};
    else if (h < 3.0f) o = {0.0f, c, x};
    else if (h < 4.0f) o = {0.0f, x, c};
    else if (h < 5.0f) o = {x, 0.0f, c};
    else               o = {c, 0.0f, x};
    return {o.x + m, o.y + m, o.z + m};
}

// Deep: 1 = full effect, fading to 0 above a cutoff that drops as Deep rises
inline float deepKeep(float Y0, float deep)
{
    if (deep <= 0.0f) return 1.0f;
    float ev = std::log2(std::max(Y0, 1.0e-6f) / 0.18f);
    float cut = 5.0f - 6.0f * deep;
    return 1.0f - smooth01(cut - 1.5f, cut + 1.0f, ev);
}

// excluded highlights in magenta over a grey picture (DI in, DI out)
inline V3 deepView(V3 di, float deep)
{
    V3 lin = {diToLin(di.x), diToLin(di.y), diToLin(di.z)};
    float Y0 = lumaDwg(lin);
    float e = 1.0f - deepKeep(Y0, deep);
    float g = std::max(Y0, 0.0f);
    return {linToDi(g + (0.9f - g) * e), linToDi(g + (0.05f - g) * e), linToDi(g + (0.7f - g) * e)};
}

// One stage, the same as one HueVec_*.dctl node. DI in, DI out.
inline V3 stage(V3 di, Kind kind, const Vectors& vec, float deep)
{
    V3 lin = {diToLin(di.x), diToLin(di.y), diToLin(di.z)};
    float mx = std::max(lin.x, std::max(lin.y, lin.z));
    float mn = std::min(lin.x, std::min(lin.y, lin.z));
    float Y0 = lumaDwg(lin);
    float keep = deepKeep(Y0, deep);

    float chroma = mx - mn;
    if (mx <= 1.0e-6f || chroma <= 1.0e-7f) return di;

    // hexcone hue 0..6 on DWG primaries
    float h;
    if (mx == lin.x)      h = (lin.y - lin.z) / chroma;
    else if (mx == lin.y) h = 2.0f + (lin.z - lin.x) / chroma;
    else                  h = 4.0f + (lin.x - lin.y) / chroma;
    if (h < 0.0f) h += 6.0f;
    float s = chroma / mx;

    float amt = kind == kHue ? 0.0f : vec.glob;
    for (int i = 0; i < 6; ++i) amt += vecWeight(h, float(i), vec.range) * vec.v[i];
    amt *= keep;

    float h2 = h;
    float s2 = s;
    float dens = 1.0f;
    float up = 0.0f;
    float down = 0.0f;
    if (kind == kHue) {
        h2 = h + 0.5f * amt; // full scale = halfway (30 deg) to the neighbor
        if (h2 < 0.0f) h2 += 6.0f;
        if (h2 >= 6.0f) h2 -= 6.0f;
    } else if (kind == kSat) {
        up = std::max(amt, 0.0f);
        down = std::min(amt, 0.0f);
    } else {
        float d = amt * std::min(s, 1.0f); // neutrals get no density
        dens = std::exp2(-1.5f * d);        // about 1.5 stops darker at full
        up = 0.25f * std::max(d, 0.0f);     // a little subtractive saturation stays rich
    }
    // increases approach full saturation without passing it (stays in gamut);
    // input already outside (s >= 1) is only ever reduced
    if (up > 0.0f && s < 1.0f) s2 = 1.0f - std::pow(1.0f - s, 1.0f + up);
    s2 = std::max(s2 * (1.0f + down), 0.0f);

    // rebuild, keep the original luminance (gain limited to 1/4..4), then apply density.
    // Pushing violets toward the DWG blue primary can take the luma to zero or below; the
    // gain then stays at its limit instead of snapping back to 1, which in the DCTLs made a
    // hard edge in saturated blues. Everything else is unchanged from the DCTLs.
    V3 o = hsvToRgb(h2, s2, mx);
    float Y1 = lumaDwg(o);
    if (Y0 > 0.0f) {
        float k = Y1 <= 0.25f * Y0 ? 4.0f : std::min(std::max(Y0 / Y1, 0.25f), 4.0f);
        o = {o.x * k, o.y * k, o.z * k};
    }
    return {linToDi(o.x * dens), linToDi(o.y * dens), linToDi(o.z * dens)};
}

// The whole plugin: Richness folded into the global values, then Density -> Sat -> Hue.
inline V3 processPixel(V3 di, const Settings& s)
{
    if (s.showDeep) return deepView(di, s.deep);

    Vectors den = s.density, sat = s.sat;
    float rd, rs;
    richnessSplit(s.richness, s.balance, rd, rs);
    den.glob += rd;
    sat.glob += rs;

    V3 o = di;
    if (den.any()) o = stage(o, kDensity, den, s.deep);
    if (sat.any()) o = stage(o, kSat, sat, s.deep);
    if (s.hue.any()) o = stage(o, kHue, s.hue, s.deep);

    const float m = s.mix;
    return {di.x + (o.x - di.x) * m, di.y + (o.y - di.y) * m, di.z + (o.z - di.z) * m};
}

inline bool isIdentity(const Settings& s)
{
    if (s.showDeep) return false;
    if (s.mix <= 0.0f) return true;
    return s.richness == 0.0f && !s.density.any() && !s.sat.any() && !s.hue.any();
}

} // namespace huevec
} // namespace drs
