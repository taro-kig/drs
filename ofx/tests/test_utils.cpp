// Checks for the utility plugins' math: Auto Neutral, Shot Match, Color Keeper, Check View,
// Palette Card. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

#include "CheckMath.h"
#include "ColorMath.h"
#include "FrameColor.h"
#include "Lut3D.h"
#include "PaletteLayout.h"

using namespace drs;

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

// a little synthetic "frame": a list of DI pixels
using Frame = std::vector<V3>;

static V3 labDi(float L, float a, float b) { return linToDi3(labToLin({L, a, b})); }
static V3 greyDi(float stops)
{
    float v = linToDi(0.18f * std::exp2(stops));
    return {v, v, v};
}
static V3 srgbDi(V3 c)
{
    return linToDi3(mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)})));
}
static V3 lab(V3 di) { return linToLab(diToLin3(di)); }
static float hueDeg(V3 l)
{
    float h = std::atan2(l.z, l.y) / kDeg;
    return h < 0 ? h + 360 : h;
}
static float hueGap(float a, float b) { return std::fabs(std::fmod(a - b + 540.0f, 360.0f) - 180.0f); }

static FrameSamples S(const Frame& f) { return measureSamples(f); }

static void add(Frame& f, V3 c, int n) { f.insert(f.end(), size_t(n), c); }

static Frame gains(const Frame& f, V3 g, float stops = 0)
{
    Frame o;
    float k = std::exp2(stops);
    for (V3 v : f) {
        V3 l = diToLin3(v);
        o.push_back(linToDi3({l.x * g.x * k, l.y * g.y * k, l.z * g.z * k}));
    }
    return o;
}

// a room: grey walls at several brightnesses, skin, wood, leaves, sky
static Frame room()
{
    Frame f;
    for (float st = -4; st <= 2.5f; st += 0.25f) add(f, greyDi(st), 40);
    add(f, srgbDi({0.80f, 0.58f, 0.47f}), 200);
    add(f, srgbDi({0.45f, 0.30f, 0.18f}), 300);
    add(f, srgbDi({0.40f, 0.55f, 0.25f}), 250);
    add(f, srgbDi({0.45f, 0.63f, 0.86f}), 250);
    return f;
}

