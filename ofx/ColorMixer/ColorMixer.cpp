// Color Mixer - OFX plugin for DaVinci Resolve
// Photo-style HSL mixer: Hue / Saturation / Luminance sliders for 7 colors.
// Works on DaVinci Wide Gamut / DaVinci Intermediate, like the DCTLs in this repo.
// Rendering: the math is baked into a cached 65^3 LUT, applied on the GPU (OpenCL)
// when the host offers it, otherwise on all CPU cores.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ClLut.h"
#include "Lut3D.h"
#include "MixerMath.h"

#define kPluginName "Color Mixer"
#define kPluginGrouping "drs"
#define kPluginDescription                                                                   \
    "Hue, Saturation and Luminance for Red, Orange, Yellow, Green, Aqua, Blue and Magenta, " \
    "like the HSL panel of a photo editor. Orange covers skin, Green covers foliage. "       \
    "Neighboring colors blend without seams; greys and blacks are never touched.\n"         \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."
#define kPluginIdentifier "io.github.taro-kig.drs.ColorMixer"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

using namespace OFX;

namespace {

// color chips for the labels (UTF-8): red, orange, yellow, green, white (aqua), blue, purple
const char* const kChips[drs::kMixColors] = {"\xF0\x9F\x9F\xA5", "\xF0\x9F\x9F\xA7", "\xF0\x9F\x9F\xA8",
                                             "\xF0\x9F\x9F\xA9", "\xE2\xAC\x9C",     "\xF0\x9F\x9F\xA6",
                                             "\xF0\x9F\x9F\xAA"};
const char* const kKinds[3] = {"hue", "sat", "lum"};

std::string pid(const char* base, int i)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%d", base, i);
    return buf;
}

class MixerProcessor : public ImageProcessor {
public:
    explicit MixerProcessor(ImageEffect& effect) : ImageProcessor(effect) {}

    Image* src = nullptr;
    drs::MixerSettings settings;
    std::shared_ptr<const drs::Lut3D> lut;

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* dst = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, dst += 4) {
                const float* s = static_cast<const float*>(src ? src->getPixelAddress(x, y) : nullptr);
                if (!s) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
                    continue;
                }
                drs::V3 in = {s[0], s[1], s[2]};
                drs::V3 o = lut->inDomain(in) ? lut->apply(in) : drs::mixPixel(in, settings);
                dst[0] = o.x;
                dst[1] = o.y;
                dst[2] = o.z;
                dst[3] = s[3];
            }
        }
    }
};

class ColorMixerPlugin : public ImageEffect {
public:
    explicit ColorMixerPlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        for (int i = 0; i < drs::kMixColors; ++i) {
            m_Hue[i] = fetchDoubleParam(pid("hue", i));
            m_Sat[i] = fetchDoubleParam(pid("sat", i));
            m_Lum[i] = fetchDoubleParam(pid("lum", i));
        }
        m_Mask = fetchChoiceParam("showMask");
        m_Mix = fetchDoubleParam("mix");
    }

    void render(const RenderArguments& args) override
    {
        if (m_Dst->getPixelDepth() != eBitDepthFloat || m_Dst->getPixelComponents() != ePixelComponentRGBA) {
            throwSuiteStatusException(kOfxStatErrUnsupported);
        }
        std::unique_ptr<Image> dst(m_Dst->fetchImage(args.time));
        std::unique_ptr<Image> src(m_Src->fetchImage(args.time));
        if (!dst || !src) throwSuiteStatusException(kOfxStatFailed);
        if (src->getPixelDepth() != dst->getPixelDepth() || src->getPixelComponents() != dst->getPixelComponents()) {
            throwSuiteStatusException(kOfxStatErrValue);
        }

        drs::MixerSettings s = readSettings(args.time);
        std::vector<double> key;
        for (int i = 0; i < drs::kMixColors; ++i) {
            key.push_back(s.hue[i]);
            key.push_back(s.sat[i]);
            key.push_back(s.lum[i]);
        }
        key.push_back(s.solo);
        key.push_back(s.mix);
        auto lut = m_Cache.get(key, 65, [s](drs::V3 c) { return drs::mixPixel(c, s); });

        if (args.isEnabledOpenCLRender) {
            const OfxRectI b = dst->getBounds();
            const char* err = nullptr;
            if (!drs::clApplyLuts(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), b.x2 - b.x1,
                                  b.y2 - b.y1, {lut->data().data()}, lut->size(), drs::Lut3D::kLo, lut->scale(),
                                  drs::ClGrid{}, &err)) {
                sendMessage(Message::eMessageError, "", std::string("Color Mixer GPU: ") + (err ? err : "?"));
                throwSuiteStatusException(kOfxStatFailed);
            }
            return;
        }

        MixerProcessor proc(*this);
        proc.setDstImg(dst.get());
        proc.setRenderWindow(args.renderWindow);
        proc.src = src.get();
        proc.settings = s;
        proc.lut = lut;
        proc.process();
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime) override
    {
        if (drs::mixerIsIdentity(readSettings(args.time))) {
            identityClip = m_Src;
            identityTime = args.time;
            return true;
        }
        return false;
    }

