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

// Photo Frame: largest |value| per column and per row (to find black bars). Blocks.
bool clContentMax(void* queue, void* src, int width, int height, std::vector<float>& colMax,
                  std::vector<float>& rowMax, const char** err);

struct ClFrameArgs {
    int cx0 = 0, cy0 = 0, cw = 1, ch = 1;          // source content rect (y up)
    float px0 = 0, py0 = 0, pw = 1, ph = 1;        // photo rect in the output (y up)
    float colors[10] = {};                         // bg rgb, text rgb, edge rgb, edge width
    const unsigned char* mask = nullptr;           // caption alpha, tw x th, row 0 = top
    int tw = 0, th = 0, tx = 0, ttop = 0;          // caption position: left x, top row (y up)
};

// Photo Frame: draws frame, scaled photo (bicubic), edge line and caption.
bool clFrame(void* queue, void* src, void* dst, int width, int height, const ClFrameArgs& f, const char** err);

} // namespace drs
