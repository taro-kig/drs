// Color Look - OFX plugin for DaVinci Resolve
// Seven sliders, one per hue band: raise a color and it becomes the hero of the look. The
// plugin reads the frame and decides the rest (where the hero tint goes, the complementary
// color, how far to lean) without repainting wood, skin or leaves (HeroMath.h).
// The original pick-a-color slots are still there under Manual. Auto Tone sets exposure
// and contrast from the frame. Working space: DaVinci Wide Gamut / DaVinci Intermediate.
// Rendering: cached 65^3 LUT, on the GPU (OpenCL) when offered, otherwise CPU.

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ClLut.h"
#include "ColorLookMath.h"
#include "Lut3D.h"

#define kPluginName "Color Look"
#define kPluginGrouping "drs"
#define kPluginDescription                                                                      \
    "Raise a color and it becomes the hero of the look.\n"                                      \
    "Orange -> teal and orange, Aqua -> a cool cyan look, Green -> a green-led film look. "     \
    "The plugin reads the frame and decides the rest: where the hero tint goes, the "           \
    "complementary color, how far the picture leans. Colors already in the picture (skin, "     \
    "wood, leaves) keep their own hue. Manual: pick exact colors per band instead. "            \
    "Auto Tone sets exposure and contrast from the frame.\n"                                    \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."
#define kPluginIdentifier "io.github.taro-kig.drs.ColorLook"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 1

using namespace OFX;

namespace {

const char* const kChips[drs::kMixColors] = {"\xF0\x9F\x9F\xA5", "\xF0\x9F\x9F\xA7", "\xF0\x9F\x9F\xA8",
                                             "\xF0\x9F\x9F\xA9", "\xE2\xAC\x9C",     "\xF0\x9F\x9F\xA6",
                                             "\xF0\x9F\x9F\xAA"};
constexpr int kStatStep = 4; // measure every 4th pixel in x and y

std::string pid(const char* base, int i)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%d", base, i);
    return buf;
}

class LookProcessor : public ImageProcessor {
public:
    explicit LookProcessor(ImageEffect& effect) : ImageProcessor(effect) {}

    Image* src = nullptr;
    drs::ColorLookSettings settings;
    std::shared_ptr<const drs::Lut3D> lut;

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* dst = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, dst += 4) {
                const float* s = static_cast<const float*>(src->getPixelAddress(x, y));
                if (!s) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
                    continue;
                }
                drs::V3 in = {s[0], s[1], s[2]};
                drs::V3 o = lut->inDomain(in) ? lut->apply(in) : drs::colorLookPixel(in, settings);
                dst[0] = o.x;
                dst[1] = o.y;
                dst[2] = o.z;
                dst[3] = s[3];
            }
        }
    }
};

