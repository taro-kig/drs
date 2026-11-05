// Lut3D.h - bake any per-pixel color function into a 3D LUT once, then apply it with
// tetrahedral interpolation. Turns a heavy per-pixel color model (Oklab, exp, cbrt...)
// into a few lookups. No OFX dependency.
//
// Domain: DaVinci Intermediate code values in [kLo, kHi] per channel. Pixels outside
// (super-whites, negatives) are reported so the caller can compute them directly.
#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ColorMath.h"

namespace drs {

class Lut3D {
public:
    static constexpr float kLo = -0.05f;
    static constexpr float kHi = 1.05f;

    // f: V3 (DI) -> V3 (DI). Runs on all cores.
    template <class F>
    void bake(int n, const F& f)
    {
        m_N = n;
        m_Scale = float(n - 1) / (kHi - kLo);
        m_Data.assign(size_t(n) * n * n * 3, 0.0f);
        unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        threads = std::min<unsigned>(threads, unsigned(n));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([this, &f, n, t, threads] {
                const float step = (kHi - kLo) / float(n - 1);
                for (int b = int(t); b < n; b += int(threads))
                    for (int g = 0; g < n; ++g)
                        for (int r = 0; r < n; ++r) {
                            V3 o = f(V3{kLo + r * step, kLo + g * step, kLo + b * step});
                            float* d = &m_Data[(size_t(b) * n * n + size_t(g) * n + r) * 3];
                            d[0] = o.x;
                            d[1] = o.y;
                            d[2] = o.z;
                        }
            });
        }
        for (auto& th : pool) th.join();
    }

    bool inDomain(V3 c) const
    {
        return c.x >= kLo && c.x <= kHi && c.y >= kLo && c.y <= kHi && c.z >= kLo && c.z <= kHi;
    }

    // Tetrahedral interpolation. Only call for inDomain() input.
    V3 apply(V3 c) const
    {
        const int n = m_N;
        float fr = (c.x - kLo) * m_Scale, fg = (c.y - kLo) * m_Scale, fb = (c.z - kLo) * m_Scale;
        int r0 = std::min(int(fr), n - 2), g0 = std::min(int(fg), n - 2), b0 = std::min(int(fb), n - 2);
        float dr = fr - r0, dg = fg - g0, db = fb - b0;

        const size_t sr = 3, sg = size_t(n) * 3, sb = size_t(n) * n * 3;
        const float* p000 = &m_Data[size_t(b0) * sb + size_t(g0) * sg + size_t(r0) * sr];
        const float* p111 = p000 + sr + sg + sb;
        const float *pA, *pB;
        float w0, wA, wB, w1;
        if (dr > dg) {
            if (dg > db) {        // r > g > b
                pA = p000 + sr; pB = p000 + sr + sg;
                w0 = 1 - dr; wA = dr - dg; wB = dg - db; w1 = db;
            } else if (dr > db) { // r > b > g
                pA = p000 + sr; pB = p000 + sr + sb;
                w0 = 1 - dr; wA = dr - db; wB = db - dg; w1 = dg;
            } else {              // b > r > g
                pA = p000 + sb; pB = p000 + sb + sr;
                w0 = 1 - db; wA = db - dr; wB = dr - dg; w1 = dg;
            }
        } else {
            if (db > dg) {        // b > g > r
                pA = p000 + sb; pB = p000 + sb + sg;
                w0 = 1 - db; wA = db - dg; wB = dg - dr; w1 = dr;
            } else if (db > dr) { // g > b > r
                pA = p000 + sg; pB = p000 + sg + sb;
                w0 = 1 - dg; wA = dg - db; wB = db - dr; w1 = dr;
            } else {              // g > r > b
                pA = p000 + sg; pB = p000 + sg + sr;
                w0 = 1 - dg; wA = dg - dr; wB = dr - db; w1 = db;
            }
        }
        return {w0 * p000[0] + wA * pA[0] + wB * pB[0] + w1 * p111[0],
                w0 * p000[1] + wA * pA[1] + wB * pB[1] + w1 * p111[1],
                w0 * p000[2] + wA * pA[2] + wB * pB[2] + w1 * p111[2]};
    }

    // raw grid for GPU upload: index ((b * n + g) * n + r) * 3
    int size() const { return m_N; }
    float scale() const { return m_Scale; }
    const std::vector<float>& data() const { return m_Data; }

private:
    int m_N = 0;
    float m_Scale = 1.0f;
    std::vector<float> m_Data;
};

// Keeps the last baked LUT for a set of parameter values, so re-renders with the same
// settings (scrubbing, viewer refresh, other frames) skip the bake. Thread safe.
class LutCache {
public:
    template <class F>
    std::shared_ptr<const Lut3D> get(const std::vector<double>& key, int n, const F& f)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Lut && key == m_Key && n == m_N) return m_Lut;
        auto lut = std::make_shared<Lut3D>();
        lut->bake(n, f);
        m_Key = key;
        m_N = n;
        m_Lut = lut;
        return m_Lut;
    }

private:
    std::mutex m_Mutex;
    std::vector<double> m_Key;
    int m_N = 0;
    std::shared_ptr<const Lut3D> m_Lut;
};

} // namespace drs
