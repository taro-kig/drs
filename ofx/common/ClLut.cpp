// ClLut.cpp - see ClLut.h
#include "ClLut.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define CL_API_CALL __stdcall
#else
#include <dlfcn.h>
#define CL_API_CALL
#endif

namespace drs {
namespace {

// --- the small part of the OpenCL 1.2 API we use, declared here so no SDK is needed ---
typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_bitfield;
typedef cl_bitfield cl_mem_flags;
typedef cl_uint cl_command_queue_info;
typedef cl_uint cl_program_build_info;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_event* cl_event;

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_command_queue_info CL_QUEUE_CONTEXT = 0x1090;
constexpr cl_command_queue_info CL_QUEUE_DEVICE = 0x1091;
constexpr cl_mem_flags CL_MEM_READ_WRITE = 1 << 0;
constexpr cl_mem_flags CL_MEM_READ_ONLY = 1 << 2;
constexpr cl_mem_flags CL_MEM_COPY_HOST_PTR = 1 << 5;
constexpr cl_program_build_info CL_PROGRAM_BUILD_LOG = 0x1183;

struct Api {
    cl_int(CL_API_CALL* GetCommandQueueInfo)(cl_command_queue, cl_command_queue_info, size_t, void*, size_t*);
    cl_program(CL_API_CALL* CreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
    cl_int(CL_API_CALL* BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void(CL_API_CALL*)(cl_program, void*), void*);
    cl_int(CL_API_CALL* GetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
    cl_kernel(CL_API_CALL* CreateKernel)(cl_program, const char*, cl_int*);
    cl_int(CL_API_CALL* SetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
    cl_int(CL_API_CALL* EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
    cl_mem(CL_API_CALL* CreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
    cl_int(CL_API_CALL* ReleaseMemObject)(cl_mem);
    cl_int(CL_API_CALL* EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, void*, cl_uint, const cl_event*, cl_event*);
    bool ok = false;
};

template <class T>
void load(T& fn, void* lib, const char* name, bool& ok)
{
#ifdef _WIN32
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name)));
#else
    fn = reinterpret_cast<T>(dlsym(lib, name));
#endif
    ok = ok && fn != nullptr;
}

const Api& api()
{
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        void* lib = LoadLibraryA("OpenCL.dll");
#else
        void* lib = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!lib) return;
        bool ok = true;
        load(a.GetCommandQueueInfo, lib, "clGetCommandQueueInfo", ok);
        load(a.CreateProgramWithSource, lib, "clCreateProgramWithSource", ok);
        load(a.BuildProgram, lib, "clBuildProgram", ok);
        load(a.GetProgramBuildInfo, lib, "clGetProgramBuildInfo", ok);
        load(a.CreateKernel, lib, "clCreateKernel", ok);
        load(a.SetKernelArg, lib, "clSetKernelArg", ok);
        load(a.EnqueueNDRangeKernel, lib, "clEnqueueNDRangeKernel", ok);
        load(a.CreateBuffer, lib, "clCreateBuffer", ok);
        load(a.ReleaseMemObject, lib, "clReleaseMemObject", ok);
        load(a.EnqueueReadBuffer, lib, "clEnqueueReadBuffer", ok);
        a.ok = ok;
    });
    return a;
}

// ASCII only: the AMD OpenCL compiler is picky.
const char* kSource = R"CL(
__constant uchar kDigits[45] = {2,6,2,2,7, 7,1,7,4,7, 7,1,7,1,7, 5,5,7,1,1, 7,4,7,1,7,
                                7,4,7,5,7, 7,1,1,1,1, 7,5,7,5,7, 7,5,7,1,7};

float3 lutApply(__global const float* lut, int n, float lo, float scale, float3 c)
{
    float hi = lo + (float)(n - 1) / scale;
    float3 cc = clamp(c, lo, hi);
    float fr = (cc.x - lo) * scale, fg = (cc.y - lo) * scale, fb = (cc.z - lo) * scale;
    int r0 = min((int)fr, n - 2), g0 = min((int)fg, n - 2), b0 = min((int)fb, n - 2);
    float dr = fr - r0, dg = fg - g0, db = fb - b0;
    int sr = 3, sg = n * 3, sb = n * n * 3;
    int i000 = b0 * sb + g0 * sg + r0 * sr;
    int iA, iB;
    float w0, wA, wB, w1;
    if (dr > dg) {
        if (dg > db)      { iA = sr; iB = sr + sg; w0 = 1 - dr; wA = dr - dg; wB = dg - db; w1 = db; }
        else if (dr > db) { iA = sr; iB = sr + sb; w0 = 1 - dr; wA = dr - db; wB = db - dg; w1 = dg; }
        else              { iA = sb; iB = sb + sr; w0 = 1 - db; wA = db - dr; wB = dr - dg; w1 = dg; }
    } else {
        if (db > dg)      { iA = sb; iB = sb + sg; w0 = 1 - db; wA = db - dg; wB = dg - dr; w1 = dr; }
        else if (db > dr) { iA = sg; iB = sg + sb; w0 = 1 - dg; wA = dg - db; wB = db - dr; w1 = dr; }
        else              { iA = sg; iB = sg + sr; w0 = 1 - dg; wA = dg - dr; wB = dr - db; w1 = db; }
    }
    int i111 = i000 + sr + sg + sb;
    float3 o;
    o.x = w0 * lut[i000] + wA * lut[i000 + iA] + wB * lut[i000 + iB] + w1 * lut[i111];
    o.y = w0 * lut[i000 + 1] + wA * lut[i000 + iA + 1] + wB * lut[i000 + iB + 1] + w1 * lut[i111 + 1];
    o.z = w0 * lut[i000 + 2] + wA * lut[i000 + iA + 2] + wB * lut[i000 + iB + 2] + w1 * lut[i111 + 2];
    // outside the LUT range (rare super-whites / negatives): carry the difference through
    return o + (c - cc);
}

float3 px(__global const float* src, int W, int H, int x, int y)
{
    x = clamp(x, 0, W - 1);
    y = clamp(y, 0, H - 1);
    int i = (y * W + x) * 4;
    return (float3)(src[i], src[i + 1], src[i + 2]);
}

// luminance histogram in stops from 18% grey, sampled every `step` pixels
float diToLin(float t)
{
    if (t <= 0.02740668f) return t / 10.44426855f;
    return exp2(t / 0.07329248f - 7.0f) - 0.0075f;
}

__kernel void lumaHist(__global const float* src, int W, int H, int step, __global uint* hist,
                       float lo, float hi, int bins)
{
    int x = get_global_id(0) * step, y = get_global_id(1) * step;
    if (x >= W || y >= H) return;
    int i = (y * W + x) * 4;
    float Y = 0.27411851f * diToLin(src[i]) + 0.87363190f * diToLin(src[i + 1]) - 0.14775041f * diToLin(src[i + 2]);
    float e = log2(fmax(Y, 1.0e-6f) / 0.18f);
    int b = clamp((int)((e - lo) / (hi - lo) * bins), 0, bins - 1);
    atomic_inc(&hist[b]);
}

// Oklab hue x chroma x lightness histogram; mirrors colorBin() in ColorLook/HeroMath.h
__kernel void colorHist(__global const float* src, int W, int H, int step, __global uint* hist)
{
    int x = get_global_id(0) * step, y = get_global_id(1) * step;
    if (x >= W || y >= H) return;
    int i = (y * W + x) * 4;
    float r = diToLin(src[i]), g = diToLin(src[i + 1]), b = diToLin(src[i + 2]);
    float X = 0.70062239f * r + 0.14877482f * g + 0.10105872f * b;
    float Y = 0.27411851f * r + 0.87363190f * g - 0.14775041f * b;
    float Z = -0.09896291f * r - 0.13789533f * g + 1.32591599f * b;
    float l = cbrt(0.8189330101f * X + 0.3618667424f * Y - 0.1288597137f * Z);
    float m = cbrt(0.0329845436f * X + 0.9293118715f * Y + 0.0361456387f * Z);
    float s = cbrt(0.0482003018f * X + 0.2643662691f * Y + 0.6338517070f * Z);
    float L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    float A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    float B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    if (isnan(L)) return;
    float C = sqrt(A * A + B * B);
    float h = atan2(B, A) * 57.29578f;
    if (h < 0.0f) h += 360.0f;
    int hb = clamp((int)(h / 10.0f), 0, 35);
    int cb = clamp((int)(sqrt(C / 0.2f) * 8.0f), 0, 7);
    int lb = clamp((int)(L * 8.0f), 0, 7);
    atomic_inc(&hist[(hb * 8 + cb) * 8 + lb]);
}

__kernel void applyLuts(__global const float* src, __global float* dst, int W, int H,
                        __global const float* luts, int n, float lo, float scale,
                        int grid, int pick, int gap, int blk)
{
    int x = get_global_id(0), y = get_global_id(1);
    if (x >= W || y >= H) return;
    int o = (y * W + x) * 4;
    if (grid == 0) {
        float3 r = lutApply(luts, n, lo, scale, (float3)(src[o], src[o + 1], src[o + 2]));
        dst[o] = r.x; dst[o + 1] = r.y; dst[o + 2] = r.z; dst[o + 3] = src[o + 3];
        return;
    }

    // 3x3 contact sheet, cell 0 at the top left (y = 0 is the bottom row of the image)
    int col = min(2, x * 3 / W), row = min(2, (H - 1 - y) * 3 / H);
    int cell = row * 3 + col;
    int cx0 = col * W / 3, cx1 = (col + 1) * W / 3;
    int cyTop = H - row * H / 3, cyBot = H - (row + 1) * H / 3;
    int edge = min(min(x - cx0, cx1 - 1 - x), min(y - cyBot, cyTop - 1 - y));
    float3 c;
    float3 gold = (float3)(0.62f, 0.55f, 0.12f);
    if (edge < gap) c = (float3)(0.04f);
    else if (cell == pick && edge < gap * 4) c = gold;
    else if (x - cx0 >= gap * 5 && cyTop - 1 - y >= gap * 5 &&
             (x - cx0 - gap * 5) / blk < 5 && (cyTop - 1 - y - gap * 5) / blk < 7) {
        int bx = (x - cx0 - gap * 5) / blk, by = (cyTop - 1 - y - gap * 5) / blk;
        bool on = bx >= 1 && bx <= 3 && by >= 1 && by <= 5 && ((kDigits[cell * 5 + by - 1] >> (3 - bx)) & 1);
        c = on ? (cell == pick ? gold : (float3)(0.58f)) : (float3)(0.06f);
    } else {
        float fx = (x - cx0 + 0.5f) / (float)(cx1 - cx0);
        float fy = (y - cyBot + 0.5f) / (float)(cyTop - cyBot);
        int sx = (int)(fx * W), sy = (int)(fy * H);
        float3 acc = (float3)(0.0f);
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) acc += px(src, W, H, sx + i, sy + j);
        c = lutApply(luts + cell * n * n * n * 3, n, lo, scale, acc / 9.0f);
    }
    dst[o] = c.x; dst[o + 1] = c.y; dst[o + 2] = c.z; dst[o + 3] = 1.0f;
}
)CL";

