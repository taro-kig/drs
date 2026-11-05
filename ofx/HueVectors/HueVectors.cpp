// Hue Vectors - OFX plugin for DaVinci Resolve
// The HueVec_Density / HueVec_Sat / HueVec_Hue DCTLs in one node, with a master Richness
// knob on top and the per-hue controls folded away in groups.
// Works on DaVinci Wide Gamut / DaVinci Intermediate, like the DCTLs in this repo.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ClLut.h"
#include "HueVecMath.h"
#include "Lut3D.h"

#define kPluginName "Hue Vectors"
#define kPluginGrouping "drs"
#define kPluginDescription                                                                     \
    "Richer, film-like color in one knob. Richness adds density (saturated colors get deeper " \
    "and darker, neutrals untouched) and in-gamut saturation together; Balance tilts it "      \
    "toward saturation (0) or density (1). Per-hue Density, Saturation and Hue shift are in "  \
    "the folded groups below and run in that order.\n"                                         \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."
#define kPluginIdentifier "io.github.taro-kig.drs.HueVectors"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

using namespace OFX;

namespace {

namespace hv = drs::huevec;

// same vectors and color chips as the HueVec DCTLs (Yellow uses the orange chip there too)
const char* const kHueNames[6] = {"Red", "Yellow", "Green", "Cyan", "Blue", "Magenta"};
const char* const kHueIds[6] = {"R", "Y", "G", "C", "B", "M"};
const char* const kChips[6] = {"\xF0\x9F\x9F\xA5", "\xF0\x9F\x9F\xA7", "\xF0\x9F\x9F\xA9",
                               "\xE2\xAC\x9C",     "\xF0\x9F\x9F\xA6", "\xF0\x9F\x9F\xAA"};

std::string pid(const char* base, int i) { return std::string(base) + kHueIds[i]; }

class HueVecProcessor : public ImageProcessor {
public:
    explicit HueVecProcessor(ImageEffect& effect) : ImageProcessor(effect) {}

    void setSrcImg(Image* img) { m_Src = img; }
    void setSettings(const hv::Settings& s) { m_Settings = s; }
    void setLut(std::shared_ptr<const drs::Lut3D> lut) { m_Lut = std::move(lut); }

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
                drs::V3 in = {src[0], src[1], src[2]};
                drs::V3 o = m_Lut->inDomain(in) ? m_Lut->apply(in) : hv::processPixel(in, m_Settings);
                dst[0] = o.x;
                dst[1] = o.y;
                dst[2] = o.z;
                dst[3] = src[3];
            }
        }
    }

private:
    Image* m_Src = nullptr;
    hv::Settings m_Settings;
    std::shared_ptr<const drs::Lut3D> m_Lut;
};

void pushVectors(std::vector<double>& k, const hv::Vectors& v)
{
    k.insert(k.end(), std::begin(v.v), std::end(v.v));
    k.push_back(v.glob);
    k.push_back(v.range);
}

// every value that changes the result, for the LUT cache
std::vector<double> settingsKey(const hv::Settings& s)
{
    std::vector<double> k = {s.richness, s.balance, s.deep, s.mix, s.showDeep ? 1.0 : 0.0};
    pushVectors(k, s.density);
    pushVectors(k, s.sat);
    pushVectors(k, s.hue);
    return k;
}

class HueVectorsPlugin : public ImageEffect {
public:
    explicit HueVectorsPlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        m_Richness = fetchDoubleParam("richness");
        m_Balance = fetchDoubleParam("balance");
        m_Deep = fetchDoubleParam("deep");
        m_Mix = fetchDoubleParam("mix");
        m_ShowDeep = fetchBooleanParam("showDeep");
        for (int i = 0; i < 6; ++i) {
            m_Den[i] = fetchDoubleParam(pid("den", i));
            m_Sat[i] = fetchDoubleParam(pid("sat", i));
            m_Hue[i] = fetchDoubleParam(pid("hue", i));
        }
        m_SatRange = fetchDoubleParam("satRange");
        m_HueRange = fetchDoubleParam("hueRange");
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

        hv::Settings s = readSettings(args.time);
        auto lut = m_Cache.get(settingsKey(s), 65, [&s](drs::V3 c) { return hv::processPixel(c, s); });

        if (args.isEnabledOpenCLRender) {
            // GPU: the images are cl_mem buffers on the host's queue
            const OfxRectI b = dst->getBounds();
            const char* err = nullptr;
            if (!drs::clApplyLuts(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), b.x2 - b.x1,
                                  b.y2 - b.y1, {lut->data().data()}, lut->size(), drs::Lut3D::kLo, lut->scale(),
                                  drs::ClGrid{}, &err)) {
                sendMessage(Message::eMessageError, "", std::string("Hue Vectors GPU: ") + (err ? err : "?"));
                throwSuiteStatusException(kOfxStatFailed);
            }
            return;
        }

        HueVecProcessor proc(*this);
        proc.setDstImg(dst.get());
        proc.setSrcImg(src.get());
        proc.setRenderWindow(args.renderWindow);
        proc.setSettings(s);
        proc.setLut(lut);
        proc.process();
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime) override
    {
        if (hv::isIdentity(readSettings(args.time))) {
            identityClip = m_Src;
            identityTime = args.time;
            return true;
        }
        return false;
    }

