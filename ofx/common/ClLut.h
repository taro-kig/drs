// ClLut.h - apply baked 3D LUTs on the GPU through OpenCL, using the command queue and
// cl_mem buffers the host (DaVinci Resolve) hands to an OFX render.
// OpenCL is loaded at run time (OpenCL.dll / libOpenCL.so.1), so the plugin has no link
// dependency and still loads on machines without it.
#pragma once

#include <cstdint>
#include <vector>

namespace drs {

struct ClGrid {
    bool on = false; // false: one LUT over the whole frame
    int pick = 0;    // highlighted cell 0..8
    int gap = 2;     // frame width in pixels
    int blk = 2;     // digit block size in pixels
};

// src / dst: cl_mem of packed RGBA float, width x height.
// luts: nLuts grids of n^3 RGB floats back to back (9 for the contact sheet).
// Returns false with a message in `err` if OpenCL is unavailable or a call fails.
bool clApplyLuts(void* queue, void* src, void* dst, int width, int height, const std::vector<const float*>& luts,
                 int n, float lo, float scale, const ClGrid& grid, const char** err);

// Histogram of luminance (DWG / DI input), in stops from 18% grey over [lo, hi), sampling
// every `step`-th pixel in x and y. Blocks until the result is back on the host.
bool clLumaHistogram(void* queue, void* src, int width, int height, int step, int bins, float lo, float hi,
                     std::vector<uint32_t>& out, const char** err);

// Oklab hue x chroma x lightness histogram for Color Look (see colorBin() in HeroMath.h),
// sampling every `step`-th pixel. Blocks until the result is back on the host.
bool clColorHistogram(void* queue, void* src, int width, int height, int step, int bins, std::vector<uint32_t>& out,
                      const char** err);

} // namespace drs