int main()
{
    const Frame base = room();
    const V3 tungsten = {1.25f, 1.0f, 0.72f};

    // ---------------- Auto Neutral
    Cast c0 = castFromSamples(S(base));
    std::printf("       room cast (%+.4f, %+.4f), support %.2f\n", c0.a, c0.b, c0.support);
    check(std::fabs(c0.a) < 0.004f && std::fabs(c0.b) < 0.004f, "neutral room: no cast measured");
    Frame warm = gains(base, tungsten);
    Cast cw = castFromSamples(S(warm));
    std::printf("       tungsten cast (%+.4f, %+.4f), support %.2f\n", cw.a, cw.b, cw.support);
    check(cw.b > 0.02f && cw.support > 0.3f, "tungsten light reads as a warm cast");
    NeutralSettings ns = makeNeutral(cw, 1, 0, 0);
    V3 wallW = gains({greyDi(0)}, tungsten)[0];
    V3 fixedWall = neutralPixel(wallW, ns);
    V3 fl = lab(fixedWall);
    std::printf("       grey wall under tungsten C %.4f -> %.4f\n", std::hypot(lab(wallW).y, lab(wallW).z),
                std::hypot(fl.y, fl.z));
    check(std::hypot(fl.y, fl.z) < 0.008f, "Auto Neutral makes the grey wall grey again");
    check(std::fabs(lumaDwgLin(diToLin3(fixedWall)) - lumaDwgLin(diToLin3(wallW))) < 0.01f * lumaDwgLin(diToLin3(wallW)) + 1e-4f,
          "... keeping its luminance");
    NeutralSettings half = makeNeutral(cw, 0.5f, 0, 0);
    V3 hl = lab(neutralPixel(wallW, half));
    check(std::hypot(hl.y, hl.z) > std::hypot(fl.y, fl.z) + 0.01f, "Strength 0.5 leaves part of the cast");
    NeutralSettings warmer = makeNeutral(cw, 1, 1, 0);
    check(lab(neutralPixel(wallW, warmer)).z > fl.z + 0.01f, "Warmth + makes the result warmer");
    NeutralSettings tinted = makeNeutral(cw, 1, 0, 1);
    check(lab(neutralPixel(wallW, tinted)).y > fl.y + 0.005f, "Tint + makes it more magenta");
    // a frame with nothing neutral: left alone
    Frame leaves;
    add(leaves, srgbDi({0.30f, 0.55f, 0.20f}), 500);
    add(leaves, srgbDi({0.20f, 0.45f, 0.15f}), 500);
    NeutralSettings nn = makeNeutral(castFromSamples(S(leaves)), 1, 0, 0);
    check(std::fabs(nn.gains.x - 1) < 1e-3f && std::fabs(nn.gains.z - 1) < 1e-3f, "nothing neutral in frame -> no change");

    // ---------------- Shot Match
    const FrameSamples baseS = S(base);
    ShotStats ref = shotStats(baseS);
    MatchSettings same = makeMatch(ref, ref, 1, 1, 1, &baseS);
    bool ident = true;
    for (V3 v : base) {
        V3 o = matchPixel(v, same);
        ident = ident && std::fabs(o.x - v.x) < 2e-3f && std::fabs(o.y - v.y) < 2e-3f && std::fabs(o.z - v.z) < 2e-3f;
    }
    check(ident, "Shot Match: a shot matched to itself does not change");
    Frame under = gains(base, {0.85f, 1.0f, 1.2f}, -1.2f);
    const FrameSamples underS = S(under);
    ShotStats cur = shotStats(underS);
    MatchSettings m = makeMatch(cur, ref, 1, 1, 1, &underS);
    Frame matched;
    for (V3 v : under) matched.push_back(matchPixel(v, m));
    ShotStats after = shotStats(S(matched));
    std::printf("       median ref %+.2f, shot %+.2f -> %+.2f | cast b ref %+.4f, shot %+.4f -> %+.4f\n", ref.tone.median,
                cur.tone.median, after.tone.median, ref.cast.b, cur.cast.b, after.cast.b);
    check(std::fabs(after.tone.median - ref.tone.median) < 0.15f && std::fabs(after.tone.p95 - ref.tone.p95) < 0.15f,
          "Shot Match: exposure and highlights land on the reference");
    check(std::fabs(after.cast.b - ref.cast.b) < 0.4f * std::fabs(cur.cast.b - ref.cast.b), "Shot Match: light color moves to the reference");
    MatchSettings expOnly = makeMatch(cur, ref, 1, 0, 0, nullptr);
    check(std::fabs(expOnly.gains.x - 1) < 1e-4f && expOnly.chromaScale == 1.0f, "Light Color 0 / Saturation 0 -> exposure only");
    ShotStats wild = cur;
    wild.tone.p95 = wild.tone.median + 0.2f; // nearly flat shot vs a contrasty reference
    MatchSettings mw = makeMatch(wild, ref, 1, 0, 0);
    check(mw.slopeHi <= 2.0f && mw.slopeLo >= 0.5f, "Shot Match: contrast change stays within 0.5x..2x");

    // ---------------- Color Keeper
    const V3 jacket = srgbDi({0.85f, 0.35f, 0.25f}), leaf = srgbDi({0.35f, 0.55f, 0.25f});
    Frame scene;
    for (float st = -3; st <= 2; st += 0.5f) add(scene, greyDi(st), 50);
    add(scene, jacket, 300);
    add(scene, leaf, 300);
    const float jHue = hueDeg(lab(jacket));
    Region r0 = regionMean(S(scene), jHue, 30);
    std::printf("       jacket hue %.0f, region share %.2f, region hue %.0f\n", jHue, r0.share, hueDeg(r0.lab));
    check(r0.share > 0.2f && hueGap(hueDeg(r0.lab), jHue) < 8, "Color Keeper finds the jacket");
    // another shot: warmer light shifts the jacket; keep it to the reference
    Frame shot2 = gains(scene, {1.15f, 1.0f, 0.8f});
    Region r2 = regionMean(S(shot2), jHue, 30);
    KeeperSettings ks = makeKeeper(r2, r0.lab, jHue, 30, 1, 1);
    V3 j2 = gains({jacket}, {1.15f, 1.0f, 0.8f})[0];
    V3 kept = keeperPixel(j2, ks);
    float before = hueGap(hueDeg(lab(j2)), jHue), afterK = hueGap(hueDeg(lab(kept)), jHue);
    std::printf("       jacket hue off by %.1f deg -> %.1f deg after Color Keeper\n", before, afterK);
    check(afterK < before * 0.5f, "Color Keeper pulls the jacket back toward the reference");
    V3 l2 = gains({leaf}, {1.15f, 1.0f, 0.8f})[0];
    V3 lk = keeperPixel(l2, ks);
    check(std::fabs(lk.x - l2.x) < 1e-4f && std::fabs(lk.y - l2.y) < 1e-4f && std::fabs(lk.z - l2.z) < 1e-4f,
          "Color Keeper leaves the leaves alone");
    V3 g2 = gains({greyDi(0)}, {1.15f, 1.0f, 0.8f})[0];
    V3 gk = keeperPixel(g2, ks);
    check(std::fabs(gk.x - g2.x) < 1e-4f, "Color Keeper leaves greys alone");
    KeeperSettings none = makeKeeper(Region{}, r0.lab, jHue, 30, 1, 1);
    check(none.delta.x == 0 && none.delta.y == 0, "color not in the frame -> nothing moves");
    KeeperSettings show = ks;
    show.show = true;
    V3 sj = keeperPixel(jacket, show), sl = keeperPixel(leaf, show);
    check(std::fabs(sj.x - jacket.x) < 0.02f && std::fabs(sl.x - sl.y) < 1e-4f, "Show Area: jacket in color, leaves grey");

    // ---------------- Check View
    CheckSettings fc = makeCheck(kCheckFalseColor);
    auto isZone = [&](V3 o, int z) {
        return std::fabs(o.x - fc.zone[z * 3]) < 1e-6f && std::fabs(o.y - fc.zone[z * 3 + 1]) < 1e-6f &&
               std::fabs(o.z - fc.zone[z * 3 + 2]) < 1e-6f;
    };
    check(isZone(checkPixel(greyDi(0), fc), 2), "False Color: middle grey is green");
    check(isZone(checkPixel(greyDi(-7), fc), 0), "False Color: -7 stops is crushed (purple)");
    check(isZone(checkPixel(greyDi(-5), fc), 1), "False Color: -5 stops is deep shadow (blue)");
    check(isZone(checkPixel(greyDi(1.4f), fc), 3), "False Color: +1.4 stops is bright skin (pink)");
    check(isZone(checkPixel(greyDi(5.2f), fc), 5), "False Color: +5.2 stops is near clip (orange)");
    check(isZone(checkPixel(greyDi(7), fc), 6), "False Color: +7 stops is clipped (red)");
    V3 between = checkPixel(greyDi(-2), fc);
    check(std::fabs(between.x - between.y) < 1e-6f && std::fabs(between.y - between.z) < 1e-6f,
          "False Color: in-between exposures stay grey");
    CheckSettings sk = makeCheck(kCheckSkin);
    V3 skin = srgbDi({0.80f, 0.58f, 0.47f}), dskin = srgbDi({0.45f, 0.28f, 0.20f});
    V3 so = checkPixel(skin, sk), dso = checkPixel(dskin, sk), lo = checkPixel(leaf, sk), bo = checkPixel(srgbDi({0.3f, 0.4f, 0.8f}), sk);
    check(std::fabs(so.x - skin.x) < 0.01f && std::fabs(so.z - skin.z) < 0.01f, "Skin Tones: light skin stays in color");
    check(std::fabs(dso.x - dskin.x) < 0.02f, "Skin Tones: deep skin stays in color");
    check(std::fabs(lo.x - lo.y) < 1e-5f && std::fabs(bo.x - bo.z) < 1e-5f, "Skin Tones: leaves and blue go grey");
    CheckSettings g709 = makeCheck(kCheckGamut709), gp3 = makeCheck(kCheckGamutP3);
    auto alert = [&](V3 o, const CheckSettings& s) { return std::fabs(o.x - s.zone[21]) < 1e-6f && std::fabs(o.z - s.zone[23]) < 1e-6f; };
    V3 srgbRed = srgbDi({1.0f, 0.0f, 0.0f}), dwgGreen = linToDi3({0.0f, 0.5f, 0.0f});
    check(!alert(checkPixel(srgbDi({0.9f, 0.2f, 0.1f}), g709), g709), "Gamut 709: an sRGB color is fine");
    check(alert(checkPixel(dwgGreen, g709), g709), "Gamut 709: a DWG-primary green is flagged");
    // a color inside P3 but outside Rec.709: built from P3 primaries (a strong P3 green)
    static const float kP3ToXyz[9] = {0.4865709f, 0.2656677f, 0.1982173f, 0.2289746f, 0.6917385f,
                                      0.0792869f, 0.0f,       0.0451134f, 1.0439444f};
    V3 p3col = linToDi3(mul(kXyzToDwg, mul(kP3ToXyz, {0.1f, 0.8f, 0.1f})));
    check(alert(checkPixel(p3col, g709), g709) && !alert(checkPixel(p3col, gp3), gp3), "a P3-only color: flagged for 709, fine for P3");
    check(!alert(checkPixel(srgbRed, gp3), gp3), "Gamut P3: sRGB red is fine");

    // ---------------- Palette Card
    Frame pf;
    add(pf, srgbDi({0.85f, 0.35f, 0.25f}), 4000);
    add(pf, srgbDi({0.20f, 0.35f, 0.70f}), 2500);
    add(pf, srgbDi({0.90f, 0.85f, 0.70f}), 1500);
    add(pf, srgbDi({0.10f, 0.10f, 0.10f}), 2000);
    const FrameSamples pfS = S(pf);
    std::vector<Swatch> pal = paletteFromSamples(pfS, 4);
    check(pal.size() == 4, "Palette: 4 colors asked, 4 given");
    sortPalette(pal, kOrderShare);
    float share = 0;
    for (const Swatch& s : pal) share += s.share;
    check(std::fabs(share - 1) < 1e-3f, "Palette: shares add up to 1");
    check(std::fabs(pal[0].share - 0.4f) < 0.05f, "Palette: biggest color first, about 40%");
    const V3 truth[4] = {lab(srgbDi({0.85f, 0.35f, 0.25f})), lab(srgbDi({0.20f, 0.35f, 0.70f})),
                         lab(srgbDi({0.90f, 0.85f, 0.70f})), lab(srgbDi({0.10f, 0.10f, 0.10f}))};
    float worst = 0;
    for (const V3& tv : truth) {
        float best = 1e9f;
        for (const Swatch& s : pal)
            best = std::min(best, std::sqrt((s.lab.x - tv.x) * (s.lab.x - tv.x) + (s.lab.y - tv.y) * (s.lab.y - tv.y) +
                                            (s.lab.z - tv.z) * (s.lab.z - tv.z)));
        worst = std::max(worst, best);
    }
    std::printf("       palette: worst distance to the true colors %.3f (Oklab)\n", worst);
    check(worst < 0.01f, "Palette: each true color has a swatch close to it");
    sortPalette(pal, kOrderLight);
    check(pal.front().lab.x < pal.back().lab.x, "Palette: Lightness order goes dark to light");
    std::vector<Swatch> pa = paletteFromSamples(pfS, 5), pb = paletteFromSamples(S(pf), 5);
    bool stable = pa.size() == pb.size();
    for (size_t i = 0; stable && i < pa.size(); ++i) stable = pa[i].lab.x == pb[i].lab.x && pa[i].share == pb[i].share;
    check(stable, "Palette: same frame -> same palette (deterministic)");

    PaletteDraw d = layoutPalette(pal, 1920, 1080, kPosBottom, 0.14f, true);
    bool inside = d.rects.size() == 4 * 5;
    for (size_t r = 0; inside && r < d.rects.size() / 4; ++r)
        inside = d.rects[r * 4] >= 0 && d.rects[r * 4 + 2] <= 1920 && d.rects[r * 4 + 1] >= 0 && d.rects[r * 4 + 3] <= 1080;
    check(inside, "Layout: background + 4 swatches, all inside the frame");
    check(d.rects[3] <= 152 && d.rects[3] >= 150, "Layout: bottom strip is 14% of the height");
    bool noOverlap = true;
    for (size_t r = 1; r + 1 < d.rects.size() / 4; ++r) noOverlap = noOverlap && d.rects[r * 4 + 2] <= d.rects[(r + 1) * 4];
    check(noOverlap, "Layout: swatches don't overlap");
    PaletteDraw dr = layoutPalette(pal, 1920, 1080, kPosRight, 0.2f, false);
    float h1 = dr.rects[7] - dr.rects[5], h2 = dr.rects[11] - dr.rects[9];
    check(std::fabs(h1 - h2) <= 1 && dr.rects[4] >= 1920 * 0.8f - 1, "Layout: right strip, equal swatches");

    // ---------------- LUTs (the three LUT-based tools) stay close to the direct math
    // realistic pixels like test_lut: sRGB-gamut colors from -4 to +4 stops around grey
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    auto lutErr = [&](const char* name, const std::function<V3(V3)>& fn) {
        Lut3D lut;
        lut.bake(65, fn);
        double sum = 0;
        float mx = 0;
        for (int i = 0; i < 20000; ++i) {
            V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(u(rng)), srgbToLin(u(rng)), srgbToLin(u(rng))}));
            float k = std::exp2(u(rng) * 8.0f - 4.0f) * 0.5f;
            V3 c = linToDi3({d.x * k, d.y * k, d.z * k});
            if (!lut.inDomain(c)) continue;
            V3 a = lut.apply(c), b = fn(c);
            float e = std::max(std::fabs(a.x - b.x), std::max(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
            sum += e, mx = std::max(mx, e);
        }
        std::printf("       %-13s LUT err max %.4f mean %.5f\n", name, mx, sum / 20000);
        return mx < 0.02f && sum / 20000 < 0.0005;
    };
    check(lutErr("Auto Neutral", [&](V3 c) { return neutralPixel(c, ns); }), "Auto Neutral LUT within 0.02 / 0.0005");
    check(lutErr("Shot Match", [&](V3 c) { return matchPixel(c, m); }), "Shot Match LUT within 0.02 / 0.0005");
    check(lutErr("Color Keeper", [&](V3 c) { return keeperPixel(c, ks); }), "Color Keeper LUT within 0.02 / 0.0005");

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
