// Checks for Lut3D.h: baked LUTs match the exact pixel math closely and are much faster.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "ColorMath.h"
#include "LookMath.h"
#include "Lut3D.h"

using namespace drs;

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

template <class F>
static void compare(const char* name, int n, const F& f, float maxTol, float meanTol)
{
    Lut3D lut;
    auto t0 = std::chrono::steady_clock::now();
    lut.bake(n, f);
    double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    // realistic pixels: any sRGB-gamut color at -4..+4 stops (in DWG / DI) ...
    std::vector<V3> px(400000);
    for (V3& p : px) {
        V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(u(rng)), srgbToLin(u(rng)), srgbToLin(u(rng))}));
        float k = std::exp2(u(rng) * 8.0f - 4.0f) * 0.5f;
        p = {linToDi(d.x * k), linToDi(d.y * k), linToDi(d.z * k)};
    }
    // ... plus near-neutrals, where errors would show most
    for (size_t i = 0; i < px.size() / 2; ++i) {
        float l = u(rng) * 0.8f + 0.1f;
        px[i] = {l + (u(rng) - 0.5f) * 0.06f, l + (u(rng) - 0.5f) * 0.04f, l + (u(rng) - 0.5f) * 0.06f};
    }

    float maxErr = 0, sumErr = 0;
    size_t worst = 0;
    std::vector<V3> a(px.size()), b(px.size());
    auto t1 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < px.size(); ++i) a[i] = f(px[i]);
    auto t2 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < px.size(); ++i) b[i] = lut.apply(px[i]);
    auto t3 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < px.size(); ++i) {
        float e = std::fmax(std::fabs(a[i].x - b[i].x), std::fmax(std::fabs(a[i].y - b[i].y), std::fabs(a[i].z - b[i].z)));
        if (e > maxErr) worst = i;
        maxErr = std::fmax(maxErr, e);
        sumErr += e;
    }
    double directMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double lutMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
    float mean = sumErr / px.size();
    if (maxErr > maxTol)
        std::printf("       worst at in (%.4f %.4f %.4f): exact (%.4f %.4f %.4f) LUT (%.4f %.4f %.4f)\n", px[worst].x,
                    px[worst].y, px[worst].z, a[worst].x, a[worst].y, a[worst].z, b[worst].x, b[worst].y, b[worst].z);
    std::printf("       %-26s n=%d bake %.0f ms | 400k px: exact %.1f ms, LUT %.1f ms (x%.0f) | err max %.5f mean %.6f\n",
                name, n, bakeMs, directMs, lutMs, directMs / lutMs, maxErr, mean);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "%s: LUT within %.3f (max) / %.4f (mean) DI of exact", name, maxTol, meanTol);
    check(maxErr < maxTol && mean < meanTol, msg);
    std::snprintf(msg, sizeof(msg), "%s: LUT at least 5x faster than exact", name);
    check(directMs > lutMs * 5.0, msg);
}

int main()
{
    // Color Anchors: skin -> golden, plus a narrow sky anchor
    Settings s;
    s.anchors[0] = makeAnchor(true, {0.80f, 0.58f, 0.47f}, {0.82f, 0.62f, 0.42f}, 0.06f, 1, 0);
    s.anchors[1] = makeAnchor(true, {0.45f, 0.63f, 0.86f}, {0.35f, 0.62f, 0.80f}, 0.03f, 1, 1);
    compare("Color Anchors", 65, [&s](V3 c) { return processPixel(c, s); }, 0.02f, 0.0005f);

    // Look Explorer: a starting look and an extreme variation
    for (int cell : {1, 3}) {
        LookParams k = prepare(finish(variation({}, 0, 1, cell, 0.6f), 1, 0, 0), 1);
        char name[48];
        std::snprintf(name, sizeof(name), "Look %s", kArchetypeNames[cell]);
        compare(name, 65, [k](V3 c) { return applyLook(c, k); }, 0.02f, 0.0005f);
    }

    Lut3D lut;
    lut.bake(5, [](V3 c) { return c; });
    check(!lut.inDomain({1.2f, 0.5f, 0.5f}) && !lut.inDomain({-0.2f, 0.5f, 0.5f}) && lut.inDomain({0.5f, 0.5f, 0.5f}),
          "out-of-range pixels are reported (exact math used for them)");
    V3 id = lut.apply({0.123f, 0.456f, 0.789f});
    check(std::fabs(id.x - 0.123f) + std::fabs(id.y - 0.456f) + std::fabs(id.z - 0.789f) < 1e-5f,
          "identity LUT is exact (tetrahedral interpolation of a linear function)");

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