private:
    hv::Settings readSettings(double t)
    {
        hv::Settings s;
        s.richness = float(m_Richness->getValueAtTime(t));
        s.balance = float(m_Balance->getValueAtTime(t));
        s.deep = float(m_Deep->getValueAtTime(t));
        s.mix = float(m_Mix->getValueAtTime(t));
        s.showDeep = m_ShowDeep->getValueAtTime(t);
        for (int i = 0; i < 6; ++i) {
            s.density.v[i] = float(m_Den[i]->getValueAtTime(t));
            s.sat.v[i] = float(m_Sat[i]->getValueAtTime(t));
            s.hue.v[i] = float(m_Hue[i]->getValueAtTime(t));
        }
        s.sat.range = float(m_SatRange->getValueAtTime(t));
        s.hue.range = float(m_HueRange->getValueAtTime(t));
        return s;
    }

    drs::LutCache m_Cache;
    Clip* m_Dst;
    Clip* m_Src;
    DoubleParam* m_Richness;
    DoubleParam* m_Balance;
    DoubleParam* m_Deep;
    DoubleParam* m_Mix;
    BooleanParam* m_ShowDeep;
    DoubleParam* m_Den[6];
    DoubleParam* m_Sat[6];
    DoubleParam* m_Hue[6];
    DoubleParam* m_SatRange;
    DoubleParam* m_HueRange;
};

DoubleParamDescriptor* defineSlider(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* parent,
                                    const std::string& name, const std::string& label, const char* hint, double def,
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

GroupParamDescriptor* defineGroup(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name,
                                  const char* label, bool open)
{
    GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    page->addChild(*g);
    return g;
}

// six -1..1 sliders, one per hue vector
void defineHueSliders(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* grp,
                      const char* base, const char* what, const char* hint)
{
    for (int i = 0; i < 6; ++i) {
        std::string label = std::string(kHueNames[i]) + " " + what + " " + kChips[i];
        defineSlider(desc, page, grp, pid(base, i), label, hint, 0.0, -1.0, 1.0, 0.001);
    }
}

} // namespace

mDeclarePluginFactory(HueVectorsFactory, {}, {});

void HueVectorsFactory::describe(ImageEffectDescriptor& desc)
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

void HueVectorsFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
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

    // the everyday controls, always open
    GroupParamDescriptor* main = defineGroup(desc, page, "main", "Main", true);
    defineSlider(desc, page, main, "richness", "Richness",
                 "+ = deeper, richer color (density and saturation together), - = softer. "
                 "Neutrals are never touched.",
                 0.0, -1.0, 1.0, 0.001);
    defineSlider(desc, page, main, "balance", "Balance",
                 "What Richness adds: 0 = saturation only (brighter, video-like), 0.5 = both, "
                 "1 = density only (darker, print-film-like).",
                 0.5, 0.0, 1.0, 0.01);
    defineSlider(desc, page, main, "deep", "Deep",
                 "Keeps the effect out of the highlights. Higher = more of the bright range left clean.", 0.0,
                 0.0, 1.0, 0.001);
    defineSlider(desc, page, main, "mix", "Mix", "Blend with the untouched image.", 1.0, 0.0, 1.0, 0.01);
    BooleanParamDescriptor* show = desc.defineBooleanParam("showDeep");
    show->setLabels("Show Deep", "Show Deep", "Show Deep");
    show->setHint("Shows the highlights Deep leaves out in magenta. Turn it off before rendering.");
    show->setDefault(false);
    show->setParent(*main);
    page->addChild(*show);

    // per-hue fine tuning, folded away; runs Density -> Saturation -> Hue
    GroupParamDescriptor* den = defineGroup(desc, page, "densityGrp", "Density per Hue", false);
    defineHueSliders(desc, page, den, "den", "Den",
                     "+ = deeper and darker (up to about 1.5 stops on saturated colors), - = lighter.");

    GroupParamDescriptor* sat = defineGroup(desc, page, "satGrp", "Saturation per Hue", false);
    defineHueSliders(desc, page, sat, "sat", "Sat", "+ = more saturated (stays in gamut), - = less. Luminance kept.");
    defineSlider(desc, page, sat, "satRange", "Sat Hue Range",
                 "1 = seamless between neighbors. Lower isolates a color (a yellow taxi without the skin next "
                 "to it).",
                 1.0, 0.4, 2.0, 0.01);

    GroupParamDescriptor* hue = defineGroup(desc, page, "hueGrp", "Hue Shift", false);
    defineHueSliders(desc, page, hue, "hue", "Hue",
                     "+ = toward the next color (R > Y > G > C > B > M > R), - = toward the previous. "
                     "Full scale = 30 degrees.");
    defineSlider(desc, page, hue, "hueRange", "Hue Range", "1 = seamless between neighbors. Lower isolates a color.",
                 1.0, 0.4, 2.0, 0.01);
}

ImageEffect* HueVectorsFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new HueVectorsPlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static HueVectorsFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