private:
    drs::MixerSettings readSettings(double t)
    {
        drs::MixerSettings s;
        for (int i = 0; i < drs::kMixColors; ++i) {
            s.hue[i] = float(m_Hue[i]->getValueAtTime(t));
            s.sat[i] = float(m_Sat[i]->getValueAtTime(t));
            s.lum[i] = float(m_Lum[i]->getValueAtTime(t));
        }
        int mask = 0;
        m_Mask->getValueAtTime(t, mask);
        s.solo = mask - 1;
        s.mix = float(m_Mix->getValueAtTime(t));
        return s;
    }

    drs::LutCache m_Cache;
    Clip* m_Dst;
    Clip* m_Src;
    DoubleParam* m_Hue[drs::kMixColors];
    DoubleParam* m_Sat[drs::kMixColors];
    DoubleParam* m_Lum[drs::kMixColors];
    ChoiceParam* m_Mask;
    DoubleParam* m_Mix;
};

void defineSlider(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* parent,
                  const std::string& name, const std::string& label, const char* hint, double def, double lo, double hi)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(lo, hi);
    p->setIncrement(0.01);
    p->setDoubleType(eDoubleTypePlain);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
}

} // namespace

mDeclarePluginFactory(ColorMixerFactory, {}, {});

void ColorMixerFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
#ifndef __APPLE__
    desc.setSupportsOpenCLBuffersRender(true); // DaVinci Resolve on Windows / Linux
#endif
}

void ColorMixerFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
{
    ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(ePixelComponentRGBA);
    src->setTemporalClipAccess(false);
    src->setSupportsTiles(false);
    src->setIsMask(false);

    ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    PageParamDescriptor* page = desc.definePageParam("Controls");

    const char* groupLabels[3] = {"Hue", "Saturation", "Luminance"};
    const char* hints[3] = {"Shift this color's hue (+ / - up to 25 degrees toward its neighbors).",
                            "Less (-) or more (+) saturation for this color. -1 = grey.",
                            "Darker (-) or brighter (+) for this color, about +-1 stop."};
    for (int k = 0; k < 3; ++k) {
        GroupParamDescriptor* grp = desc.defineGroupParam(std::string("group_") + kKinds[k]);
        grp->setLabels(groupLabels[k], groupLabels[k], groupLabels[k]);
        grp->setOpen(true);
        page->addChild(*grp);
        for (int i = 0; i < drs::kMixColors; ++i) {
            std::string label = std::string(drs::kMixNames[i]) + " " + kChips[i];
            defineSlider(desc, page, grp, pid(kKinds[k], i), label, hints[k], 0.0, -1.0, 1.0);
        }
    }

    ChoiceParamDescriptor* mask = desc.defineChoiceParam("showMask");
    mask->setLabels("Show Mask", "Show Mask", "Show Mask");
    mask->setHint("Show where one color band acts (white = fully).");
    mask->appendOption("Off");
    for (int i = 0; i < drs::kMixColors; ++i) mask->appendOption(drs::kMixNames[i]);
    mask->setDefault(0);
    mask->setAnimates(false);
    page->addChild(*mask);

    defineSlider(desc, page, nullptr, "mix", "Mix", "Blend with the untouched image.", 1.0, 0.0, 1.0);
}

ImageEffect* ColorMixerFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new ColorMixerPlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static ColorMixerFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
