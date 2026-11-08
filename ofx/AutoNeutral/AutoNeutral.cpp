// Auto Neutral - OFX plugin for DaVinci Resolve
// White balance in one knob: reads the color of the light from near-neutral surfaces in
// the frame (walls, paper, grey stone; not sky, skin or wood) and takes it out. Warmth and
// Tint trim the result. Working space: DaVinci Wide Gamut / DaVinci Intermediate.

#include <cmath>

#include "FrameColor.h"
#include "StatLutEffect.h"

#define kPluginName "Auto Neutral"
#define kPluginIdentifier "io.github.taro-kig.drs.AutoNeutral"
#define kPluginDescription                                                                       \
    "White balance in one knob. Reads the color of the light from near-neutral surfaces in "     \
    "the frame (walls, paper, grey stone; colorful things like sky, skin and wood are ignored) " \
    "and takes it out, keeping exposure. Warmth / Tint trim the result. A frame with almost "    \
    "nothing neutral is left alone.\n"                                                           \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."

using namespace OFX;

namespace {

class AutoNeutralPlugin : public drs::StatLutEffect {
public:
    explicit AutoNeutralPlugin(OfxImageEffectHandle h) : StatLutEffect(h, kPluginName)
    {
        m_Strength = fetchDoubleParam("strength");
        m_Warmth = fetchDoubleParam("warmth");
        m_Tint = fetchDoubleParam("tint");
    }

protected:
    bool build(double t, const drs::FrameSamples& f, std::vector<double>& key, drs::PixelFn& fn) override
    {
        float strength = float(m_Strength->getValueAtTime(t));
        float warmth = float(m_Warmth->getValueAtTime(t)), tint = float(m_Tint->getValueAtTime(t));
        drs::Cast c = drs::castFromSamples(f);
        // rounded: frame-to-frame noise neither re-bakes nor flickers
        c.a = std::round(c.a * 2000.0f) / 2000.0f;
        c.b = std::round(c.b * 2000.0f) / 2000.0f;
        c.support = std::round(c.support * 200.0f) / 200.0f;
        key = {strength, warmth, tint, c.a, c.b, c.support};
        drs::NeutralSettings s = drs::makeNeutral(c, strength, warmth, tint);
        fn = [s](drs::V3 v) { return drs::neutralPixel(v, s); };
        return true;
    }

private:
    DoubleParam* m_Strength;
    DoubleParam* m_Warmth;
    DoubleParam* m_Tint;
};

} // namespace

mDeclarePluginFactory(AutoNeutralFactory, {}, {});

void AutoNeutralFactory::describe(ImageEffectDescriptor& desc) { drs::describeStatLut(desc, kPluginName, kPluginDescription); }

void AutoNeutralFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    drs::describeClips(desc);
    PageParamDescriptor* page = desc.definePageParam("Controls");
    drs::slider(desc, page, nullptr, "strength", "Strength",
                "How much of the measured color cast to take out. 1 = neutral.", 1.0, 0.0, 1.5);
    drs::slider(desc, page, nullptr, "warmth", "Warmth", "+ = warmer (amber), - = cooler (blue), on top of the result.",
                0.0, -1.0, 1.0);
    drs::slider(desc, page, nullptr, "tint", "Tint", "+ = magenta, - = green, on top of the result.", 0.0, -1.0, 1.0);
}

ImageEffect* AutoNeutralFactory::createInstance(OfxImageEffectHandle handle, ContextEnum)
{
    return new AutoNeutralPlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static AutoNeutralFactory p(kPluginIdentifier, 1, 0);
    ids.push_back(&p);
}
