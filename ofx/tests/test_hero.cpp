// Checks for the hero colors in Color Look (HeroMath.h). Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "ColorLookMath.h"

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
static float chroma(V3 di)
{
    V3 l = lab(di);
    return std::sqrt(l.y * l.y + l.z * l.z);
}
static float hueGap(float a, float b) { return std::fabs(std::fmod(a - b + 540.0f, 360.0f) - 180.0f); }

struct Px {
    V3 c;
    int n;
};
static ColorStats statsOf(const std::vector<Px>& scene)
{
    std::vector<uint32_t> h(kColorBins, 0u);
    for (const Px& p : scene) {
        int b = colorBin(p.c);
        if (b >= 0) h[size_t(b)] += uint32_t(p.n);
    }
    return statsFromColorHist(h);
}

static ColorLookSettings heroLook(int band, float s, const ColorStats& cs, float amount = 1)
{
    float hero[kMixColors] = {};
    if (band >= 0) hero[band] = s;
    bool on[kMixColors] = {};
    V3 pick[kMixColors];
    for (int i = 0; i < kMixColors; ++i) pick[i] = bandReferenceSrgb(i);
    return makeColorLook(hero, cs, on, pick, amount, 0, 0, 0, ToneStats{});
}

static ColorLookSettings heroLookMulti(const float hero[kMixColors], const ColorStats& cs)
{
    bool on[kMixColors] = {};
    V3 pick[kMixColors];
    for (int i = 0; i < kMixColors; ++i) pick[i] = bandReferenceSrgb(i);
    return makeColorLook(hero, cs, on, pick, 1, 0, 0, 0, ToneStats{});
}

enum { kRed, kOrange, kYellow, kGreen, kAqua, kBlue, kMagenta };

