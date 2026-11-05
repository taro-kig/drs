// Checks for ColorLookMath.h. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <vector>

#include "ColorLookMath.h"
#include "Lut3D.h"

using namespace drs;

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

static V3 srgbToDi(V3 c, float stops = 0)
{
    V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)}));
    float k = std::exp2(stops);
    return {linToDi(d.x * k), linToDi(d.y * k), linToDi(d.z * k)};
}
static V3 greyDi(float stops)
{
    float v = linToDi(0.18f * std::exp2(stops));
    return {v, v, v};
}
static V3 lab(V3 di) { return xyzToOklab(mul(kDwgToXyz, {diToLin(di.x), diToLin(di.y), diToLin(di.z)})); }
static float hueDeg(V3 di)
{
    V3 l = lab(di);
    float h = std::atan2(l.z, l.y) * 57.29578f;
    return h < 0 ? h + 360 : h;
}
static float dist(V3 a, V3 b) { return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z); }

struct Slots {
    bool on[kMixColors] = {};
    V3 pick[kMixColors];
    Slots()
    {
        for (int i = 0; i < kMixColors; ++i) pick[i] = bandReferenceSrgb(i);
    }
};

static ColorLookSettings make(const Slots& s, float amount = 1, float autoS = 0, ToneStats st = {})
{
    return makeColorLook(s.on, s.pick, amount, 0, 0, autoS, st);
}