class ColorLookPlugin : public ImageEffect {
public:
    explicit ColorLookPlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        for (int i = 0; i < drs::kMixColors; ++i) {
            m_Hero[i] = fetchDoubleParam(pid("hero", i));
            m_On[i] = fetchBooleanParam(pid("on", i));
            m_Color[i] = fetchRGBParam(pid("color", i));
        }
        m_Amount = fetchDoubleParam("amount");
        m_Auto = fetchDoubleParam("autoTone");
        m_Bright = fetchDoubleParam("brightness");
        m_Contrast = fetchDoubleParam("contrast");
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) override
    {
        // Auto Tone and the hero colors measure the whole frame
        rois.setRegionOfInterest(*m_Src, m_Src->getRegionOfDefinition(args.time));
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
        const double t = args.time;
        const OfxRectI b = src->getBounds();
        const int W = b.x2 - b.x1, H = b.y2 - b.y1;
        const bool gpu = args.isEnabledOpenCLRender;
        const char* err = nullptr;

        // 1. measure the frame (only needed with Auto Tone)
        float autoStrength = float(m_Auto->getValueAtTime(t));
        drs::ToneStats stats;
        if (autoStrength > 0.0f) {
            std::vector<uint32_t> hist(drs::kHistBins, 0u);
            if (gpu) {
                if (!drs::clLumaHistogram(args.pOpenCLCmdQ, src->getPixelData(), W, H, kStatStep, drs::kHistBins,
                                          drs::kHistLo, drs::kHistHi, hist, &err)) {
                    fail(err);
                }
            } else {
                for (int y = b.y1; y < b.y2; y += kStatStep)
                    for (int x = b.x1; x < b.x2; x += kStatStep) {
                        const float* s = static_cast<const float*>(src->getPixelAddress(x, y));
                        if (s) ++hist[drs::histBin(drs::lumaStops({s[0], s[1], s[2]}))];
                    }
            }
            stats = drs::statsFromHistogram(hist);
        }

        // 1b. colors in the frame (only needed for hero colors)
        float hero[drs::kMixColors];
        bool anyHero = false;
        for (int i = 0; i < drs::kMixColors; ++i) {
            hero[i] = float(m_Hero[i]->getValueAtTime(t));
            anyHero = anyHero || hero[i] > 0.0f;
        }
        drs::ColorStats cstats;
        if (anyHero) {
            std::vector<uint32_t> chist(drs::kColorBins, 0u);
            if (gpu) {
                if (!drs::clColorHistogram(args.pOpenCLCmdQ, src->getPixelData(), W, H, kStatStep, drs::kColorBins,
                                           chist, &err)) {
                    fail(err);
                }
            } else {
                for (int y = b.y1; y < b.y2; y += kStatStep)
                    for (int x = b.x1; x < b.x2; x += kStatStep) {
                        const float* s = static_cast<const float*>(src->getPixelAddress(x, y));
                        if (!s) continue;
                        int bin = drs::colorBin({s[0], s[1], s[2]});
                        if (bin >= 0) ++chist[size_t(bin)];
                    }
            }
            cstats = drs::statsFromColorHist(chist);
            // rounded like the tone stats: frame-to-frame noise neither re-bakes nor flickers
            for (float& c : cstats.coverage) c = std::round(c * 100.0f) / 100.0f;
            cstats.neutral = std::round(cstats.neutral * 100.0f) / 100.0f;
        }

        // 2. settings -> cached LUT
        bool on[drs::kMixColors];
        drs::V3 pick[drs::kMixColors];
        std::vector<double> key;
        for (int i = 0; i < drs::kMixColors; ++i) {
            on[i] = m_On[i]->getValueAtTime(t);
            double r, g, bl;
            m_Color[i]->getValueAtTime(t, r, g, bl);
            pick[i] = {float(r), float(g), float(bl)};
            key.insert(key.end(), {on[i] ? 1.0 : 0.0, r, g, bl});
        }
        float amount = float(m_Amount->getValueAtTime(t));
        float bright = float(m_Bright->getValueAtTime(t));
        float contrast = float(m_Contrast->getValueAtTime(t));
        // stats rounded so tiny frame-to-frame noise does not force a re-bake
        auto q = [](float v) { return std::round(v * 20.0f) / 20.0f; };
        stats.median = q(stats.median);
        stats.p05 = q(stats.p05);
        stats.p95 = q(stats.p95);
        key.insert(key.end(), {amount, autoStrength, bright, contrast, stats.median, stats.p05, stats.p95});
        for (int i = 0; i < drs::kMixColors; ++i) key.insert(key.end(), {hero[i], cstats.coverage[i]});
        key.insert(key.end(), {cstats.neutral, cstats.valid ? 1.0 : 0.0});

        drs::ColorLookSettings s =
            drs::makeColorLook(hero, cstats, on, pick, amount, contrast, bright, autoStrength, stats);
        auto lut = m_Cache.get(key, 65, [s](drs::V3 c) { return drs::colorLookPixel(c, s); });

        // 3. apply
        if (gpu) {
            if (!drs::clApplyLuts(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), W, H,
                                  {lut->data().data()}, lut->size(), drs::Lut3D::kLo, lut->scale(), drs::ClGrid{},
                                  &err)) {
                fail(err);
            }
            return;
        }
        LookProcessor proc(*this);
        proc.setDstImg(dst.get());
        proc.setRenderWindow(args.renderWindow);
        proc.src = src.get();
        proc.settings = s;
        proc.lut = lut;
        proc.process();
    }