// Photo Frame kernels: a separate program keeps each source literal small (MSVC limit).
const char* kFrameSource = R"CL(
// largest |value| of every column / row, for finding Resolve's black bars
__kernel void colMax(__global const float* src, int W, int H, __global float* out)
{
    int x = get_global_id(0);
    if (x >= W) return;
    float m = 0.0f;
    for (int y = 0; y < H; y++) {
        int i = (y * W + x) * 4;
        m = fmax(m, fmax(fabs(src[i]), fmax(fabs(src[i + 1]), fabs(src[i + 2]))));
    }
    out[x] = m;
}

__kernel void rowMax(__global const float* src, int W, int H, __global float* out)
{
    int y = get_global_id(0);
    if (y >= H) return;
    float m = 0.0f;
    for (int x = 0; x < W; x++) {
        int i = (y * W + x) * 4;
        m = fmax(m, fmax(fabs(src[i]), fmax(fabs(src[i + 1]), fabs(src[i + 2]))));
    }
    out[y] = m;
}

float cr(float t, float a, float b, float c, float d)
{
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

float3 fetch(__global const float* src, int W, int x, int y)
{
    int i = (y * W + x) * 4;
    return (float3)(src[i], src[i + 1], src[i + 2]);
}

// geo: content x0 y0 w h | photo x0 y0 w h (floats) ; col: bg rgb, text rgb, edge rgb, edge width
__kernel void frame(__global const float* src, __global float* dst, int W, int H,
                    int cx0, int cy0, int cw, int ch, float px0, float py0, float pw, float ph,
                    __global const float* col, __global const uchar* mask, int tw, int th, int tx, int ttop)
{
    int x = get_global_id(0), y = get_global_id(1);
    if (x >= W || y >= H) return;
    float3 bg = (float3)(col[0], col[1], col[2]);
    float3 tc = (float3)(col[3], col[4], col[5]);
    float3 ec = (float3)(col[6], col[7], col[8]);
    float ew = col[9];
    float u = x + 0.5f, v = y + 0.5f;
    float3 o = bg;
    if (u >= px0 && u < px0 + pw && v >= py0 && v < py0 + ph) {
        float sx = cx0 + (u - px0) / pw * cw - 0.5f, sy = cy0 + (v - py0) / ph * ch - 0.5f;
        int ix = (int)floor(sx), iy = (int)floor(sy);
        float fx = sx - ix, fy = sy - iy;
        float3 r[4];
        for (int j = 0; j < 4; j++) {
            int yy = clamp(iy - 1 + j, cy0, cy0 + ch - 1);
            float3 p0 = fetch(src, W, clamp(ix - 1, cx0, cx0 + cw - 1), yy);
            float3 p1 = fetch(src, W, clamp(ix, cx0, cx0 + cw - 1), yy);
            float3 p2 = fetch(src, W, clamp(ix + 1, cx0, cx0 + cw - 1), yy);
            float3 p3 = fetch(src, W, clamp(ix + 2, cx0, cx0 + cw - 1), yy);
            r[j] = (float3)(cr(fx, p0.x, p1.x, p2.x, p3.x), cr(fx, p0.y, p1.y, p2.y, p3.y), cr(fx, p0.z, p1.z, p2.z, p3.z));
        }
        o = (float3)(cr(fy, r[0].x, r[1].x, r[2].x, r[3].x), cr(fy, r[0].y, r[1].y, r[2].y, r[3].y),
                     cr(fy, r[0].z, r[1].z, r[2].z, r[3].z));
    } else if (ew > 0.0f && u >= px0 - ew && u < px0 + pw + ew && v >= py0 - ew && v < py0 + ph + ew) {
        o = ec;
    } else if (tw > 0) {
        int mx = x - tx, my = ttop - y;
        if (mx >= 0 && mx < tw && my >= 0 && my < th) {
            float a = mask[my * tw + mx] / 255.0f;
            o = bg + (tc - bg) * a;
        }
    }
    int i = (y * W + x) * 4;
    dst[i] = o.x; dst[i + 1] = o.y; dst[i + 2] = o.z; dst[i + 3] = 1.0f;
}
)CL";

