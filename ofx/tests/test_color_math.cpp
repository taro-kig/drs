// Checks for ColorMath.h. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>

#include "ColorMath.h"

using namespace drs;

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

static bool near3(V3 a, V3 b, float eps)
{
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

// a display sRGB color -> the DI code value of the same color in DWG
static V3 srgbToDi(V3 c)
{
    V3 xyz = mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)});
    V3 d = mul(kXyzToDwg, xyz);
    return {linToDi(d.x), linToDi(d.y), linToDi(d.z)};
}

static float hueDeg(V3 di)
{
    V3 lab = xyzToOklab(mul(kDwgToXyz, {diToLin(di.x), diToLin(di.y), diToLin(di.z)}));
    return std::atan2(lab.z, lab.y) * 57.29578f;
}

static void print(const char* name, V3 v) { std::printf("       %-22s %.4f %.4f %.4f\n", name, v.x, v.y, v.z); }

int main()
{
    // round trips
    V3 p = {0.3f, 0.45f, 0.6f};
    V3 lab = xyzToOklab(mul(kDwgToXyz, p));
    check(near3(mul(kXyzToDwg, oklabToXyz(lab)), p, 1e-4f), "DWG -> Oklab -> DWG round trip");
    check(std::fabs(linToDi(diToLin(0.42f)) - 0.42f) < 1e-5f, "DI round trip");
    V3 white = xyzToOklab(mul(kSrgbToXyz, {1, 1, 1}));
    check(std::fabs(white.x - 1.0f) < 1e-3f && std::fabs(white.y) < 1e-3f && std::fabs(white.z) < 1e-3f,
          "sRGB white -> Oklab (1, 0, 0)");

    V3 skin = srgbToDi({0.80f, 0.58f, 0.47f});
    V3 sky = srgbToDi({0.45f, 0.63f, 0.86f});
    V3 grey = srgbToDi({0.46f, 0.46f, 0.46f});

    // nothing enabled -> untouched
    Settings off;
    check(near3(processPixel(skin, off), skin, 1e-6f), "no anchors -> no change");

    // enabled but From == To -> untouched
    Settings same;
    same.anchors[0] = makeAnchor(true, {0.80f, 0.58f, 0.47f}, {0.80f, 0.58f, 0.47f}, 0.06f, 1, 0);
    check(near3(processPixel(skin, same), skin, 1e-4f), "From == To -> no change");

    // skin -> a more golden skin
    Settings s;
    s.anchors[0] = makeAnchor(true, {0.80f, 0.58f, 0.47f}, {0.82f, 0.62f, 0.42f}, 0.06f, 1, 0);
    V3 skinOut = processPixel(skin, s);
    V3 skinTarget = srgbToDi({0.82f, 0.62f, 0.42f});
    print("skin in", skin);
    print("skin out", skinOut);
    print("skin target", skinTarget);
    check(std::fabs(hueDeg(skinOut) - hueDeg(skinTarget)) < 1.0f, "picked color lands on the target hue");
    check(near3(processPixel(sky, s), sky, 1e-3f), "far color (sky) untouched");
    check(near3(processPixel(grey, s), grey, 1e-4f), "neutral grey untouched");

    // the same skin 2 stops darker still moves (exposure independent) ...
    V3 lin = {diToLin(skin.x) * 0.25f, diToLin(skin.y) * 0.25f, diToLin(skin.z) * 0.25f};
    V3 darkSkin = {linToDi(lin.x), linToDi(lin.y), linToDi(lin.z)};
    check(std::fabs(hueDeg(processPixel(darkSkin, s)) - hueDeg(darkSkin)) > 3.0f, "darker skin still follows");
    // ... unless Match Lightness asks for similar brightness only
    Settings sl = s;
    sl.anchors[0].matchL = 4.0f;
    check(std::fabs(hueDeg(processPixel(darkSkin, sl)) - hueDeg(darkSkin)) < 1.0f,
          "Match Lightness 4: 2 stops darker skin left alone");

    // mix and mask
    Settings sm = s;
    sm.mix = 0.0f;
    check(near3(processPixel(skin, sm), skin, 1e-6f), "Mix 0 -> no change");
    Settings mk = s;
    mk.showMask = true;
    V3 m1 = processPixel(skin, mk), m0 = processPixel(sky, mk);
    check(m1.x > 0.95f && m0.x < 0.01f, "mask: white on the anchor, black far away");

    // no NaN / Inf on awkward inputs
    bool finite = true;
    const V3 odd[] = {{0, 0, 0}, {-0.1f, 0.2f, 0.3f}, {2.0f, 2.0f, 2.0f}, {0.9f, -0.2f, 0.1f}};
    for (V3 v : odd) {
        V3 o = processPixel(v, s);
        finite = finite && std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.z);
    }
    check(finite, "no NaN / Inf on black, negative and very bright input");

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
