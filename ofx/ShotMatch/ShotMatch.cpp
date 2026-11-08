// Shot Match - OFX plugin for DaVinci Resolve
// Make shots look like a reference shot: on the reference, press Capture (its exposure,
// contrast, light color and colorfulness are stored in the node), then copy the node to
// the other shots. Each shot is measured and pulled toward the reference.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.

#include <cmath>
#include <cstdio>

#include "FrameColor.h"
#include "StatLutEffect.h"

#define kPluginName "Shot Match"
#define kPluginIdentifier "io.github.taro-kig.drs.ShotMatch"
#define kPluginDescription                                                                       \
    "Make shots look like a reference shot.\n"                                                   \
    "1. On the reference shot, press Capture Reference (stored in this node).\n"                 \
    "2. Copy the node to the other shots (or use it as a shared node).\n"                        \
    "Each shot is measured and pulled toward the reference: exposure and contrast (by its "      \
    "shadows, middle and highlights), the color of the light (read from near-neutral "           \
    "surfaces, like a white balance) and how colorful it is. Hues of objects are not touched.\n" \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."

using namespace OFX;

namespace {

const char* const kRefNames[] = {"refValid", "refP05", "refMedian", "refP95", "refCastA", "refCastB", "refChroma"};
constexpr int kRefCount = 7;

class ShotMatchPlugin : public drs::StatLutEffect {
public:
    explicit ShotMatchPlugin(OfxImageEffectHandle h) : StatLutEffect(h, kPluginName)
    {
        m_Exposure = fetchDoubleParam("exposure");
        m_Color = fetchDoubleParam("color");
        m_Sat = fetchDoubleParam("saturation");
        m_Status = fetchStringParam("status");
        for (int i = 0; i < kRefCount; ++i) m_Ref[i] = fetchDoubleParam(kRefNames[i]);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) override
    {
        if (args.reason != eChangeUserEdit) return;
        if (name == "capture") {
            drs::FrameSamples h;
            if (!lastSamples(h) || h.lab.empty()) {
                sendMessage(Message::eMessageMessage, "", "Shot Match: play or step the reference frame once, then press Capture again.");
                return;
            }
            drs::ShotStats s = drs::shotStats(h);
            const double v[kRefCount] = {1.0, s.tone.p05, s.tone.median, s.tone.p95, s.cast.a, s.cast.b, s.chroma};
            beginEditBlock("Capture Reference");
            for (int i = 0; i < kRefCount; ++i) m_Ref[i]->setValue(v[i]);
            m_Status->setValue(describe(s));
            endEditBlock();
        } else if (name == "clear") {
            beginEditBlock("Clear Reference");
            m_Ref[0]->setValue(0.0);
            m_Status->setValue("No reference yet");
            endEditBlock();
        }
    }

protected:
    bool build(double t, const drs::FrameSamples& h, std::vector<double>& key, drs::PixelFn& fn) override
    {
        drs::ShotStats ref;
        if (m_Ref[0]->getValueAtTime(t) < 0.5) return false;
        ref.tone.p05 = float(m_Ref[1]->getValueAtTime(t));
        ref.tone.median = float(m_Ref[2]->getValueAtTime(t));
        ref.tone.p95 = float(m_Ref[3]->getValueAtTime(t));
        ref.cast.a = float(m_Ref[4]->getValueAtTime(t));
        ref.cast.b = float(m_Ref[5]->getValueAtTime(t));
        ref.chroma = float(m_Ref[6]->getValueAtTime(t));
        ref.valid = true;

        drs::ShotStats cur = drs::shotStats(h);
        // rounded: frame-to-frame noise neither re-bakes nor flickers
        auto q = [](float v, float k) { return std::round(v * k) / k; };
        cur.tone.p05 = q(cur.tone.p05, 20), cur.tone.median = q(cur.tone.median, 20), cur.tone.p95 = q(cur.tone.p95, 20);
        cur.cast.a = q(cur.cast.a, 2000), cur.cast.b = q(cur.cast.b, 2000), cur.chroma = q(cur.chroma, 500);

        float e = float(m_Exposure->getValueAtTime(t)), c = float(m_Color->getValueAtTime(t)),
              s = float(m_Sat->getValueAtTime(t));
        key = {e, c, s, ref.tone.p05, ref.tone.median, ref.tone.p95, ref.cast.a, ref.cast.b, ref.chroma,
               cur.tone.p05, cur.tone.median, cur.tone.p95, cur.cast.a, cur.cast.b, cur.chroma};
        drs::MatchSettings m = drs::makeMatch(cur, ref, e, c, s, &h);
        m.chromaScale = q(m.chromaScale, 500);
        key.push_back(m.chromaScale);
        fn = [m](drs::V3 v) { return drs::matchPixel(v, m); };
        return true;
    }

private:
    static std::string describe(const drs::ShotStats& s)
    {
        float warm = s.cast.b, C = std::sqrt(s.cast.a * s.cast.a + s.cast.b * s.cast.b);
        const char* light = C < 0.004f ? "neutral" : warm > 0 ? "warm" : "cool";
        char buf[160];
        std::snprintf(buf, sizeof buf, "Reference: middle %+.1f stops, range %.1f stops, %s light", s.tone.median,
                      s.tone.p95 - s.tone.p05, light);
        return buf;
    }

    DoubleParam* m_Exposure;
    DoubleParam* m_Color;
    DoubleParam* m_Sat;
    StringParam* m_Status;
    DoubleParam* m_Ref[kRefCount];
};

} // namespace

mDeclarePluginFactory(ShotMatchFactory, {}, {});

void ShotMatchFactory::describe(ImageEffectDescriptor& desc) { drs::describeStatLut(desc, kPluginName, kPluginDescription); }

void ShotMatchFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    drs::describeClips(desc);
    PageParamDescriptor* page = desc.definePageParam("Controls");

    PushButtonParamDescriptor* cap = desc.definePushButtonParam("capture");
    cap->setLabels("Capture Reference", "Capture Reference", "Capture Reference");
    cap->setHint("Stores the frame on screen as the reference (in this node). Then copy the node to other shots.");
    page->addChild(*cap);
    StringParamDescriptor* st = desc.defineStringParam("status");
    st->setLabels("Reference", "Reference", "Reference");
    st->setStringType(eStringTypeLabel);
    st->setDefault("No reference yet");
    st->setAnimates(false);
    st->setEvaluateOnChange(false);
    page->addChild(*st);

    drs::slider(desc, page, nullptr, "exposure", "Exposure / Contrast",
                "How far exposure and contrast follow the reference (by shadows, middle and highlights).", 1.0, 0.0, 1.0);
    drs::slider(desc, page, nullptr, "color", "Light Color",
                "How far the color of the light (white balance) follows the reference.", 1.0, 0.0, 1.0);
    drs::slider(desc, page, nullptr, "saturation", "Saturation",
                "How far overall colorfulness follows the reference (limited to 0.7x .. 1.4x).", 1.0, 0.0, 1.0);

    PushButtonParamDescriptor* clr = desc.definePushButtonParam("clear");
    clr->setLabels("Clear Reference", "Clear Reference", "Clear Reference");
    page->addChild(*clr);
    for (const char* n : kRefNames) drs::hiddenValue(desc, page, n);
}

ImageEffect* ShotMatchFactory::createInstance(OfxImageEffectHandle handle, ContextEnum) { return new ShotMatchPlugin(handle); }

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static ShotMatchFactory p(kPluginIdentifier, 1, 0);
    ids.push_back(&p);
}
