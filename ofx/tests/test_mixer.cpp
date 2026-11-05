// Checks for MixerMath.h. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>

#include "Lut3D.h"
#include "MixerMath.h"

using namespace drs;

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

static V3 srgbToDi(V3 c)
{
    V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)}));
    return {linToDi(d.x), linToDi(d.y), linToDi(d.z)};
}

static V3 lab(V3 di) { return xyzToOklab(mul(kDwgToXyz, {diToLin(di.x), diToLin(di.y), diToLin(di.z)})); }
static float hueDeg(V3 di)
{
    V3 l = lab(di);
    float h = std::atan2(l.z, l.y) * 57.29578f;
    return h < 0 ? h + 360 : h;
}
static float chroma(V3 di)
{
    V3 l = lab(di);
    return std::hypot(l.y, l.z);
}
static float dist(V3 a, V3 b) { return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z); }

int main()
{
    const V3 skin = srgbToDi({0.80f, 0.58f, 0.47f});
    const V3 sky = srgbToDi({0.45f, 0.63f, 0.86f});
    const V3 leaf = srgbToDi({0.40f, 0.55f, 0.25f});
    const V3 grey = srgbToDi({0.46f, 0.46f, 0.46f});
    const V3 red = srgbToDi({0.80f, 0.15f, 0.15f});
    std::printf("       Oklab hue: skin %.0f  sky %.0f  foliage %.0f  red %.0f\n", hueDeg(skin), hueDeg(sky),
                hueDeg(leaf), hueDeg(red));

    // weights always sum to 1, and peak at the band centers
    bool sum1 = true;
    for (float h = -30; h < 400; h += 0.7f) {
        float w[kMixColors];
        mixWeights(h, w);
        float s = 0;
        for (float v : w) s += v;
        sum1 = sum1 && std::fabs(s - 1) < 1e-5f;
    }
    check(sum1, "band weights sum to 1 all around the hue circle");
    bool peaks = true;
    for (int i = 0; i < kMixColors; ++i) {
        float w[kMixColors];
        mixWeights(kMixCenters[i], w);
        peaks = peaks && w[i] > 0.9999f;
    }
    check(peaks, "each band is at full weight on its center");

    MixerSettings none;
    float worst = 0;
    for (V3 c : {skin, sky, leaf, grey, red}) worst = std::fmax(worst, dist(mixPixel(c, none), c));
    check(worst < 1e-4f && mixerIsIdentity(none), "all sliders at 0 -> no change");

    // Orange saturation: skin changes, sky does not, grey never
    MixerSettings o;
    o.sat[1] = -0.6f;
    check(chroma(mixPixel(skin, o)) < chroma(skin) * 0.6f, "Orange Sat -0.6 desaturates skin");
    check(dist(mixPixel(sky, o), sky) < 1e-4f, "... and leaves the sky alone");
    check(dist(mixPixel(grey, o), grey) < 1e-4f, "... and greys");

    // Green hue moves foliage, Blue luminance darkens sky
    MixerSettings g;
    g.hue[3] = -1.0f;
    float dh = hueDeg(mixPixel(leaf, g)) - hueDeg(leaf);
    std::printf("       foliage hue shift with Green Hue -1: %.1f deg\n", dh);
    check(dh < -10.0f, "Green Hue - turns foliage toward yellow");
    MixerSettings b;
    b.lum[5] = -1.0f;
    float l0 = lab(sky).x, l1 = lab(mixPixel(sky, b)).x;
    std::printf("       sky Oklab L %.3f -> %.3f with Blue Lum -1\n", l0, l1);
    check(l1 < l0 * 0.9f, "Blue Lum - darkens the sky");
    check(std::fabs(hueDeg(mixPixel(sky, b)) - hueDeg(sky)) < 1.0f, "... without changing its hue");

    // mask
    MixerSettings m;
    m.solo = 1;
    check(mixPixel(skin, m).x > 0.5f && mixPixel(sky, m).x < 0.01f && mixPixel(grey, m).x < 0.01f,
          "Show Mask Orange: skin white, sky and grey black");

    // LUT stays close to the exact math with strong settings
    MixerSettings strong;
    for (int i = 0; i < kMixColors; ++i) {
        strong.hue[i] = (i % 2) ? 0.8f : -0.8f;
        strong.sat[i] = 0.5f;
        strong.lum[i] = (i % 3) ? -0.5f : 0.5f;
    }
    // a typical edit: warmer, softer skin, deeper sky, yellower foliage
    MixerSettings typical;
    typical.hue[1] = 0.2f;
    typical.sat[1] = -0.2f;
    typical.lum[1] = 0.2f;
    typical.sat[5] = 0.3f;
    typical.lum[5] = -0.4f;
    typical.hue[3] = -0.5f;
    typical.sat[3] = -0.3f;
    const V3 samples[] = {skin, sky, leaf, grey, red, srgbToDi({0.2f, 0.3f, 0.6f}), srgbToDi({0.9f, 0.8f, 0.2f}),
                          srgbToDi({0.5f, 0.48f, 0.45f}), srgbToDi({0.1f, 0.12f, 0.1f})};
    auto lutErr = [&](const MixerSettings& st, int n) {
        Lut3D lut;
        lut.bake(n, [&st](V3 c) { return mixPixel(c, st); });
        float e = 0;
        for (V3 c : samples) {
            V3 a = mixPixel(c, st), l = lut.apply(c);
            e = std::fmax(e, std::fmax(std::fabs(a.x - l.x), std::fmax(std::fabs(a.y - l.y), std::fabs(a.z - l.z))));
        }
        return e;
    };
    float eTyp = lutErr(typical, 65), eStrong = lutErr(strong, 65);
    std::printf("       LUT vs exact: typical edit %.5f DI, every slider pushed hard %.5f DI\n", eTyp, eStrong);
    check(eTyp < 0.01f, "65^3 LUT matches the exact math for a typical edit");
    check(eStrong < 0.04f, "... and stays close with every slider pushed hard");

    bool finite = true;
    for (V3 v : {V3{0, 0, 0}, V3{-0.1f, 0.2f, 0.3f}, V3{1.5f, 1.5f, 1.5f}, V3{0.9f, -0.2f, 0.1f}}) {
        V3 r = mixPixel(v, strong);
        finite = finite && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z);
    }
    check(finite, "no NaN / Inf on odd input");

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
