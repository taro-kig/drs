// Color Anchors - OFX plugin for DaVinci Resolve
// Pick up to 6 "this color -> that color" pairs; nearby colors follow smoothly.
// Works on DaVinci Wide Gamut / DaVinci Intermediate, like the DCTLs in this repo.

#include <cstdio>
#include <memory>
#include <string>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ColorMath.h"

#define kPluginName "Color Anchors"
#define kPluginGrouping "drs"
#define kPluginDescription                                                                   \
    "Move picked colors to new ones; similar colors follow smoothly.\n"                      \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate.\n"                            \
    "Pick colors as you see them on screen. Match Lightness > 0 makes an anchor affect "     \
    "only colors of similar brightness, so shadows and highlights can go different ways."
#define kPluginIdentifier "io.github.taro-kig.drs.ColorAnchors"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

using namespace OFX;

namespace {

struct AnchorPreset {
    const char* label;
    double r, g, b;
};

// starting points only: from == to, so nothing changes until you move "To"
const AnchorPreset kPresets[drs::kMaxAnchors] = {
    {"Skin", 0.80, 0.58, 0.47},   {"Sky", 0.45, 0.63, 0.86}, {"Foliage", 0.40, 0.55, 0.25},
    {"Red", 0.78, 0.20, 0.18},    {"Yellow", 0.88, 0.76, 0.30}, {"Teal", 0.22, 0.58, 0.60},
};

std::string pid(const char* base, int i)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%d", base, i + 1);
    return buf;
}

class AnchorProcessor : public ImageProcessor {
public:
    explicit AnchorProcessor(ImageEffect& effect) : ImageProcessor(effect) {}

    void setSrcImg(Image* img) { m_Src = img; }
    void setSettings(const drs::Settings& s) { m_Settings = s; }

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* dst = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, dst += 4) {
                const float* src = static_cast<const float*>(m_Src ? m_Src->getPixelAddress(x, y) : nullptr);
                if (!src) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
                    continue;
                }
                drs::V3 o = drs::processPixel({src[0], src[1], src[2]}, m_Settings);
                dst[0] = o.x;
                dst[1] = o.y;
                dst[2] = o.z;
                dst[3] = src[3];
            }
        }
    }

private:
    Image* m_Src = nullptr;
    drs::Settings m_Settings;
};

class ColorAnchorsPlugin : public ImageEffect {
public:
    explicit ColorAnchorsPlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        for (int i = 0; i < drs::kMaxAnchors; ++i) {
            m_Enable[i] = fetchBooleanParam(pid("enable", i));
            m_From[i] = fetchRGBParam(pid("from", i));
            m_To[i] = fetchRGBParam(pid("to", i));
            m_Range[i] = fetchDoubleParam(pid("range", i));
            m_Strength[i] = fetchDoubleParam(pid("strength", i));
            m_MatchL[i] = fetchDoubleParam(pid("matchL", i));
        }
        m_Protect = fetchDoubleParam("protect");
        m_Mix = fetchDoubleParam("mix");
        m_ShowMask = fetchBooleanParam("showMask");
    }

    void render(const RenderArguments& args) override
    {
        if (m_Dst->getPixelDepth() != eBitDepthFloat || m_Dst->getPixelComponents() != ePixelComponentRGBA) {
            throwSuiteStatusException(kOfxStatErrUnsupported);
        }
        std::unique_ptr<Image> dst(m_Dst->fetchImage(args.time));
        std::unique_ptr<Image> src(m_Src->fetchImage(args.time));
        if (!dst) throwSuiteStatusException(kOfxStatFailed);
        if (src && (src->getPixelDepth() != dst->getPixelDepth() ||
                    src->getPixelComponents() != dst->getPixelComponents())) {
            throwSuiteStatusException(kOfxStatErrValue);
        }

        AnchorProcessor proc(*this);
        proc.setDstImg(dst.get());
        proc.setSrcImg(src.get());
        proc.setRenderWindow(args.renderWindow);
        proc.setSettings(readSettings(args.time));
        proc.process();
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime) override
    {
        if (m_ShowMask->getValueAtTime(args.time)) return false;
        bool any = false;
        for (int i = 0; i < drs::kMaxAnchors; ++i) any = any || m_Enable[i]->getValueAtTime(args.time);
        if (!any || m_Mix->getValueAtTime(args.time) <= 0.0) {
            identityClip = m_Src;
            identityTime = args.time;
            return true;
        }
        return false;
    }

