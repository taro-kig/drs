// Checks for HueVecMath.h. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <random>

#include "HueVecMath.h"

using namespace drs;
namespace hv = drs::huevec;

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

static V3 toLin(V3 di) { return {diToLin(di.x), diToLin(di.y), diToLin(di.z)}; }
static V3 toDi(V3 l) { return {linToDi(l.x), linToDi(l.y), linToDi(l.z)}; }
static float luma(V3 di) { return hv::lumaDwg(toLin(di)); }

// HSV saturation in linear DWG
static float sat(V3 di)
{
    V3 l = toLin(di);
    float mx = std::max(l.x, std::max(l.y, l.z)), mn = std::min(l.x, std::min(l.y, l.z));
    return mx > 0 ? (mx - mn) / mx : 0;
}

static void print(const char* name, V3 v) { std::printf("       %-22s %.4f %.4f %.4f\n", name, v.x, v.y, v.z); }

int main()
{
    const V3 red = toDi({0.30f, 0.06f, 0.05f});
    const V3 yellow = toDi({0.25f, 0.22f, 0.04f});
    const V3 blue = toDi({0.04f, 0.07f, 0.30f});
    const V3 grey = toDi({0.18f, 0.18f, 0.18f});
    const V3 hiRed = toDi({6.0f, 1.2f, 1.0f}); // bright saturated highlight

    // defaults do nothing
    hv::Settings def;
    check(hv::isIdentity(def), "defaults -> identity");
    check(near3(hv::processPixel(red, def), red, 1e-6f), "defaults -> no change");

    // Richness split
    float d, s;
    hv::richnessSplit(1.0f, 0.5f, d, s);
    check(d == 1.0f && s == 1.0f, "Balance 0.5 -> full density and full saturation");
    hv::richnessSplit(1.0f, 0.0f, d, s);
    check(d == 0.0f && s == 1.0f, "Balance 0 -> saturation only");
    hv::richnessSplit(1.0f, 1.0f, d, s);
    check(d == 1.0f && s == 0.0f, "Balance 1 -> density only");

    // Richness +
    hv::Settings rich;
    rich.richness = 1.0f;
    V3 r = hv::processPixel(red, rich);
    print("red", red);
    print("red, Richness 1", r);
    check(!hv::isIdentity(rich), "Richness on -> not identity");
    check(near3(hv::processPixel(grey, rich), grey, 1e-5f), "Richness leaves neutral grey alone");
    check(luma(r) < luma(red) * 0.8f, "Richness darkens a saturated red (density)");
    check(sat(r) > sat(red), "Richness makes the red more saturated");

    // Balance 0: saturation only, luminance kept
    hv::Settings satOnly = rich;
    satOnly.balance = 0.0f;
    const V3 teal = toDi({0.06f, 0.20f, 0.16f});
    V3 so = hv::processPixel(teal, satOnly);
    check(std::fabs(luma(so) - luma(teal)) < 1e-4f, "Balance 0 keeps luminance");
    check(sat(so) > sat(teal), "Balance 0 raises saturation");

    // Richness -
    hv::Settings soft;
    soft.richness = -1.0f;
    V3 sf = hv::processPixel(red, soft);
    check(luma(sf) > luma(red) && sat(sf) < sat(red), "Richness - = lighter and less saturated");

    // stays in gamut: in-gamut input never gets a negative linear channel
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    bool inGamut = true;
    hv::Settings max = rich;
    for (int i = 0; i < 6; ++i) max.sat.v[i] = 1.0f;
    for (int i = 0; i < 20000 && inGamut; ++i) {
        V3 lin = {u(rng), u(rng), u(rng)};
        V3 o = toLin(hv::processPixel(toDi(lin), max));
        if (o.x < -1e-5f || o.y < -1e-5f || o.z < -1e-5f) inGamut = false;
    }
    check(inGamut, "max saturation keeps in-gamut colors in gamut");

    // per-hue isolation: Red Den does not touch blue
    hv::Settings redDen;
    redDen.density.v[0] = 1.0f;
    check(near3(hv::processPixel(blue, redDen), blue, 1e-5f), "Red Den leaves blue alone");
    check(luma(hv::processPixel(red, redDen)) < luma(red), "Red Den darkens red");

    // hue shift: Red + moves red toward yellow (green channel up relative to red)
    hv::Settings shift;
    shift.hue.v[0] = 1.0f;
    V3 sh = toLin(hv::processPixel(red, shift));
    V3 rl = toLin(red);
    check(sh.y / sh.x > rl.y / rl.x, "Red Hue + moves red toward yellow");
    check(std::fabs(hv::lumaDwg(sh) - hv::lumaDwg(rl)) < 1e-3f, "Hue shift keeps luminance");
    check(near3(hv::processPixel(blue, shift), blue, 1e-5f), "Red Hue leaves blue alone");

    // narrower Sat Hue Range isolates more
    hv::Settings ySat;
    ySat.sat.v[1] = 1.0f;
    hv::Settings ySatNarrow = ySat;
    ySatNarrow.sat.range = 0.4f;
    V3 orange = toDi({0.30f, 0.15f, 0.04f});
    float wide = sat(hv::processPixel(orange, ySat)) - sat(orange);
    float narrow = sat(hv::processPixel(orange, ySatNarrow)) - sat(orange);
    check(narrow < wide, "lower Sat Hue Range reaches less of the neighbors");
    check(sat(hv::processPixel(yellow, ySatNarrow)) > sat(yellow), "narrow range still hits its own hue");

    // Deep keeps bright highlights clean
    hv::Settings deep = rich;
    deep.deep = 0.5f; // cutoff around +2 stops
    float hiNoDeep = std::fabs(luma(hv::processPixel(hiRed, rich)) - luma(hiRed));
    float hiDeep = std::fabs(luma(hv::processPixel(hiRed, deep)) - luma(hiRed));
    check(hiDeep < 0.05f * hiNoDeep, "Deep 0.5 leaves a bright highlight (almost) alone");
    check(luma(hv::processPixel(red, deep)) < luma(red) * 0.8f, "Deep 0.5 still works in the mid tones");

    hv::Settings view = deep;
    view.showDeep = true;
    V3 vh = hv::processPixel(hiRed, view);
    V3 vm = hv::processPixel(grey, view);
    check(vh.x > vh.y && vh.z > vh.y, "Show Deep paints excluded highlights magenta");
    check(std::fabs(vm.x - vm.y) < 1e-4f && std::fabs(vm.y - vm.z) < 1e-4f, "Show Deep keeps mid tones grey");

    // Mix
    hv::Settings half = rich;
    half.mix = 0.5f;
    V3 h = hv::processPixel(red, half);
    check(near3(h, {(red.x + r.x) * 0.5f, (red.y + r.y) * 0.5f, (red.z + r.z) * 0.5f}, 1e-5f), "Mix 0.5 = halfway");
    hv::Settings none = rich;
    none.mix = 0.0f;
    check(hv::isIdentity(none), "Mix 0 -> identity");

    // the chain is the three DCTL stages in a row
    hv::Settings all;
    all.richness = 0.6f;
    all.balance = 0.7f;
    all.deep = 0.3f;
    all.density.v[2] = -0.4f;
    all.sat.v[4] = 0.5f;
    all.sat.range = 0.8f;
    all.hue.v[1] = -0.3f;
    hv::Vectors den = all.density, sv = all.sat;
    hv::richnessSplit(all.richness, all.balance, d, s);
    den.glob = d;
    sv.glob = s;
    bool chainOk = true;
    for (int i = 0; i < 2000 && chainOk; ++i) {
        V3 c = {u(rng), u(rng), u(rng)};
        V3 manual = hv::stage(hv::stage(hv::stage(c, hv::kDensity, den, all.deep), hv::kSat, sv, all.deep),
                              hv::kHue, all.hue, all.deep);
        chainOk = near3(hv::processPixel(c, all), manual, 1e-6f);
    }
    check(chainOk, "processPixel = Density -> Sat -> Hue stages");

    // LUT accuracy is checked with the other plugins in test_lut

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