int main()
{
    const V3 leaf = srgbToDi({0.40f, 0.55f, 0.25f}), skin = srgbToDi({0.80f, 0.58f, 0.47f});
    const V3 amber = {0.85f, 0.60f, 0.32f}, deepTeal = {0.10f, 0.28f, 0.30f}, paleCream = {0.95f, 0.90f, 0.80f};

    // nothing on, Amount 0, Auto Tone off -> untouched
    Slots none;
    float worst = 0;
    for (V3 c : {leaf, skin, greyDi(-3), greyDi(2)}) worst = std::fmax(worst, dist(colorLookPixel(c, make(none, 0)), c));
    check(worst < 1e-4f, "no colors, Amount 0, Auto Tone 0 -> no change");

    // each slot's starting swatch means "no move" for its band
    bool still = true;
    for (int i = 0; i < kMixColors; ++i) {
        MixerSettings m;
        bandMoveFromPick(i, bandReferenceSrgb(i), m);
        V3 r = bandReferenceSrgb(i);
        std::printf("       %-8s swatch %.2f %.2f %.2f  move h %+.2f s %+.2f l %+.2f\n", kMixNames[i], r.x, r.y, r.z,
                    m.hue[i], m.sat[i], m.lum[i]);
        still = still && std::fabs(m.hue[i]) < 0.1f && std::fabs(m.sat[i]) < 0.15f && std::fabs(m.lum[i]) < 0.1f;
    }
    check(still, "a slot left at its starting swatch barely moves its band");

    // Green slot -> deep teal: foliage turns toward teal and darker
    Slots g;
    g.on[3] = true;
    g.pick[3] = deepTeal;
    V3 leafOut = colorLookPixel(leaf, make(g));
    std::printf("       foliage hue %.0f -> %.0f, L %.3f -> %.3f\n", hueDeg(leaf), hueDeg(leafOut), lab(leaf).x,
                lab(leafOut).x);
    check(hueDeg(leafOut) > hueDeg(leaf) + 15 && lab(leafOut).x < lab(leaf).x, "Green slot = deep teal: leaves go teal and deeper");

    // palette amber (light) + deep teal (dark): warm highlights, teal shadows
    Slots p;
    p.on[1] = true;
    p.pick[1] = amber;
    p.on[4] = true;
    p.pick[4] = deepTeal;
    ColorLookSettings ps = make(p);
    V3 hi = lab(colorLookPixel(greyDi(2), ps)), lo = lab(colorLookPixel(greyDi(-3), ps));
    float hiH = std::atan2(hi.z, hi.y) * 57.29578f, loH = std::atan2(lo.z, lo.y) * 57.29578f;
    std::printf("       grey +2 -> hue %.0f C %.3f | grey -3 -> hue %.0f C %.3f\n", hiH, std::hypot(hi.y, hi.z), loH,
                std::hypot(lo.y, lo.z));
    check(hiH > 30 && hiH < 110 && loH < -100, "amber + deep teal: warm highlights, teal shadows");

    // one color only: shadows go to its opposite
    Slots one;
    one.on[1] = true;
    one.pick[1] = amber;
    V3 lo1 = lab(colorLookPixel(greyDi(-3), make(one)));
    float lo1H = std::atan2(lo1.z, lo1.y) * 57.29578f;
    check(lo1H < -90 || lo1H > 170, "one amber color: shadows lean blue/teal");

    // mood: a dark palette gives more contrast than a light one
    auto span = [](const ColorLookSettings& s) {
        return lab(colorLookPixel(greyDi(2), s)).x - lab(colorLookPixel(greyDi(-2), s)).x;
    };
    Slots dark, light;
    dark.on[4] = dark.on[1] = true;
    dark.pick[4] = deepTeal;
    dark.pick[1] = {0.45f, 0.25f, 0.12f};
    light.on[1] = true;
    light.pick[1] = paleCream;
    float sd = span(make(dark)), sl = span(make(light));
    std::printf("       L span -2..+2 stops: dark palette %.3f, light palette %.3f\n", sd, sl);
    check(sd > sl, "dark palette = punchier than a light, airy one");
    V3 blackLight = colorLookPixel(greyDi(-8), make(light)), blackDark = colorLookPixel(greyDi(-8), make(dark));
    check(diToLin(blackLight.y) > diToLin(blackDark.y), "light palette lifts the blacks more");

    // Auto Tone: a dark, flat frame gets brighter and more contrasty
    ToneStats flat;
    flat.median = -3.0f;
    flat.p05 = -5.0f;
    flat.p95 = -1.0f;
    ColorLookSettings at = make(one, 1, 1, flat);
    float midIn = -3.0f, midOut = lumaStops(colorLookPixel(greyDi(midIn), at));
    // the Auto Tone stage alone (the look on top may soften or harden it on purpose)
    float r0 = lumaStops(applyTone(greyDi(-1), at.tone)) - lumaStops(applyTone(greyDi(-5), at.tone));
    std::printf("       Auto Tone on a dark flat frame: median %.1f -> %.1f stops, 5-95%% range 4.0 -> %.1f\n", midIn,
                midOut, r0);
    check(midOut > -1.5f && r0 > 5.5f, "Auto Tone lifts a dark frame and opens up its range");

    // histogram -> stats
    std::vector<uint32_t> hist(kHistBins, 0);
    for (float s = -4; s <= 2; s += 0.01f) ++hist[histBin(s)];
    ToneStats st = statsFromHistogram(hist);
    std::printf("       stats of a -4..+2 ramp: p05 %.2f median %.2f p95 %.2f\n", st.p05, st.median, st.p95);
    check(std::fabs(st.median + 1) < 0.15f && std::fabs(st.p05 + 3.7f) < 0.15f && std::fabs(st.p95 - 1.7f) < 0.15f,
          "histogram percentiles");

    // LUT matches exact math for a full setup
    ColorLookSettings full = make(p, 1, 1, flat);
    Lut3D lut;
    lut.bake(65, [&full](V3 c) { return colorLookPixel(c, full); });
    float e = 0;
    for (float st2 = -5; st2 <= 3; st2 += 0.5f)
        for (V3 c : {srgbToDi({0.80f, 0.58f, 0.47f}, st2), srgbToDi({0.40f, 0.55f, 0.25f}, st2),
                     srgbToDi({0.45f, 0.63f, 0.86f}, st2), greyDi(st2)}) {
            if (!lut.inDomain(c)) continue;
            V3 a = colorLookPixel(c, full), l = lut.apply(c);
            e = std::fmax(e, std::fmax(std::fabs(a.x - l.x), std::fmax(std::fabs(a.y - l.y), std::fabs(a.z - l.z))));
        }
    std::printf("       LUT vs exact: %.5f DI\n", e);
    check(e < 0.015f, "65^3 LUT matches the exact math");

    bool finite = true;
    Slots all;
    for (int i = 0; i < kMixColors; ++i) {
        all.on[i] = true;
        all.pick[i] = {float(i % 2), 0.2f * i / 6.0f, float((i + 1) % 2)};
    }
    for (V3 v : {V3{0, 0, 0}, V3{-0.1f, 0.2f, 0.3f}, V3{1.5f, 1.5f, 1.5f}, V3{0.9f, -0.2f, 0.1f}}) {
        V3 r = colorLookPixel(v, make(all, 1.5f, 1, flat));
        finite = finite && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z);
    }
    check(finite, "no NaN / Inf with every slot on and extreme colors");

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
