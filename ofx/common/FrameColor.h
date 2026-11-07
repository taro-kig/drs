// FrameColor.h - frame measurements and the color math behind the utility plugins
// (Auto Neutral, Shot Match, Color Keeper, Palette Card). No OFX dependency, so it can be
// tested alone. Everything works on a sparse sample of the frame (about 480 pixels across,
// gathered on the GPU or read on the CPU), measured exactly in Oklab: histograms with
// 10-degree hue bins were too coarse to see a small hue drift.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ColorLookMath.h" // ToneStats, lumaStops, colorBin, chromaBinCenter, ColorMath

namespace drs {

inline V3 diToLin3(V3 di) { return {diToLin(di.x), diToLin(di.y), diToLin(di.z)}; }
inline V3 linToDi3(V3 l) { return {linToDi(l.x), linToDi(l.y), linToDi(l.z)}; }
inline float lumaDwgLin(V3 l) { return 0.27411851f * l.x + 0.87363190f * l.y - 0.14775041f * l.z; }
inline V3 linToLab(V3 l) { return xyzToOklab(mul(kDwgToXyz, l)); }
inline V3 labToLin(V3 lab) { return mul(kXyzToDwg, oklabToXyz(lab)); }

// A sparse sample of the frame, measured once.
struct FrameSamples {
    std::vector<V3> lab;      // Oklab (NaN-free)
    std::vector<float> stops; // luminance in stops from 18% grey
};

inline FrameSamples measureSamples(const std::vector<V3>& di)
{
    FrameSamples f;
    f.lab.reserve(di.size());
    f.stops.reserve(di.size());
    for (V3 v : di) {
        V3 l = diToLin3(v);
        V3 lab = linToLab(l);
        if (!(lab.x == lab.x) || !(lab.y == lab.y) || !(lab.z == lab.z)) continue;
        f.lab.push_back(lab);
        f.stops.push_back(std::log2(std::max(lumaDwgLin(l), 1.0e-6f) / 0.18f));
    }
    return f;
}

inline float chromaOf(V3 lab) { return std::sqrt(lab.y * lab.y + lab.z * lab.z); }

// ---------------------------------------------------------------- light color (white balance)

// Color cast of the light, read like a "white patch": bright, low-chroma pixels count most
// (white walls, paper, grey stone), true greys included so a neutral scene reads neutral,
// colorful things (sky, skin, wood) barely at all.
struct Cast {
    float a = 0, b = 0;
    float support = 0; // share of the frame that is near-neutral (what the estimate rests on)
};

// Measured as chroma relative to lightness (a/L, b/L): under one light, every grey surface
// from dark to bright lands on the same point there, so the light shows up as the densest
// cluster of near-neutral pixels. The cast is returned at L = 0.6 (what wbGains expects).
inline Cast castFromSamples(const FrameSamples& f)
{
    Cast c;
    if (f.lab.empty()) return c;
    std::vector<float> u, v, w;
    double n = 0;
    for (V3 lab : f.lab) {
        if (lab.x < 0.25f) continue; // dark pixels: noise, not light
        float pu = lab.y / lab.x, pv = lab.z / lab.x;
        float C = std::sqrt(pu * pu + pv * pv);
        if (C > 0.18f) continue; // clearly colored things
        u.push_back(pu), v.push_back(pv), w.push_back(lab.x * lab.x); // brighter surfaces count more
        if (chromaOf(lab) < 0.08f) n += 1;
    }
    if (u.empty()) return c;
    // seed: the densest point (checked on a subset), then mean-shift to the cluster's center
    const float r2 = 0.02f * 0.02f;
    size_t sub = std::max<size_t>(1, u.size() / 400), best = 0;
    double bestD = -1;
    for (size_t i = 0; i < u.size(); i += sub) {
        double d = 0;
        for (size_t j = 0; j < u.size(); j += sub) {
            float du = u[j] - u[i], dv = v[j] - v[i];
            if (du * du + dv * dv < r2) d += w[j];
        }
        if (d > bestD) bestD = d, best = i;
    }
    double eu = u[best], ev = v[best];
    const double sigma2 = 2.0 * 0.02 * 0.02;
    for (int it = 0; it < 10; ++it) {
        double nu = 0, nv = 0, nw = 0;
        for (size_t i = 0; i < u.size(); ++i) {
            double du = u[i] - eu, dv = v[i] - ev;
            double k = w[i] * std::exp(-(du * du + dv * dv) / sigma2);
            nu += k * u[i], nv += k * v[i], nw += k;
        }
        if (nw <= 0) break;
        eu = nu / nw, ev = nv / nw;
    }
    c.a = float(eu * 0.6);
    c.b = float(ev * 0.6);
    c.support = float(n / double(f.lab.size()));
    return c;
}

// Per-channel gains (linear DWG) that remove an Oklab cast (a, b) from a grey, keeping its
// luminance. Gains for adding a cast are the reciprocals.
inline V3 wbGains(float a, float b)
{
    V3 c = labToLin({0.6f, a, b});
    float Y = lumaDwgLin(c);
    auto g = [Y](float v) { return v > 1.0e-6f ? std::clamp(Y / v, 0.25f, 4.0f) : 1.0f; };
    return {g(c.x), g(c.y), g(c.z)};
}

inline V3 powGains(V3 g, float s) { return {std::pow(g.x, s), std::pow(g.y, s), std::pow(g.z, s)}; }

// scale gains so a grey carrying the cast (a, b) keeps its luminance through them
inline V3 keepLuma(V3 g, float a, float b)
{
    V3 c = labToLin({0.6f, a, b});
    float Y0 = lumaDwgLin(c), Y1 = lumaDwgLin({c.x * g.x, c.y * g.y, c.z * g.z});
    float k = Y1 > 1.0e-6f && Y0 > 1.0e-6f ? Y0 / Y1 : 1.0f;
    return {g.x * k, g.y * k, g.z * k};
}

// ---------------------------------------------------------------- Auto Neutral

struct NeutralSettings {
    V3 gains = {1, 1, 1}; // final per-channel gains, linear DWG
};

// strength: how much of the measured cast to remove; warmth / tint: -1..1 trims on top
// (+ warmth = amber, + tint = magenta).
inline NeutralSettings makeNeutral(const Cast& cast, float strength, float warmth, float tint)
{
    NeutralSettings s;
    // no near-neutral surfaces to read the light from (a frame full of foliage, say): don't guess
    float evidence = smoothstep(0.005f, 0.03f, cast.support);
    V3 remove = powGains(wbGains(cast.a, cast.b), std::clamp(strength, 0.0f, 1.5f) * evidence);
    // trims: add a cast of their own (Oklab: amber ~ +b, magenta ~ +a)
    float ta = 0.012f * tint, tb = 0.02f * warmth;
    V3 add = wbGains(ta, tb);
    s.gains = {remove.x / add.x, remove.y / add.y, remove.z / add.z};
    // keep exposure: the surface the cast was read from keeps its luminance
    s.gains = keepLuma(s.gains, cast.a, cast.b);
    return s;
}

inline V3 neutralPixel(V3 di, const NeutralSettings& s)
{
    V3 l = diToLin3(di);
    return linToDi3({l.x * s.gains.x, l.y * s.gains.y, l.z * s.gains.z});
}

// ---------------------------------------------------------------- Shot Match

// What a shot looks like, for matching: tone (stops), light color, colorfulness.
struct ShotStats {
    ToneStats tone;
    Cast cast;
    float chroma = 0.05f; // mean Oklab chroma of the colored part of the frame
    bool valid = false;
};

// mean chroma of the colored part of the frame; with `gains`, as it will be after those
// per-channel gains
inline float meanChroma(const FrameSamples& f, const V3* gains = nullptr)
{
    double s = 0, n = 0;
    for (V3 lab : f.lab) {
        if (gains) {
            V3 l = labToLin(lab);
            lab = linToLab({l.x * gains->x, l.y * gains->y, l.z * gains->z});
            if (!(lab.x == lab.x)) continue;
        }
        if (lab.x < 0.15f) continue;
        float C = chromaOf(lab);
        double w = smoothstep(0.015f, 0.04f, C); // greys don't dilute it
        s += w * C;
        n += w;
    }
    return n > 0 ? float(s / n) : 0.05f;
}

inline ToneStats toneFromSamples(const FrameSamples& f)
{
    ToneStats t;
    if (f.stops.empty()) return t;
    std::vector<float> s = f.stops;
    auto pct = [&](double q) {
        size_t k = std::min(s.size() - 1, size_t(q * double(s.size())));
        std::nth_element(s.begin(), s.begin() + long(k), s.end());
        return s[k];
    };
    t.p05 = pct(0.05);
    t.median = pct(0.5);
    t.p95 = pct(0.95);
    return t;
}

inline ShotStats shotStats(const FrameSamples& f)
{
    ShotStats s;
    s.tone = toneFromSamples(f);
    s.cast = castFromSamples(f);
    s.chroma = meanChroma(f);
    s.valid = !f.lab.empty();
    return s;
}

struct MatchSettings {
    // tone: piecewise-linear map in stops through (p05, median, p95)
    float x[3] = {-4, 0, 3}, y[3] = {-4, 0, 3};
    float slopeLo = 1, slopeHi = 1;
    float toneAmt = 0;
    V3 gains = {1, 1, 1}; // light color: current cast out, reference cast in
    float chromaScale = 1;
};

// curSamples (optional): the current frame, so colorfulness is compared after the light
// color has been matched (removing a cast also removes some chroma)
inline MatchSettings makeMatch(const ShotStats& cur, const ShotStats& ref, float exposure, float color, float sat,
                               const FrameSamples* curSamples = nullptr)
{
    MatchSettings m;
    if (!cur.valid || !ref.valid) return m;
    m.toneAmt = std::clamp(exposure, 0.0f, 1.0f);
    m.x[0] = cur.tone.p05, m.x[1] = cur.tone.median, m.x[2] = cur.tone.p95;
    m.y[0] = ref.tone.p05, m.y[1] = ref.tone.median, m.y[2] = ref.tone.p95;
    // keep the curve sane: each segment's slope 0.5..2; outside the measured range (below p05,
    // above p95) the curve continues at slope 1, so extremes are offset, never stretched
    auto slope = [](float dy, float dx) { return dx > 0.1f ? std::clamp(dy / dx, 0.5f, 2.0f) : 1.0f; };
    m.slopeLo = slope(m.y[1] - m.y[0], m.x[1] - m.x[0]);
    m.slopeHi = slope(m.y[2] - m.y[1], m.x[2] - m.x[1]);
    m.y[0] = m.y[1] - m.slopeLo * (m.x[1] - m.x[0]);
    m.y[2] = m.y[1] + m.slopeHi * (m.x[2] - m.x[1]);

    float c = std::clamp(color, 0.0f, 1.0f);
    V3 out = wbGains(cur.cast.a, cur.cast.b), in = wbGains(ref.cast.a, ref.cast.b);
    m.gains = keepLuma(powGains({out.x / in.x, out.y / in.y, out.z / in.z}, c), cur.cast.a, cur.cast.b);

    float curChroma = curSamples ? meanChroma(*curSamples, &m.gains) : cur.chroma;
    float k = std::clamp(ref.chroma / std::max(curChroma, 1.0e-4f), 0.7f, 1.4f);
    m.chromaScale = std::pow(k, std::clamp(sat, 0.0f, 1.0f));
    return m;
}

inline float matchStops(float s, const MatchSettings& m)
{
    float o;
    if (s < m.x[0]) o = m.y[0] + (s - m.x[0]);
    else if (s <= m.x[1]) o = m.y[1] + (s - m.x[1]) * m.slopeLo;
    else if (s <= m.x[2]) o = m.y[1] + (s - m.x[1]) * m.slopeHi;
    else o = m.y[2] + (s - m.x[2]);
    return s + (o - s) * m.toneAmt;
}

inline V3 matchPixel(V3 di, const MatchSettings& m)
{
    V3 l = diToLin3(di);
    // tone: one gain per pixel from its luminance, so hues stay put
    // (deep blues can have zero or negative DWG luminance: they get the gain of the darkest
    // tones, which is constant below p05, so there is no jump)
    float s = std::log2(std::max(lumaDwgLin(l), 1.0e-6f) / 0.18f);
    float g = std::exp2(matchStops(s, m) - s);
    l = {l.x * g, l.y * g, l.z * g};
    l = {l.x * m.gains.x, l.y * m.gains.y, l.z * m.gains.z};
    if (m.chromaScale != 1.0f) {
        V3 lab = linToLab(l);
        if (lab.x == lab.x) {
            // more saturation goes mostly to the less saturated colors (vibrance-like): colors
            // already near the edge would only clip
            float kc = m.chromaScale;
            if (kc > 1.0f) kc = 1.0f + (kc - 1.0f) * (1.0f - 0.8f * smoothstep(0.08f, 0.22f, chromaOf(lab)));
            // never push a channel further below zero than it already is (extreme colors near
            // the edge of DWG): use the largest scale toward chromaScale that stays in range,
            // found by bisection so neighbouring colors get neighbouring results
            const float floor = std::min({l.x, l.y, l.z, 0.0f}) - 1.0e-4f;
            auto at = [&](float k) { return labToLin({lab.x, lab.y * k, lab.z * k}); };
            auto ok = [&](V3 o) { return std::min({o.x, o.y, o.z}) >= floor; };
            V3 o = at(kc);
            if (!ok(o)) {
                float lo = 1.0f, hi = kc; // scale 1 = input, always in range
                for (int i = 0; i < 14; ++i) {
                    float mid = 0.5f * (lo + hi);
                    (ok(at(mid)) ? lo : hi) = mid;
                }
                o = at(lo);
            }
            l = o;
        }
    }
    return linToDi3(l);
}

// ---------------------------------------------------------------- Color Keeper

// Which pixels belong to the kept color: hue window around `hue` (degrees, raised cosine,
// `width` = half width) and colorful enough to have a hue at all.
inline float keeperWeight(V3 lab, float hue, float width)
{
    float C = std::sqrt(lab.y * lab.y + lab.z * lab.z);
    float gate = smoothstep(0.02f, 0.05f, C);
    if (gate <= 0.0f) return 0.0f;
    float h = std::atan2(lab.z, lab.y) / kDeg;
    float d = std::fabs(std::fmod(h - hue + 540.0f, 360.0f) - 180.0f);
    if (d >= width) return 0.0f;
    float c = std::cos(1.5707963f * d / width);
    return gate * c * c;
}

struct Region {
    V3 lab = {0, 0, 0}; // mean Oklab of the kept color in the frame
    float share = 0;
};

inline Region regionMean(const FrameSamples& f, float hue, float width)
{
    Region r;
    double sL = 0, sa = 0, sb = 0, sw = 0;
    for (V3 lab : f.lab) {
        if (lab.x < 0.1f) continue;
        double w = keeperWeight(lab, hue, width);
        sL += w * lab.x;
        sa += w * lab.y;
        sb += w * lab.z;
        sw += w;
    }
    if (sw <= 0 || f.lab.empty()) return r;
    r.lab = {float(sL / sw), float(sa / sw), float(sb / sw)};
    r.share = float(sw / double(f.lab.size()));
    return r;
}

struct KeeperSettings {
    float hue = 60, width = 30;
    V3 delta = {0, 0, 0}; // Oklab shift for a fully-weighted pixel as bright as the region
    float refL = 0.5f;     // the region's lightness: a/b shifts scale with a pixel's own L
    bool show = false;
};

// target: where the color should be (Oklab); matchL: 0..1 how much brightness follows too
inline KeeperSettings makeKeeper(const Region& cur, V3 target, float hue, float width, float strength, float matchL)
{
    KeeperSettings k;
    k.hue = hue;
    k.width = std::clamp(width, 5.0f, 90.0f);
    if (cur.share <= 0.0005f) return k; // the color is not in this frame
    k.refL = std::max(cur.lab.x, 0.05f);
    float s = std::clamp(strength, 0.0f, 1.0f);
    float da = target.y - cur.lab.y, db = target.z - cur.lab.z;
    float dc = std::sqrt(da * da + db * db), lim = 0.08f; // never more than a big nudge
    if (dc > lim) da *= lim / dc, db *= lim / dc;
    float dL = std::clamp(target.x - cur.lab.x, -0.15f, 0.15f) * std::clamp(matchL, 0.0f, 1.0f);
    k.delta = {dL * s, da * s, db * s};
    return k;
}

inline V3 keeperPixel(V3 di, const KeeperSettings& k)
{
    V3 lab = linToLab(diToLin3(di));
    if (!(lab.x == lab.x)) return di;
    float w = keeperWeight(lab, k.hue, k.width);
    if (k.show) { // affected pixels in their color, the rest grey
        float g = linToDi(0.18f * std::exp2((lab.x - 0.6f) * 6.0f) * 0.5f);
        V3 grey = {g, g, g};
        return {grey.x + (di.x - grey.x) * w, grey.y + (di.y - grey.y) * w, grey.z + (di.z - grey.z) * w};
    }
    if (w <= 0.0f) return di;
    // move the pixel with the region; the a/b shift scales with its lightness (a darker part
    // of the same jacket has proportionally smaller a/b), so shadows are not overcooked
    float rel = std::clamp(lab.x / k.refL, 0.0f, 1.5f);
    lab = {lab.x + k.delta.x * w, lab.y + k.delta.y * w * rel, lab.z + k.delta.z * w * rel};
    return linToDi3(labToLin(lab));
}

// ---------------------------------------------------------------- Palette Card

struct Swatch {
    V3 lab = {0, 0, 0};
    float share = 0;
};

// Dominant colors of the frame: k-means in Oklab over the samples (chroma counts double, so
// a small strong color is not swallowed by a big grey). Deterministic: seeded with the
// densest sample, then the samples furthest from what is chosen.
inline std::vector<Swatch> paletteFromSamples(const FrameSamples& f, int k)
{
    std::vector<Swatch> out;
    if (f.lab.empty()) return out;
    // at most ~20k points for speed, evenly strided (deterministic)
    std::vector<V3> pts;
    size_t stride = std::max<size_t>(1, f.lab.size() / 20000);
    for (size_t i = 0; i < f.lab.size(); i += stride) pts.push_back({f.lab[i].x, 2.0f * f.lab[i].y, 2.0f * f.lab[i].z});
    k = std::clamp(k, 1, std::min(8, int(pts.size())));
    auto d2 = [](V3 a, V3 b) {
        float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return x * x + y * y + z * z;
    };
    // seed: the point with the most neighbours within 0.05 (on a coarse subset)
    std::vector<V3> c;
    {
        size_t best = 0;
        int bestN = -1;
        size_t sub = std::max<size_t>(1, pts.size() / 500);
        for (size_t i = 0; i < pts.size(); i += sub) {
            int n = 0;
            for (size_t j = 0; j < pts.size(); j += sub) n += d2(pts[i], pts[j]) < 0.0025f;
            if (n > bestN) bestN = n, best = i;
        }
        c.push_back(pts[best]);
    }
    std::vector<float> nearest(pts.size(), 1e9f);
    while (int(c.size()) < k) { // farthest point
        size_t best = 0;
        float bestD = -1;
        for (size_t i = 0; i < pts.size(); ++i) {
            nearest[i] = std::min(nearest[i], d2(pts[i], c.back()));
            if (nearest[i] > bestD) bestD = nearest[i], best = i;
        }
        c.push_back(pts[best]);
    }
    std::vector<int> assign(pts.size(), 0);
    for (int it = 0; it < 25; ++it) {
        bool moved = false;
        for (size_t i = 0; i < pts.size(); ++i) {
            int bi = 0;
            float bd = 1e9f;
            for (int j = 0; j < k; ++j) {
                float d = d2(pts[i], c[size_t(j)]);
                if (d < bd) bd = d, bi = j;
            }
            moved = moved || assign[i] != bi;
            assign[i] = bi;
        }
        std::vector<double> sx(size_t(k), 0), sy(size_t(k), 0), sz(size_t(k), 0), sn(size_t(k), 0);
        for (size_t i = 0; i < pts.size(); ++i) {
            size_t j = size_t(assign[i]);
            sx[j] += pts[i].x, sy[j] += pts[i].y, sz[j] += pts[i].z, sn[j] += 1;
        }
        for (size_t j = 0; j < size_t(k); ++j)
            if (sn[j] > 0) c[j] = {float(sx[j] / sn[j]), float(sy[j] / sn[j]), float(sz[j] / sn[j])};
        if (!moved && it > 0) break;
    }
    std::vector<double> sn(size_t(k), 0);
    for (int a : assign) sn[size_t(a)] += 1;
    for (size_t j = 0; j < size_t(k); ++j)
        if (sn[j] > 0) out.push_back({{c[j].x, 0.5f * c[j].y, 0.5f * c[j].z}, float(sn[j] / double(pts.size()))});
    return out;
}

enum PaletteOrder { kOrderShare = 0, kOrderLight = 1, kOrderHue = 2 };

inline void sortPalette(std::vector<Swatch>& p, int order)
{
    auto hue = [](const Swatch& s) {
        float C = std::sqrt(s.lab.y * s.lab.y + s.lab.z * s.lab.z);
        if (C < 0.02f) return 1000.0f + s.lab.x; // greys last, dark to light
        float h = std::atan2(s.lab.z, s.lab.y) / kDeg;
        return h < 0 ? h + 360.0f : h;
    };
    std::stable_sort(p.begin(), p.end(), [&](const Swatch& a, const Swatch& b) {
        if (order == kOrderLight) return a.lab.x < b.lab.x;
        if (order == kOrderHue) return hue(a) < hue(b);
        return a.share > b.share;
    });
}

} // namespace drs