struct Compiled {
    cl_kernel kernel = nullptr;
    cl_kernel hist = nullptr;
    cl_kernel colorHist = nullptr;
    cl_kernel colMax = nullptr, rowMax = nullptr, frame = nullptr;
    std::string error, frameError;
};

std::mutex g_Mutex; // kernels are shared, and clSetKernelArg is not thread safe
std::map<cl_context, Compiled> g_Kernels;

Compiled& kernelFor(const Api& a, cl_context ctx, cl_device_id dev)
{
    auto it = g_Kernels.find(ctx);
    if (it != g_Kernels.end()) return it->second;
    Compiled& c = g_Kernels[ctx];
    cl_int e = 0;
    cl_program prog = a.CreateProgramWithSource(ctx, 1, &kSource, nullptr, &e);
    if (e != CL_SUCCESS) {
        c.error = "clCreateProgramWithSource failed";
        return c;
    }
    e = a.BuildProgram(prog, 1, &dev, "-cl-fast-relaxed-math", nullptr, nullptr);
    if (e != CL_SUCCESS) {
        char log[2048] = {};
        a.GetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof(log) - 1, log, nullptr);
        c.error = std::string("OpenCL build failed: ") + log;
        return c;
    }
    c.kernel = a.CreateKernel(prog, "applyLuts", &e);
    if (e != CL_SUCCESS) c.error = "clCreateKernel failed";
    cl_int e2 = 0;
    c.hist = a.CreateKernel(prog, "lumaHist", &e2);
    if (e2 != CL_SUCCESS) c.hist = nullptr;
    c.colorHist = a.CreateKernel(prog, "colorHist", &e2);
    if (e2 != CL_SUCCESS) c.colorHist = nullptr;

    cl_program fp = a.CreateProgramWithSource(ctx, 1, &kFrameSource, nullptr, &e2);
    if (e2 == CL_SUCCESS) e2 = a.BuildProgram(fp, 1, &dev, "", nullptr, nullptr);
    if (e2 != CL_SUCCESS) {
        char log[2048] = {};
        if (fp) a.GetProgramBuildInfo(fp, dev, CL_PROGRAM_BUILD_LOG, sizeof(log) - 1, log, nullptr);
        c.frameError = std::string("OpenCL frame build failed: ") + log;
        return c;
    }
    c.colMax = a.CreateKernel(fp, "colMax", &e2);
    c.rowMax = a.CreateKernel(fp, "rowMax", &e2);
    c.frame = a.CreateKernel(fp, "frame", &e2);
    if (!c.colMax || !c.rowMax || !c.frame) c.frameError = "clCreateKernel (frame) failed";
    return c;
}

} // namespace