private:
    drs::Settings readSettings(double t)
    {
        drs::Settings s;
        for (int i = 0; i < drs::kMaxAnchors; ++i) {
            double fr, fg, fb, tr, tg, tb;
            m_From[i]->getValueAtTime(t, fr, fg, fb);
            m_To[i]->getValueAtTime(t, tr, tg, tb);
            s.anchors[i] = drs::makeAnchor(m_Enable[i]->getValueAtTime(t),
                                           {float(fr), float(fg), float(fb)},
                                           {float(tr), float(tg), float(tb)},
                                           float(m_Range[i]->getValueAtTime(t)),
                                           float(m_Strength[i]->getValueAtTime(t)),
                                           float(m_MatchL[i]->getValueAtTime(t)));
        }
        s.protect = float(m_Protect->getValueAtTime(t));
        s.mix = float(m_Mix->getValueAtTime(t));
        s.showMask = m_ShowMask->getValueAtTime(t);
        return s;
    }

    Clip* m_Dst;
    Clip* m_Src;
    BooleanParam* m_Enable[drs::kMaxAnchors];
    RGBParam* m_From[drs::kMaxAnchors];
    RGBParam* m_To[drs::kMaxAnchors];
    DoubleParam* m_Range[drs::kMaxAnchors];
    DoubleParam* m_Strength[drs::kMaxAnchors];
    DoubleParam* m_MatchL[drs::kMaxAnchors];
    DoubleParam* m_Protect;
    DoubleParam* m_Mix;
    BooleanParam* m_ShowMask;
};

DoubleParamDescriptor* defineSlider(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* parent,
                                    const std::string& name, const char* label, const char* hint, double def,
                                    double lo, double hi, double step)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(lo, hi);
    p->setIncrement(step);
    p->setDoubleType(eDoubleTypePlain);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
}

} // namespace

mDeclarePluginFactory(ColorAnchorsFactory, {}, {});

void ColorAnchorsFactory::describe(ImageEffectDescriptor& desc)
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
}

void ColorAnchorsFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
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

    for (int i = 0; i < drs::kMaxAnchors; ++i) {
        const AnchorPreset& pr = kPresets[i];
        char label[48];
        std::snprintf(label, sizeof(label), "Anchor %d (%s)", i + 1, pr.label);

        GroupParamDescriptor* grp = desc.defineGroupParam(pid("anchor", i));
        grp->setLabels(label, label, label);
        grp->setOpen(i == 0);
        page->addChild(*grp);

        BooleanParamDescriptor* en = desc.defineBooleanParam(pid("enable", i));
        en->setLabels("Enable", "Enable", "Enable");
        en->setDefault(false);
        en->setParent(*grp);
        page->addChild(*en);

        RGBParamDescriptor* from = desc.defineRGBParam(pid("from", i));
        from->setLabels("From", "From", "From");
        from->setHint("The color to grab, as it looks on screen.");
        from->setDefault(pr.r, pr.g, pr.b);
        from->setParent(*grp);
        page->addChild(*from);

        RGBParamDescriptor* to = desc.defineRGBParam(pid("to", i));
        to->setLabels("To", "To", "To");
        to->setHint("Where that color should go. Hue, saturation and brightness all count.");
        to->setDefault(pr.r, pr.g, pr.b);
        to->setParent(*grp);
        page->addChild(*to);

        defineSlider(desc, page, grp, pid("range", i), "Range",
                     "How far from the From color the effect reaches (normalized Oklab chroma).", 0.06, 0.01,
                     0.4, 0.005);
        defineSlider(desc, page, grp, pid("strength", i), "Strength", "0 = off, 1 = all the way to To.", 1.0,
                     0.0, 1.5, 0.01);
        defineSlider(desc, page, grp, pid("matchL", i), "Match Lightness",
                     "0 = any brightness. Higher = only colors about as bright as From.", 0.0, 0.0, 4.0, 0.01);
    }

    GroupParamDescriptor* global = desc.defineGroupParam("global");
    global->setLabels("Global", "Global", "Global");
    global->setOpen(true);
    page->addChild(*global);

    defineSlider(desc, page, global, "protect", "Protect Neutrals",
                 "Keeps greys and near-greys untouched. 0 = off.", 0.03, 0.0, 0.2, 0.001);
    defineSlider(desc, page, global, "mix", "Mix", "Blend with the untouched image.", 1.0, 0.0, 1.0, 0.01);

    BooleanParamDescriptor* mask = desc.defineBooleanParam("showMask");
    mask->setLabels("Show Mask", "Show Mask", "Show Mask");
    mask->setHint("Show where the anchors act (white = fully).");
    mask->setDefault(false);
    mask->setParent(*global);
    page->addChild(*mask);
}

ImageEffect* ColorAnchorsFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new ColorAnchorsPlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static ColorAnchorsFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
