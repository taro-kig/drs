// StatLutEffect.h - base for plugins that measure the frame, then apply a per-pixel color
// function through the shared 65^3 LUT (OpenCL on Windows / Linux, all CPU cores otherwise).
// Also keeps the last measured frame sample, so a "Capture" button can store the frame on
// screen as a reference without fetching images outside render.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ClLut.h"
#include "Lut3D.h"
#include "Measure.h"

namespace drs {

using PixelFn = std::function<V3(V3)>;

class StatLutProcessor : public OFX::ImageProcessor {
public:
    explicit StatLutProcessor(OFX::ImageEffect& e) : OFX::ImageProcessor(e) {}
    OFX::Image* src = nullptr;
    std::shared_ptr<const Lut3D> lut;
    const PixelFn* fn = nullptr;

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* d = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, d += 4) {
                const float* s = static_cast<const float*>(src ? src->getPixelAddress(x, y) : nullptr);
                if (!s) {
                    d[0] = d[1] = d[2] = d[3] = 0.0f;
                    continue;
                }
                V3 in = {s[0], s[1], s[2]};
                V3 o = lut->inDomain(in) ? lut->apply(in) : (*fn)(in);
                d[0] = o.x, d[1] = o.y, d[2] = o.z, d[3] = s[3];
            }
        }
    }
};

class StatLutEffect : public OFX::ImageEffect {
public:
    StatLutEffect(OfxImageEffectHandle h, const char* name) : OFX::ImageEffect(h), m_Name(name)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    }

    void getRegionsOfInterest(const OFX::RegionsOfInterestArguments& args, OFX::RegionOfInterestSetter& rois) override
    {
        rois.setRegionOfInterest(*m_Src, m_Src->getRegionOfDefinition(args.time)); // whole frame is measured
    }

    void render(const OFX::RenderArguments& args) override
    {
        if (m_Dst->getPixelDepth() != OFX::eBitDepthFloat || m_Dst->getPixelComponents() != OFX::ePixelComponentRGBA)
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        std::unique_ptr<OFX::Image> dst(m_Dst->fetchImage(args.time));
        std::unique_ptr<OFX::Image> src(m_Src->fetchImage(args.time));
        if (!dst || !src) OFX::throwSuiteStatusException(kOfxStatFailed);
        if (src->getPixelDepth() != dst->getPixelDepth() || src->getPixelComponents() != dst->getPixelComponents())
            OFX::throwSuiteStatusException(kOfxStatErrValue);

        const double t = args.time;
        const bool gpu = args.isEnabledOpenCLRender;
        const char* err = nullptr;
        FrameSamples h;
        if (!measureFrame(src.get(), gpu, args.pOpenCLCmdQ, h, &err)) fail(err);
        {
            std::lock_guard<std::mutex> lock(m_LastMutex);
            m_Last = h;
            m_HaveLast = true;
        }

        std::vector<double> key;
        PixelFn fn;
        if (!build(t, h, key, fn)) fn = [](V3 c) { return c; }, key = {-1.0};
        auto lut = m_Cache.get(key, 65, fn);

        if (gpu) {
            const OfxRectI b = dst->getBounds();
            if (!clApplyLuts(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), b.x2 - b.x1, b.y2 - b.y1,
                             {lut->data().data()}, lut->size(), Lut3D::kLo, lut->scale(), ClGrid{}, &err))
                fail(err);
            return;
        }
        StatLutProcessor p(*this);
        p.setDstImg(dst.get());
        p.setRenderWindow(args.renderWindow);
        p.src = src.get();
        p.lut = lut;
        p.fn = &fn;
        p.process();
    }

protected:
    // fill key (every value that changes the result) and fn; false = pass through
    virtual bool build(double t, const FrameSamples& f, std::vector<double>& key, PixelFn& fn) = 0;

    // the sample of the last rendered frame (what is on screen), for Capture buttons
    bool lastSamples(FrameSamples& out)
    {
        std::lock_guard<std::mutex> lock(m_LastMutex);
        if (!m_HaveLast) return false;
        out = m_Last;
        return true;
    }

    void fail(const char* err)
    {
        sendMessage(OFX::Message::eMessageError, "", m_Name + ": " + (err ? err : "?"));
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    OFX::Clip* m_Dst;
    OFX::Clip* m_Src;

private:
    std::string m_Name;
    LutCache m_Cache;
    std::mutex m_LastMutex;
    FrameSamples m_Last;
    bool m_HaveLast = false;
};

// the usual filter-plugin setup shared by these plugins
inline void describeStatLut(OFX::ImageEffectDescriptor& desc, const char* name, const char* description)
{
    desc.setLabels(name, name, name);
    desc.setPluginGrouping("drs");
    desc.setPluginDescription(description);
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
#ifndef __APPLE__
    desc.setSupportsOpenCLBuffersRender(true);
#endif
}

inline void describeClips(OFX::ImageEffectDescriptor& desc)
{
    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->setTemporalClipAccess(false);
    src->setSupportsTiles(false);
    src->setIsMask(false);
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);
}

inline OFX::DoubleParamDescriptor* slider(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* parent, const char* name, const char* label,
                                          const char* hint, double def, double lo, double hi)
{
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(lo, hi);
    p->setIncrement(0.01);
    p->setDoubleType(OFX::eDoubleTypePlain);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
}

// hidden number that a Capture button fills (saved with the node, copied with it)
inline void hiddenValue(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const std::string& name)
{
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(name, name, name);
    p->setDefault(0.0);
    p->setIsSecret(true);
    p->setAnimates(false);
    page->addChild(*p);
}

} // namespace drs