bool clApplyLuts(void* queue, void* src, void* dst, int width, int height, const std::vector<const float*>& luts,
                 int n, float lo, float scale, const ClGrid& grid, const char** err)
{
    static std::string s_Err;
    auto fail = [&](const std::string& m) {
        s_Err = m;
        if (err) *err = s_Err.c_str();
        return false;
    };
    const Api& a = api();
    if (!a.ok) return fail("OpenCL runtime not found");
    if (!queue || !src || !dst || luts.empty()) return fail("missing OpenCL queue or buffers");

    cl_command_queue q = static_cast<cl_command_queue>(queue);
    cl_context ctx = nullptr;
    cl_device_id dev = nullptr;
    if (a.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof(ctx), &ctx, nullptr) != CL_SUCCESS ||
        a.GetCommandQueueInfo(q, CL_QUEUE_DEVICE, sizeof(dev), &dev, nullptr) != CL_SUCCESS) {
        return fail("clGetCommandQueueInfo failed");
    }

    // all LUTs back to back in one buffer
    const size_t per = size_t(n) * n * n * 3;
    std::vector<float> packed(per * luts.size());
    for (size_t i = 0; i < luts.size(); ++i) std::memcpy(&packed[i * per], luts[i], per * sizeof(float));

    std::lock_guard<std::mutex> lock(g_Mutex);
    Compiled& k = kernelFor(a, ctx, dev);
    if (!k.kernel) return fail(k.error);

    cl_int e = 0;
    cl_mem lutBuf = a.CreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, packed.size() * sizeof(float),
                                   packed.data(), &e);
    if (e != CL_SUCCESS) return fail("clCreateBuffer failed");

    cl_mem s = static_cast<cl_mem>(src), d = static_cast<cl_mem>(dst);
    int g = grid.on ? 1 : 0;
    cl_uint i = 0;
    e |= a.SetKernelArg(k.kernel, i++, sizeof(cl_mem), &s);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(cl_mem), &d);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &width);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &height);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(cl_mem), &lutBuf);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &n);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(float), &lo);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(float), &scale);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &g);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &grid.pick);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &grid.gap);
    e |= a.SetKernelArg(k.kernel, i++, sizeof(int), &grid.blk);
    if (e != CL_SUCCESS) {
        a.ReleaseMemObject(lutBuf);
        return fail("clSetKernelArg failed");
    }
    const size_t local[2] = {16, 16};
    const size_t global[2] = {(size_t(width) + 15) / 16 * 16, (size_t(height) + 15) / 16 * 16};
    e = a.EnqueueNDRangeKernel(q, k.kernel, 2, nullptr, global, local, 0, nullptr, nullptr);
    // safe right after enqueue: OpenCL keeps the buffer alive until the kernel is done
    a.ReleaseMemObject(lutBuf);
    if (e != CL_SUCCESS) return fail("clEnqueueNDRangeKernel failed");
    return true;
}

