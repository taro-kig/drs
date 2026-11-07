// Measure.h - a sparse sample of the frame for the utility plugins (about 480 pixels across),
// gathered on the GPU (OpenCL) or read on the CPU, then measured exactly (FrameColor.h).
#pragma once

#include <algorithm>
#include <vector>

#include "ofxsImageEffect.h"

#include "ClLut.h"
#include "FrameColor.h"

namespace drs {

inline int sampleStep(int width) { return std::max(2, (width + 479) / 480); }

// Returns false (and sets err) only when the GPU path fails.
inline bool measureFrame(OFX::Image* src, bool gpu, void* queue, FrameSamples& out, const char** err)
{
    const OfxRectI b = src->getBounds();
    const int W = b.x2 - b.x1, H = b.y2 - b.y1;
    const int step = sampleStep(W);
    std::vector<V3> di;
    if (gpu) {
        std::vector<float> raw;
        if (!clGather(queue, src->getPixelData(), W, H, step, raw, err)) return false;
        di.reserve(raw.size() / 3);
        for (size_t i = 0; i + 2 < raw.size(); i += 3) di.push_back({raw[i], raw[i + 1], raw[i + 2]});
    } else {
        for (int y = b.y1; y < b.y2; y += step)
            for (int x = b.x1; x < b.x2; x += step) {
                const float* s = static_cast<const float*>(src->getPixelAddress(x, y));
                if (s) di.push_back({s[0], s[1], s[2]});
            }
    }
    out = measureSamples(di);
    return true;
}

} // namespace drs
