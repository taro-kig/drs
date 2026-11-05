// look_preview: writes a BMP contact sheet of Look Explorer's 9 starting looks applied
// to a synthetic chart (grey ramp, hue sweep, skin, sky, foliage). For eyeballing looks
// without Resolve. Usage: look_preview out.bmp [colorlook]
// colorlook: Color Look with 9 example palettes instead (Auto Tone off).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ColorLookMath.h"
#include "LookMath.h"

using namespace drs;

static const float kXyzToRec709[9] = {3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
                                      0.0415560f, 0.0556434f,  -0.2040259f, 1.0572252f};

static V3 srgbDisplayToDwgLin(V3 c, float stops)
{
    V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(c.x), srgbToLin(c.y), srgbToLin(c.z)}));
    float k = std::exp2(stops);
    return {d.x * k, d.y * k, d.z * k};
}

// scene: u, v in [0, 1), v = 0 at top
static V3 scene(float u, float v)
{
    if (v < 0.18f) {
        float s = 0.18f * std::exp2(-6.0f + 12.0f * u);
        return {s, s, s};
    }
    if (v < 0.42f) {
        float h = u * 6.2831853f;
        V3 lab = {0.62f, 0.13f * std::cos(h), 0.13f * std::sin(h)};
        return mul(kXyzToDwg, oklabToXyz(lab));
    }
    if (v < 0.66f) return srgbDisplayToDwgLin({0.80f, 0.58f, 0.47f}, -3.0f + 5.0f * u);
    if (v < 0.83f) return srgbDisplayToDwgLin({0.45f, 0.63f, 0.86f}, -1.5f + 2.5f * u);
    return srgbDisplayToDwgLin({0.40f, 0.55f, 0.25f}, -3.0f + 4.0f * u);
}

static uint8_t toDisplay(float x)
{
    x = std::max(x, 0.0f);
    float y = x * (1.0f + x / 16.0f) / (1.0f + x) * 1.05f; // gentle tone map, grey ~0.16
    y = std::min(y, 1.0f);
    float g = y <= 0.0031308f ? 12.92f * y : 1.055f * std::pow(y, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f));
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "looks.bmp";
    const int cw = 320, ch = 200, pad = 6;
    const int W = cw * 3 + pad * 4, H = ch * 3 + pad * 4;
    std::vector<uint8_t> img(size_t(W) * H * 3, 20);

    const bool colorLook = argc > 2 && std::string(argv[2]) == "colorlook";
    // Color Look example palettes: {slot, r, g, b} x up to 3
    struct Pick {
        int slot;
        V3 c;
    };
    const V3 amber = {0.85f, 0.60f, 0.32f}, teal = {0.10f, 0.28f, 0.30f}, cream = {0.95f, 0.90f, 0.80f};
    const std::vector<std::vector<Pick>> palettes = {
        {},                                                     // 1 nothing (base curve only)
        {{1, amber}},                                           // 2 amber only
        {{1, amber}, {4, teal}},                                // 3 amber light + teal shadow
        {{1, amber}, {4, teal}, {3, {0.12f, 0.32f, 0.26f}}},   // 4 + greens to deep teal-green
        {{1, cream}},                                           // 5 pale cream: airy
        {{1, {0.45f, 0.25f, 0.12f}}, {5, {0.08f, 0.15f, 0.25f}}}, // 6 dark brown + navy: moody
        {{5, {0.45f, 0.65f, 0.85f}}},                           // 7 sky blue only
        {{6, {0.85f, 0.55f, 0.70f}}, {4, {0.20f, 0.55f, 0.55f}}}, // 8 pink + aqua
        {{1, {0.80f, 0.45f, 0.25f}}, {3, {0.55f, 0.60f, 0.25f}}, {5, {0.30f, 0.45f, 0.60f}}}, // 9 warm earthy
    };

    for (int cell = 0; cell < 9; ++cell) {
        LookParams k = prepare(finish(variation({}, 0, 1, cell, 0.6f), 1, 0, 0), 1);
        ColorLookSettings cls;
        if (colorLook) {
            bool on[kMixColors] = {};
            V3 pick[kMixColors];
            for (int i = 0; i < kMixColors; ++i) pick[i] = bandReferenceSrgb(i);
            for (const Pick& p : palettes[cell]) {
                on[p.slot] = true;
                pick[p.slot] = p.c;
            }
            cls = makeColorLook(on, pick, 1, 0, 0, 0, ToneStats{});
        }
        int ox = pad + (cell % 3) * (cw + pad), oy = pad + (cell / 3) * (ch + pad);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                V3 lin = scene((x + 0.5f) / cw, (y + 0.5f) / ch);
                V3 in = {linToDi(lin.x), linToDi(lin.y), linToDi(lin.z)};
                V3 di = colorLook ? colorLookPixel(in, cls) : applyLook(in, k);
                V3 rgb = mul(kXyzToRec709, mul(kDwgToXyz, {diToLin(di.x), diToLin(di.y), diToLin(di.z)}));
                uint8_t* p = &img[(size_t(oy + y) * W + ox + x) * 3];
                p[0] = toDisplay(rgb.x);
                p[1] = toDisplay(rgb.y);
                p[2] = toDisplay(rgb.z);
            }
    }

    // 24-bit BMP, bottom-up rows, BGR
    int rowBytes = (W * 3 + 3) & ~3;
    uint32_t dataSize = uint32_t(rowBytes * H), fileSize = 54 + dataSize;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) hdr[o + i] = uint8_t(v >> (8 * i)); };
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(W));
    put32(22, uint32_t(H));
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, dataSize);
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t* p = &img[(size_t(y) * W + x) * 3];
            row[x * 3 + 0] = p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[0];
        }
        std::fwrite(row.data(), 1, rowBytes, f);
    }
    std::fclose(f);
    std::printf("wrote %s\n", path);
    return 0;
}