bool clLumaHistogram(void* queue, void* src, int width, int height, int step, int bins, float lo, float hi,
                     std::vector<uint32_t>& out, const char** err)
{
    static std::string s_Err;
    auto fail = [&](const std::string& m) {
        s_Err = m;
        if (err) *err = s_Err.c_str();
        return false;
    };
    const Api& a = api();
    if (!a.ok) return fail("OpenCL runtime not found");
    cl_command_queue q = static_cast<cl_command_queue>(queue);
    cl_context ctx = nullptr;
    cl_device_id dev = nullptr;
    if (a.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof(ctx), &ctx, nullptr) != CL_SUCCESS ||
        a.GetCommandQueueInfo(q, CL_QUEUE_DEVICE, sizeof(dev), &dev, nullptr) != CL_SUCCESS) {
        return fail("clGetCommandQueueInfo failed");
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    Compiled& k = kernelFor(a, ctx, dev);
    if (!k.hist) return fail(k.error.empty() ? "histogram kernel missing" : k.error);

    out.assign(size_t(bins), 0u);
    cl_int e = 0;
    cl_mem h = a.CreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, out.size() * sizeof(uint32_t),
                              out.data(), &e);
    if (e != CL_SUCCESS) return fail("clCreateBuffer failed");
    cl_mem s = static_cast<cl_mem>(src);
    cl_uint i = 0;
    e |= a.SetKernelArg(k.hist, i++, sizeof(cl_mem), &s);
    e |= a.SetKernelArg(k.hist, i++, sizeof(int), &width);
    e |= a.SetKernelArg(k.hist, i++, sizeof(int), &height);
    e |= a.SetKernelArg(k.hist, i++, sizeof(int), &step);
    e |= a.SetKernelArg(k.hist, i++, sizeof(cl_mem), &h);
    e |= a.SetKernelArg(k.hist, i++, sizeof(float), &lo);
    e |= a.SetKernelArg(k.hist, i++, sizeof(float), &hi);
    e |= a.SetKernelArg(k.hist, i++, sizeof(int), &bins);
    const size_t local[2] = {16, 16};
    const size_t gx = (size_t(width) + step - 1) / step, gy = (size_t(height) + step - 1) / step;
    const size_t global[2] = {(gx + 15) / 16 * 16, (gy + 15) / 16 * 16};
    if (e == CL_SUCCESS) e = a.EnqueueNDRangeKernel(q, k.hist, 2, nullptr, global, local, 0, nullptr, nullptr);
    if (e == CL_SUCCESS) e = a.EnqueueReadBuffer(q, h, 1, 0, out.size() * sizeof(uint32_t), out.data(), 0, nullptr, nullptr);
    a.ReleaseMemObject(h);
    if (e != CL_SUCCESS) return fail("OpenCL histogram failed");
    return true;
}