private:
    void fail(const char* err)
    {
        sendMessage(Message::eMessageError, "", std::string("Color Look GPU: ") + (err ? err : "?"));
        throwSuiteStatusException(kOfxStatFailed);
    }

    drs::LutCache m_Cache;
    Clip* m_Dst;
    Clip* m_Src;
    DoubleParam* m_Hero[drs::kMixColors];
    BooleanParam* m_On[drs::kMixColors];
    RGBParam* m_Color[drs::kMixColors];
    DoubleParam* m_Amount;
    DoubleParam* m_Auto;
    DoubleParam* m_Bright;
    DoubleParam* m_Contrast;
};

void defineSlider(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* parent,
                  const char* name, const char* label, const char* hint, double def, double lo, double hi)
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

mDeclarePluginFactory(ColorLookFactory, {}, {});

void ColorLookFactory::describe(ImageEffectDescriptor& desc)
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

void ColorLookFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
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

    // hero colors: the everyday controls
    GroupParamDescriptor* colors = desc.defineGroupParam("colors");
    colors->setLabels("Colors", "Colors", "Colors");
    colors->setOpen(true);
    page->addChild(*colors);
    for (int i = 0; i < drs::kMixColors; ++i) {
        std::string name = std::string(drs::kMixNames[i]) + " " + kChips[i];
        DoubleParamDescriptor* h = desc.defineDoubleParam(pid("hero", i));
        h->setLabels(name, name, name);
        h->setHint("How much this color leads the look. The plugin decides the rest from the frame: "
                   "where the tint goes, the complementary color, how far to lean.");
        h->setDefault(0.0);
        h->setRange(0.0, 1.0);
        h->setDisplayRange(0.0, 1.0);
        h->setIncrement(0.01);
        h->setDoubleType(eDoubleTypePlain);
        h->setParent(*colors);
        page->addChild(*h);
    }

    GroupParamDescriptor* tone = desc.defineGroupParam("tone");
    tone->setLabels("Tone", "Tone", "Tone");
    tone->setOpen(true);
    page->addChild(*tone);
    defineSlider(desc, page, tone, "amount", "Amount", "Strength of the whole look. 0 = only Auto Tone.", 1.0, 0.0, 1.5);
    defineSlider(desc, page, tone, "autoTone", "Auto Tone",
                 "Sets exposure and contrast from the frame to suit the colors. 0 = off.", 1.0, 0.0, 1.0);
    defineSlider(desc, page, tone, "brightness", "Brightness", "Brighter or darker, in stops.", 0.0, -2.0, 2.0);
    defineSlider(desc, page, tone, "contrast", "Contrast", "More (+) or less (-) contrast.", 0.0, -1.0, 1.0);

    // manual: the original pick-a-color slots (same parameter names as Color Look 1.0)
    GroupParamDescriptor* manual = desc.defineGroupParam("manual");
    manual->setLabels("Manual Colors", "Manual Colors", "Manual Colors");
    manual->setOpen(false);
    page->addChild(*manual);
    for (int i = 0; i < drs::kMixColors; ++i) {
        std::string name = std::string(drs::kMixNames[i]) + " " + kChips[i];
        BooleanParamDescriptor* on = desc.defineBooleanParam(pid("on", i));
        on->setLabels(name, name, name);
        on->setHint("Use this color: the band moves to it, and it joins the palette that sets the look.");
        on->setDefault(false);
        on->setParent(*manual);
        page->addChild(*on);

        drs::V3 ref = drs::bandReferenceSrgb(i);
        std::string clabel = std::string(drs::kMixNames[i]) + " color";
        RGBParamDescriptor* c = desc.defineRGBParam(pid("color", i));
        c->setLabels(clabel, clabel, clabel);
        c->setHint("The color you want this part of the picture to be, as it looks on screen.");
        c->setDefault(ref.x, ref.y, ref.z);
        c->setParent(*manual);
        page->addChild(*c);
    }
}

ImageEffect* ColorLookFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new ColorLookPlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static ColorLookFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