int main()
{
    const V3 skin = srgbToDi({0.80f, 0.58f, 0.47f}), wood = srgbToDi({0.45f, 0.30f, 0.18f});
    const V3 leaf = srgbToDi({0.40f, 0.55f, 0.25f}), sky = srgbToDi({0.45f, 0.63f, 0.86f});
    const V3 shadow = greyDi(-3), mid = greyDi(0), high = greyDi(2);
    const std::vector<Px> room = {{shadow, 10}, {mid, 15}, {high, 10}, {skin, 8}, {wood, 10}, {leaf, 10}, {sky, 10}};
    const ColorStats cs = statsOf(room);

    // --- frame statistics ---
    check(cs.valid, "room stats valid");
    check(cs.coverage[kOrange] > cs.coverage[kYellow] && cs.coverage[kOrange] > cs.coverage[kMagenta],
          "skin and wood count as Orange");
    check(cs.coverage[kBlue] > 0.05f && cs.coverage[kGreen] > 0.05f, "sky counts as Blue, leaves as Green");
    check(std::fabs(cs.neutral - 35.0f / 73.0f) < 0.05f, "greys count as neutral");
    std::vector<uint32_t> empty(kColorBins, 0u);
    check(!statsFromColorHist(empty).valid, "empty histogram -> not valid");

    // --- no hero: same as Color Look 1.0 (manual slots only) ---
    {
        bool on[kMixColors] = {};
        V3 pick[kMixColors];
        for (int i = 0; i < kMixColors; ++i) pick[i] = bandReferenceSrgb(i);
        on[kGreen] = true;
        pick[kGreen] = {0.35f, 0.50f, 0.30f};
        on[kOrange] = true;
        const float none[kMixColors] = {};
        ColorLookSettings a = makeColorLook(on, pick, 0.8f, 0.1f, 0, 0, ToneStats{});
        ColorLookSettings b = makeColorLook(none, cs, on, pick, 0.8f, 0.1f, 0, 0, ToneStats{});
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> u(0, 1);
        bool same = true;
        for (int i = 0; i < 5000 && same; ++i) {
            V3 c = {u(rng), u(rng), u(rng)};
            V3 x = colorLookPixel(c, a), y = colorLookPixel(c, b);
            same = x.x == y.x && x.y == y.y && x.z == y.z;
        }
        check(same, "all heroes at 0 -> bit-identical to the manual-only Color Look");
    }

    // --- Orange: teal and orange, without repainting skin / wood / leaves ---
    ColorLookSettings orange = heroLook(kOrange, 1, cs);
    V3 oHi = colorLookPixel(high, orange), oSh = colorLookPixel(shadow, orange), oMid = colorLookPixel(mid, orange);
    std::printf("       Orange: high hue %.0f C %.3f | mid hue %.0f C %.3f | shadow hue %.0f C %.3f\n", hueDeg(oHi),
                chroma(oHi), hueDeg(oMid), chroma(oMid), hueDeg(oSh), chroma(oSh));
    check(hueGap(hueDeg(oHi), 60) < 20 && chroma(oHi) > 0.02f, "Orange: grey highlights turn orange");
    check(hueGap(hueDeg(oSh), 210) < 25 && chroma(oSh) > 0.01f, "Orange: grey shadows turn teal");
    check(hueGap(hueDeg(oMid), 60) < 25 && chroma(oMid) > 0.005f, "Orange: mid greys lean orange");
    float worstHue = 0;
    for (V3 c : {skin, wood, leaf, sky}) worstHue = std::max(worstHue, hueGap(hueDeg(colorLookPixel(c, orange)), hueDeg(c)));
    std::printf("       Orange: largest hue change on skin/wood/leaf/sky %.1f deg\n", worstHue);
    check(worstHue < 8, "Orange: skin, wood, leaves and sky keep their hue (< 8 deg)");
    check(chroma(colorLookPixel(skin, orange)) > chroma(skin), "Orange: skin (the hero) gets a little richer");

    // --- Aqua: a cyan look, cool in the shadows, a touch of amber up top ---
    ColorLookSettings aqua = heroLook(kAqua, 1, cs);
    V3 aSh = colorLookPixel(shadow, aqua), aMid = colorLookPixel(mid, aqua), aHi = colorLookPixel(high, aqua);
    std::printf("       Aqua: shadow hue %.0f C %.3f | mid hue %.0f C %.3f | high hue %.0f C %.3f\n", hueDeg(aSh),
                chroma(aSh), hueDeg(aMid), chroma(aMid), hueDeg(aHi), chroma(aHi));
    check(hueGap(hueDeg(aSh), 195) < 20 && chroma(aSh) > 0.02f, "Aqua: shadows go cyan");
    check(hueGap(hueDeg(aMid), 195) < 25 && chroma(aMid) > 0.01f, "Aqua: mid greys go cyan");
    check(hueGap(hueDeg(aHi), 50) < 30, "Aqua: highlights get the warm complement");
    worstHue = 0;
    for (V3 c : {skin, leaf}) worstHue = std::max(worstHue, hueGap(hueDeg(colorLookPixel(c, aqua)), hueDeg(c)));
    check(worstHue < 8, "Aqua: skin and leaves keep their hue (< 8 deg)");

    // --- Green: green-led, leaves richer, not repainted ---
    ColorLookSettings green = heroLook(kGreen, 1, cs);
    V3 gLeaf = colorLookPixel(leaf, green);
    check(chroma(gLeaf) > chroma(leaf) * 1.1f && hueGap(hueDeg(gLeaf), hueDeg(leaf)) < 8,
          "Green: leaves get richer, same hue");
    check(hueGap(hueDeg(colorLookPixel(mid, green)), 140) < 25, "Green: mid greys lean green");

    // --- strength and Amount scale it ---
    V3 half = colorLookPixel(high, heroLook(kOrange, 0.5f, cs));
    check(chroma(half) > 0.3f * chroma(oHi) && chroma(half) < 0.8f * chroma(oHi), "strength 0.5 = about half the tint");
    V3 amt0 = colorLookPixel(high, heroLook(kOrange, 1, cs, 0));
    check(chroma(amt0) < 0.002f, "Amount 0 -> no hero tint");

    // --- adapts to the frame ---
    // the less of the hero color the frame has, the more the picture leans to it
    // (same amount of grey in both, so only the hero coverage differs)
    std::vector<Px> noOrange = {{shadow, 10}, {mid, 15}, {high, 10}, {leaf, 20}, {sky, 20}};
    std::vector<Px> lotsOrange = {{shadow, 10}, {mid, 15}, {high, 10}, {skin, 20}, {wood, 20}};
    float castLow = chroma(colorLookPixel(mid, heroLook(kOrange, 1, statsOf(noOrange))));
    float castHigh = chroma(colorLookPixel(mid, heroLook(kOrange, 1, statsOf(lotsOrange))));
    std::printf("       mid grey tint: frame without orange %.4f, frame full of orange %.4f\n", castLow, castHigh);
    check(castLow > castHigh * 1.3f, "frame without the hero color leans to it more");
    // the more complement-colored the frame, the more of the complement
    std::vector<Px> noSky = {{shadow, 10}, {mid, 15}, {high, 10}, {skin, 8}, {wood, 10}};
    std::vector<Px> bigSky = {{shadow, 10}, {mid, 15}, {high, 10}, {skin, 8}, {wood, 10}, {sky, 40}};
    HeroPlan pNo = planHero(std::vector<float>{0, 1, 0, 0, 0, 0, 0}.data(), statsOf(noSky));
    HeroPlan pBig = planHero(std::vector<float>{0, 1, 0, 0, 0, 0, 0}.data(), statsOf(bigSky));
    check(pBig.compAmt > pNo.compAmt, "frame with sky gets more of the teal complement");
    // frames with little grey lean harder (the tints have fewer neutral pixels to land on)
    std::vector<Px> greyRoom = {{shadow, 30}, {mid, 40}, {high, 30}, {skin, 5}};
    std::vector<Px> busyRoom = {{shadow, 3}, {mid, 4}, {high, 3}, {skin, 30}, {wood, 30}, {leaf, 30}};
    HeroPlan pGrey = planHero(std::vector<float>{0, 0, 0, 0, 1, 0, 0}.data(), statsOf(greyRoom));
    HeroPlan pBusy = planHero(std::vector<float>{0, 0, 0, 0, 1, 0, 0}.data(), statsOf(busyRoom));
    check(pBusy.colorful > 1.5f && pGrey.colorful < 1.1f, "colorful frame -> stronger lean");
    // a cool hero must still pull a warm, colorful frame toward itself
    {
        ColorStats bs = statsOf(busyRoom);
        ColorLookSettings aq = heroLook(kAqua, 0.8f, bs);
        float hx = std::cos(195.0f * kDeg), hy = std::sin(195.0f * kDeg);
        float lean = 0;
        for (V3 c : {skin, wood, leaf}) {
            V3 a0 = lab(c), a1 = lab(colorLookPixel(c, aq));
            lean += (a1.y - a0.y) * hx + (a1.z - a0.z) * hy;
        }
        std::printf("       Aqua 0.8 on a warm, colorful frame: lean toward cyan %.4f\n", lean / 3);
        check(lean > 0.005f * 3, "cool hero still leans a warm colorful frame toward it");
        float wh = hueGap(hueDeg(colorLookPixel(wood, aq)), hueDeg(wood));
        check(wh < 15, "... while wood keeps its hue (< 15 deg)");
    }

    // unmeasured frame still works
    check(chroma(colorLookPixel(high, heroLook(kOrange, 1, ColorStats{}))) > 0.02f, "works without frame stats");

    // --- hue layout ---
    check(hueGap(complementHue(60), 215) < 1, "complement of orange = teal (215)");
    check(hueGap(complementHue(195), 45) < 1, "complement of cyan = amber (45)");
    HeroPlan po = planHero(std::vector<float>{0, 1, 0, 0, 0, 0, 0}.data(), cs);
    HeroPlan pb = planHero(std::vector<float>{0, 0, 0, 0, 0, 1, 0}.data(), cs);
    check(po.heroHigh > 0.9f && pb.heroHigh < 0.1f, "warm hero -> highlights, cool hero -> shadows");
    HeroPlan two = planHero(std::vector<float>{1, 1, 0, 0, 0, 0, 0}.data(), cs);
    check(!two.paired && two.hue > 25 && two.hue < 60, "Red + Orange (close) -> one hero between them");
    HeroPlan bm = planHero(std::vector<float>{0, 0, 0, 0, 0, 1, 1}.data(), cs);
    check(!bm.paired && bm.hue > 260 && bm.hue < 325, "Blue + Magenta (close) -> one hero between them");

    // far-apart heroes form a pair instead of averaging into a third color
    HeroPlan oa = planHero(std::vector<float>{0, 1, 0, 0, 1, 0, 0}.data(), cs);
    check(oa.paired && hueGap(oa.hue, 60) < 1 && hueGap(oa.complement, 195) < 1,
          "Orange + Aqua -> a pair (60 / 195), not their average (green)");
    {
        const float oaHero[kMixColors] = {0, 0.8f, 0, 0, 0.8f, 0, 0};
        ColorLookSettings to = heroLookMulti(oaHero, cs);
        V3 h = colorLookPixel(high, to), sh = colorLookPixel(shadow, to);
        std::printf("       Orange + Aqua: high hue %.0f C %.3f | shadow hue %.0f C %.3f\n", hueDeg(h), chroma(h),
                    hueDeg(sh), chroma(sh));
        check(hueGap(hueDeg(h), 60) < 20 && chroma(h) > 0.02f, "Orange + Aqua: orange highlights");
        check(hueGap(hueDeg(sh), 195) < 20 && chroma(sh) > 0.015f, "Orange + Aqua: cyan shadows");
    }
    HeroPlan og = planHero(std::vector<float>{0, 0.8f, 0, 0.4f, 0, 0, 0}.data(), cs);
    check(og.paired && og.heroHigh > 0.9f && og.compAmt > 0.4f && og.compAmt < 0.6f,
          "Orange 0.8 + Green 0.4 -> orange highlights, green shadows at half strength");
    HeroPlan ba = planHero(std::vector<float>{0, 0.5f, 0, 0, 0, 1, 0}.data(), cs);
    check(ba.paired && hueGap(ba.hue, 260) < 1 && ba.heroHigh < 0.1f,
          "Blue 1 + Orange 0.5 -> blue leads (shadows), orange in the highlights");

    // --- greys stay smooth: no jumps along a grey ramp ---
    float worstStep = 0;
    V3 prev = colorLookPixel(greyDi(-6), orange);
    for (float st = -6; st <= 4; st += 0.05f) {
        V3 o = colorLookPixel(greyDi(st), orange);
        worstStep = std::max(worstStep, std::fabs(o.x - prev.x) + std::fabs(o.y - prev.y) + std::fabs(o.z - prev.z));
        prev = o;
    }
    check(worstStep < 0.03f, "grey ramp stays smooth (no steps)");

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