bool clColorHistogram(void* queue, void* src, int width, int height, int step, int bins, std::vector<uint32_t>& out,
                      const char** err)
{
    static std::string s_Err;
    auto fail = [&](const std::string& m) {
        s_Err = m;
        if (err) *err = s_Err.c_str();
        return false;
    };
    const Api& a = api();
    if (!a.ok) return fail("OpenCL runtime not found");
    cl_command_queue q = static_cast<cl_command_queue>(queue);
    cl_context ctx = nullptr;
    cl_device_id dev = nullptr;
    if (a.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof(ctx), &ctx, nullptr) != CL_SUCCESS ||
        a.GetCommandQueueInfo(q, CL_QUEUE_DEVICE, sizeof(dev), &dev, nullptr) != CL_SUCCESS) {
        return fail("clGetCommandQueueInfo failed");
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    Compiled& k = kernelFor(a, ctx, dev);
    if (!k.colorHist) return fail(k.error.empty() ? "color histogram kernel missing" : k.error);

    out.assign(size_t(bins), 0u);
    cl_int e = 0;
    cl_mem h = a.CreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, out.size() * sizeof(uint32_t),
                              out.data(), &e);
    if (e != CL_SUCCESS) return fail("clCreateBuffer failed");
    cl_mem s = static_cast<cl_mem>(src);
    cl_uint i = 0;
    e |= a.SetKernelArg(k.colorHist, i++, sizeof(cl_mem), &s);
    e |= a.SetKernelArg(k.colorHist, i++, sizeof(int), &width);
    e |= a.SetKernelArg(k.colorHist, i++, sizeof(int), &height);
    e |= a.SetKernelArg(k.colorHist, i++, sizeof(int), &step);
    e |= a.SetKernelArg(k.colorHist, i++, sizeof(cl_mem), &h);
    const size_t local[2] = {16, 16};
    const size_t gx = (size_t(width) + step - 1) / step, gy = (size_t(height) + step - 1) / step;
    const size_t global[2] = {(gx + 15) / 16 * 16, (gy + 15) / 16 * 16};
    if (e == CL_SUCCESS) e = a.EnqueueNDRangeKernel(q, k.colorHist, 2, nullptr, global, local, 0, nullptr, nullptr);
    if (e == CL_SUCCESS) e = a.EnqueueReadBuffer(q, h, 1, 0, out.size() * sizeof(uint32_t), out.data(), 0, nullptr, nullptr);
    a.ReleaseMemObject(h);
    if (e != CL_SUCCESS) return fail("OpenCL color histogram failed");
    return true;
}

