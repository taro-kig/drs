// Checks for LookMath.h. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>

#include "LookMath.h"

using namespace drs;

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

static float dist(V3 a, V3 b) { return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z); }

static V3 srgbToDi(V3 c)
{
    V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)}));
    return {linToDi(d.x), linToDi(d.y), linToDi(d.z)};
}

static V3 lookOf(V3 di, const Look& l, float amount = 1, float warmth = 0, float punch = 0)
{
    return applyLook(di, prepare(finish(l, amount, warmth, punch), amount));
}

int main()
{
    const V3 samples[] = {srgbToDi({0.80f, 0.58f, 0.47f}), srgbToDi({0.45f, 0.63f, 0.86f}),
                          srgbToDi({0.40f, 0.55f, 0.25f}), srgbToDi({0.46f, 0.46f, 0.46f}),
                          srgbToDi({0.95f, 0.95f, 0.95f}), srgbToDi({0.05f, 0.05f, 0.06f})};
    const V3 grey = {linToDi(0.18f), linToDi(0.18f), linToDi(0.18f)};

    // Amount 0 = untouched
    float worst = 0;
    for (int c = 0; c < 9; ++c)
        for (V3 s : samples) worst = std::fmax(worst, dist(lookOf(s, variation({}, 0, 1, c, 0.6f), 0.0f), s));
    std::printf("       amount 0 worst diff %.6f\n", worst);
    check(worst < 1e-4f, "Amount 0 -> no change for every starting look");

    // neutral look keeps 18% grey and stays mild
    V3 g = lookOf(grey, Look{});
    check(dist(g, grey) < 1e-3f, "neutral look keeps 18% grey");

    // the 9 starting looks are visibly different from each other
    float minPair = 1e9f;
    for (int a = 0; a < 9; ++a)
        for (int b = a + 1; b < 9; ++b) {
            float d = 0;
            for (V3 s : samples) d += dist(lookOf(s, variation({}, 0, 1, a, 0.6f)), lookOf(s, variation({}, 0, 1, b, 0.6f)));
            minPair = std::fmin(minPair, d);
        }
    std::printf("       closest pair of starting looks: %.4f\n", minPair);
    check(minPair > 0.05f, "starting looks are all distinct");

    // later rounds: cell 5 is the center, others differ, and spread shrinks per round
    Look center = variation({}, 0, 1, 0, 0.6f); // Warm Print
    Look v5 = variation(center, 3, 999, 4, 0.6f);
    bool same = true;
    for (int d = 0; d < kDims; ++d) same = same && v5.p[d] == center.p[d];
    check(same, "cell 5 of a round = the current look");
    auto meanMove = [&](int round) {
        float m = 0;
        for (int c = 0; c < 9; ++c) {
            Look v = variation(center, round, 4242, c, 0.6f);
            for (int d = 0; d < kDims; ++d) m += std::fabs(v.p[d] - center.p[d]);
        }
        return m / 8.0f;
    };
    std::printf("       mean move round 1: %.3f, round 4: %.3f\n", meanMove(1), meanMove(4));
    check(meanMove(1) > 0.5f && meanMove(4) < meanMove(1) * 0.5f, "variations narrow down round by round");
    check(std::fabs(variation(center, 1, 1, 0, 0.6f).p[0] - variation(center, 1, 2, 0, 0.6f).p[0]) +
                  std::fabs(variation(center, 1, 1, 0, 0.6f).p[2] - variation(center, 1, 2, 0, 0.6f).p[2]) +
                  std::fabs(variation(center, 1, 1, 0, 0.6f).p[6] - variation(center, 1, 2, 0, 0.6f).p[6]) >
              0.0f,
          "different seed -> different variations");

    // finishing controls
    V3 warm = lookOf(grey, Look{}, 1, 0.8f, 0);
    check(warm.x > grey.x && warm.z < grey.z, "Warmth + makes grey warmer");
    V3 hiIn = srgbToDi({0.85f, 0.85f, 0.85f}), loIn = srgbToDi({0.12f, 0.12f, 0.12f});
    float c0 = lookOf(hiIn, Look{}).y - lookOf(loIn, Look{}).y;
    float c1 = lookOf(hiIn, Look{}, 1, 0, 0.8f).y - lookOf(loIn, Look{}, 1, 0, 0.8f).y;
    check(c1 > c0, "Punch + adds contrast");

    // archetype sanity
    V3 skin = samples[0];
    V3 bleach = lookOf(skin, variation({}, 0, 1, 5, 0.6f));
    V3 skinLab = xyzToOklab(mul(kDwgToXyz, {diToLin(skin.x), diToLin(skin.y), diToLin(skin.z)}));
    V3 blLab = xyzToOklab(mul(kDwgToXyz, {diToLin(bleach.x), diToLin(bleach.y), diToLin(bleach.z)}));
    check(std::hypot(blLab.y, blLab.z) < std::hypot(skinLab.y, skinLab.z), "Bleach desaturates skin");
    V3 fadedBlack = lookOf(srgbToDi({0, 0, 0}), variation({}, 0, 1, 2, 0.6f));
    check(diToLin(fadedBlack.y) > 0.005f, "Faded Matte lifts black");

    // no NaN / Inf, including after many wild rounds
    bool finite = true;
    const V3 odd[] = {{0, 0, 0}, {-0.1f, 0.2f, 0.3f}, {1.5f, 1.5f, 1.5f}, {0.9f, -0.2f, 0.1f}};
    Look wild = {};
    for (int r = 1; r < 30; ++r) wild = variation(wild, 1, uint32_t(r * 7919), r % 9, 1.5f);
    for (int c = 0; c < 9; ++c)
        for (V3 v : odd) {
            V3 o = lookOf(v, variation(wild, 1, 5, c, 1.5f), 1.5f, 1.0f, 1.0f);
            finite = finite && std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z);
        }
    check(finite, "no NaN / Inf on odd input, extreme looks and maxed controls");

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
