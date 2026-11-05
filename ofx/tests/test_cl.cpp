// Runs the OpenCL LUT kernel on this machine's GPU (the way Resolve would) and compares
// it with the CPU LUT. Skips (passes) when there is no OpenCL GPU.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define CLC __stdcall
#else
#include <dlfcn.h>
#define CLC
#endif

#include "ClLut.h"
#include "ColorMath.h"
#include "LookMath.h"
#include "Lut3D.h"

using namespace drs;

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef void* H; // any cl_* handle

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main()
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("OpenCL.dll");
    auto sym = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(lib, n)); };
#else
    void* lib = dlopen("libOpenCL.so.1", RTLD_NOW);
    auto sym = [&](const char* n) { return dlsym(lib, n); };
#endif
    if (!lib) {
        std::printf("no OpenCL runtime, skipped\n");
        return 0;
    }
    auto GetPlatformIDs = (cl_int(CLC*)(cl_uint, H*, cl_uint*))sym("clGetPlatformIDs");
    auto GetDeviceIDs = (cl_int(CLC*)(H, uint64_t, cl_uint, H*, cl_uint*))sym("clGetDeviceIDs");
    auto GetDeviceInfo = (cl_int(CLC*)(H, cl_uint, size_t, void*, size_t*))sym("clGetDeviceInfo");
    auto CreateContext = (H(CLC*)(const intptr_t*, cl_uint, const H*, void*, void*, cl_int*))sym("clCreateContext");
    auto CreateQueue = (H(CLC*)(H, H, uint64_t, cl_int*))sym("clCreateCommandQueue");
    auto CreateBuffer = (H(CLC*)(H, uint64_t, size_t, void*, cl_int*))sym("clCreateBuffer");
    auto ReadBuffer = (cl_int(CLC*)(H, H, cl_uint, size_t, size_t, void*, cl_uint, const H*, H*))sym("clEnqueueReadBuffer");
    auto Finish = (cl_int(CLC*)(H))sym("clFinish");

    H platforms[8];
    cl_uint np = 0;
    if (GetPlatformIDs(8, platforms, &np) != 0 || np == 0) {
        std::printf("no OpenCL platform, skipped\n");
        return 0;
    }
    H dev = nullptr;
    for (cl_uint i = 0; i < np && !dev; ++i) {
        cl_uint nd = 0;
        if (GetDeviceIDs(platforms[i], 1u << 2 /* GPU */, 1, &dev, &nd) != 0) dev = nullptr;
    }
    if (!dev) {
        std::printf("no OpenCL GPU, skipped\n");
        return 0;
    }
    char name[256] = {};
    GetDeviceInfo(dev, 0x102B /* CL_DEVICE_NAME */, sizeof(name) - 1, name, nullptr);
    std::printf("       GPU: %s\n", name);

    cl_int e = 0;
    H ctx = CreateContext(nullptr, 1, &dev, nullptr, nullptr, &e);
    H q = CreateQueue(ctx, dev, 0, &e);

    // a 6000x4000 still with realistic colors
    const int W = 6000, Hh = 4000;
    std::vector<float> img(size_t(W) * Hh * 4);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (size_t i = 0; i < img.size(); i += 4) {
        V3 d = mul(kXyzToDwg, mul(kSrgbToXyz, {srgbToLin(u(rng)), srgbToLin(u(rng)), srgbToLin(u(rng))}));
        float k = std::exp2(u(rng) * 8.0f - 4.0f) * 0.5f;
        img[i] = linToDi(d.x * k);
        img[i + 1] = linToDi(d.y * k);
        img[i + 2] = linToDi(d.z * k);
        img[i + 3] = 1.0f;
    }
    const size_t bytes = img.size() * sizeof(float);
    H src = CreateBuffer(ctx, (1u << 2) | (1u << 5), bytes, img.data(), &e);
    H dst = CreateBuffer(ctx, 1u << 0, bytes, nullptr, &e);

    Settings s;
    s.anchors[0] = makeAnchor(true, {0.80f, 0.58f, 0.47f}, {0.82f, 0.62f, 0.42f}, 0.06f, 1, 0);
    Lut3D lut;
    lut.bake(65, [&s](V3 c) { return processPixel(c, s); });

    // plain LUT (Color Anchors / Look Explorer Result)
    const char* err = nullptr;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = clApplyLuts(q, src, dst, W, Hh, {lut.data().data()}, 65, Lut3D::kLo, lut.scale(), ClGrid{}, &err);
    Finish(q);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) std::printf("       error: %s\n", err ? err : "?");
    check(ok, "kernel compiles and runs (first run includes the build)");
    t0 = std::chrono::steady_clock::now();
    clApplyLuts(q, src, dst, W, Hh, {lut.data().data()}, 65, Lut3D::kLo, lut.scale(), ClGrid{}, &err);
    Finish(q);
    double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("       24 MP frame: first %.1f ms, then %.1f ms on the GPU\n", ms, ms2);

    std::vector<float> out(img.size());
    ReadBuffer(q, dst, 1, 0, bytes, out.data(), 0, nullptr, nullptr);
    float maxErr = 0;
    for (size_t i = 0; i < img.size(); i += 4 * 97) {
        V3 in = {img[i], img[i + 1], img[i + 2]};
        if (!lut.inDomain(in)) continue;
        V3 c = lut.apply(in);
        maxErr = std::fmax(maxErr, std::fmax(std::fabs(out[i] - c.x), std::fmax(std::fabs(out[i + 1] - c.y), std::fabs(out[i + 2] - c.z))));
    }
    std::printf("       GPU vs CPU LUT max diff %.6f\n", maxErr);
    check(ok && maxErr < 1e-3f, "GPU result matches the CPU LUT");

    // contact sheet with 9 looks
    std::vector<Lut3D> looks(9);
    std::vector<const float*> ptrs;
    for (int c = 0; c < 9; ++c) {
        LookParams k = prepare(finish(variation({}, 0, 1, c, 0.6f), 1, 0, 0), 1);
        looks[c].bake(65, [k](V3 v) { return applyLook(v, k); });
        ptrs.push_back(looks[c].data().data());
    }
    ClGrid g;
    g.on = true;
    g.pick = 4;
    g.gap = W / 500;
    g.blk = Hh / 100;
    t0 = std::chrono::steady_clock::now();
    ok = clApplyLuts(q, src, dst, W, Hh, ptrs, 65, Lut3D::kLo, looks[0].scale(), g, &err);
    Finish(q);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ReadBuffer(q, dst, 1, 0, bytes, out.data(), 0, nullptr, nullptr);
    bool finite = true;
    for (float v : out) finite = finite && std::isfinite(v);
    std::printf("       contact sheet: %.1f ms\n", ms);
    check(ok && finite, "3x3 contact sheet runs, output finite");
    // the pixel in the corner of the image is the dark frame
    check(std::fabs(out[0] - 0.04f) < 1e-6f, "frame color at the image corner");

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