bool clContentMax(void* queue, void* src, int width, int height, std::vector<float>& colMax,
                  std::vector<float>& rowMax, const char** err)
{
    static std::string s_Err;
    auto fail = [&](const std::string& m) {
        s_Err = m;
        if (err) *err = s_Err.c_str();
        return false;
    };
    const Api& a = api();
    if (!a.ok) return fail("OpenCL runtime not found");
    cl_command_queue q = static_cast<cl_command_queue>(queue);
    cl_context ctx = nullptr;
    cl_device_id dev = nullptr;
    if (a.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof(ctx), &ctx, nullptr) != CL_SUCCESS ||
        a.GetCommandQueueInfo(q, CL_QUEUE_DEVICE, sizeof(dev), &dev, nullptr) != CL_SUCCESS) {
        return fail("clGetCommandQueueInfo failed");
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    Compiled& k = kernelFor(a, ctx, dev);
    if (!k.colMax) return fail(k.frameError.empty() ? k.error : k.frameError);

    colMax.assign(size_t(width), 0.0f);
    rowMax.assign(size_t(height), 0.0f);
    cl_int e = 0;
    cl_mem cb = a.CreateBuffer(ctx, CL_MEM_READ_WRITE, colMax.size() * sizeof(float), nullptr, &e);
    cl_mem rb = a.CreateBuffer(ctx, CL_MEM_READ_WRITE, rowMax.size() * sizeof(float), nullptr, &e);
    cl_mem s = static_cast<cl_mem>(src);
    struct Job {
        cl_kernel k;
        cl_mem out;
        size_t n;
    } jobs[2] = {{k.colMax, cb, size_t(width)}, {k.rowMax, rb, size_t(height)}};
    for (Job& j : jobs) {
        cl_uint i = 0;
        e |= a.SetKernelArg(j.k, i++, sizeof(cl_mem), &s);
        e |= a.SetKernelArg(j.k, i++, sizeof(int), &width);
        e |= a.SetKernelArg(j.k, i++, sizeof(int), &height);
        e |= a.SetKernelArg(j.k, i++, sizeof(cl_mem), &j.out);
        const size_t global = (j.n + 63) / 64 * 64, local = 64;
        if (e == CL_SUCCESS) e = a.EnqueueNDRangeKernel(q, j.k, 1, nullptr, &global, &local, 0, nullptr, nullptr);
    }
    if (e == CL_SUCCESS) e = a.EnqueueReadBuffer(q, cb, 1, 0, colMax.size() * sizeof(float), colMax.data(), 0, nullptr, nullptr);
    if (e == CL_SUCCESS) e = a.EnqueueReadBuffer(q, rb, 1, 0, rowMax.size() * sizeof(float), rowMax.data(), 0, nullptr, nullptr);
    if (cb) a.ReleaseMemObject(cb);
    if (rb) a.ReleaseMemObject(rb);
    if (e != CL_SUCCESS) return fail("OpenCL content scan failed");
    return true;
}

bool clFrame(void* queue, void* src, void* dst, int width, int height, const ClFrameArgs& f, const char** err)
{
    static std::string s_Err;
    auto fail = [&](const std::string& m) {
        s_Err = m;
        if (err) *err = s_Err.c_str();
        return false;
    };
    const Api& a = api();
    if (!a.ok) return fail("OpenCL runtime not found");
    cl_command_queue q = static_cast<cl_command_queue>(queue);
    cl_context ctx = nullptr;
    cl_device_id dev = nullptr;
    if (a.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof(ctx), &ctx, nullptr) != CL_SUCCESS ||
        a.GetCommandQueueInfo(q, CL_QUEUE_DEVICE, sizeof(dev), &dev, nullptr) != CL_SUCCESS) {
        return fail("clGetCommandQueueInfo failed");
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    Compiled& k = kernelFor(a, ctx, dev);
    if (!k.frame) return fail(k.frameError.empty() ? k.error : k.frameError);

    cl_int e = 0;
    float colors[10];
    std::memcpy(colors, f.colors, sizeof(colors));
    cl_mem colBuf = a.CreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(colors), colors, &e);
    unsigned char dummy = 0;
    const bool hasMask = f.mask && f.tw > 0 && f.th > 0;
    cl_mem maskBuf = a.CreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    hasMask ? size_t(f.tw) * f.th : 1, hasMask ? (void*)f.mask : &dummy, &e);
    if (!colBuf || !maskBuf) {
        if (colBuf) a.ReleaseMemObject(colBuf);
        if (maskBuf) a.ReleaseMemObject(maskBuf);
        return fail("clCreateBuffer failed");
    }
    int tw = hasMask ? f.tw : 0, th = hasMask ? f.th : 0;
    cl_mem s = static_cast<cl_mem>(src), d = static_cast<cl_mem>(dst);
    cl_uint i = 0;
    e = CL_SUCCESS;
    e |= a.SetKernelArg(k.frame, i++, sizeof(cl_mem), &s);
    e |= a.SetKernelArg(k.frame, i++, sizeof(cl_mem), &d);
    e |= a.SetKernelArg(k.frame, i++, sizeof(int), &width);
    e |= a.SetKernelArg(k.frame, i++, sizeof(int), &height);
    for (int v : {f.cx0, f.cy0, f.cw, f.ch}) e |= a.SetKernelArg(k.frame, i++, sizeof(int), &v);
    for (float v : {f.px0, f.py0, f.pw, f.ph}) e |= a.SetKernelArg(k.frame, i++, sizeof(float), &v);
    e |= a.SetKernelArg(k.frame, i++, sizeof(cl_mem), &colBuf);
    e |= a.SetKernelArg(k.frame, i++, sizeof(cl_mem), &maskBuf);
    for (int v : {tw, th, f.tx, f.ttop}) e |= a.SetKernelArg(k.frame, i++, sizeof(int), &v);
    const size_t local[2] = {16, 16};
    const size_t global[2] = {(size_t(width) + 15) / 16 * 16, (size_t(height) + 15) / 16 * 16};
    if (e == CL_SUCCESS) e = a.EnqueueNDRangeKernel(q, k.frame, 2, nullptr, global, local, 0, nullptr, nullptr);
    a.ReleaseMemObject(colBuf);
    a.ReleaseMemObject(maskBuf);
    if (e != CL_SUCCESS) return fail("OpenCL frame render failed");
    return true;
}

} // namespace drs