// Color Keeper - OFX plugin for DaVinci Resolve
// Keep one color the same across shots: pick it (a costume, a mask, a product, a logo),
// capture how it looks on the reference shot, and every shot with this node pulls that
// color back to it, whatever the light did. Only that hue range is touched.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.

#include <cmath>
#include <cstdio>

#include "FrameColor.h"
#include "StatLutEffect.h"

#define kPluginName "Color Keeper"
#define kPluginIdentifier "io.github.taro-kig.drs.ColorKeeper"
#define kPluginDescription                                                                      \
    "Keep one color the same across shots (a costume, a mask, a product...).\n"                 \
    "1. Pick the color and set Hue Range so Show Area covers it.\n"                             \
    "2. On the shot where it looks right, press Capture Reference.\n"                           \
    "3. Copy the node to the other shots: that color is measured and pulled back to the "       \
    "reference. Target = Picked Color pulls it to the picked swatch instead. Other hues are "   \
    "not touched.\n"                                                                            \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."

using namespace OFX;

namespace {

const char* const kRefNames[] = {"refValid", "refL", "refA", "refB"};

drs::V3 pickLab(double r, double g, double b)
{
    return drs::xyzToOklab(drs::mul(drs::kSrgbToXyz, {drs::srgbToLin(float(r)), drs::srgbToLin(float(g)), drs::srgbToLin(float(b))}));
}

float hueOf(drs::V3 lab)
{
    float h = std::atan2(lab.z, lab.y) / drs::kDeg;
    return h < 0 ? h + 360.0f : h;
}

class ColorKeeperPlugin : public drs::StatLutEffect {
public:
    explicit ColorKeeperPlugin(OfxImageEffectHandle h) : StatLutEffect(h, kPluginName)
    {
        m_Color = fetchRGBParam("color");
        m_Range = fetchDoubleParam("range");
        m_Target = fetchChoiceParam("target");
        m_Strength = fetchDoubleParam("strength");
        m_MatchL = fetchDoubleParam("matchL");
        m_Show = fetchBooleanParam("showArea");
        m_Status = fetchStringParam("status");
        for (int i = 0; i < 4; ++i) m_Ref[i] = fetchDoubleParam(kRefNames[i]);
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) override
    {
        if (args.reason != eChangeUserEdit || name != "capture") return;
        drs::FrameSamples h;
        if (!lastSamples(h) || h.lab.empty()) {
            sendMessage(Message::eMessageMessage, "", "Color Keeper: play or step the reference frame once, then press Capture again.");
            return;
        }
        double r, g, b;
        m_Color->getValue(r, g, b);
        float hue = hueOf(pickLab(r, g, b));
        drs::Region reg = drs::regionMean(h, hue, float(m_Range->getValue()));
        if (reg.share < 0.002f) {
            sendMessage(Message::eMessageMessage, "", "Color Keeper: that color is (almost) not in this frame. Check Show Area.");
            return;
        }
        beginEditBlock("Capture Reference");
        m_Ref[0]->setValue(1.0);
        m_Ref[1]->setValue(reg.lab.x);
        m_Ref[2]->setValue(reg.lab.y);
        m_Ref[3]->setValue(reg.lab.z);
        char buf[120];
        std::snprintf(buf, sizeof buf, "Captured: %.0f%% of the frame, hue %.0f deg", reg.share * 100.0f, hueOf(reg.lab));
        m_Status->setValue(buf);
        endEditBlock();
    }

protected:
    bool build(double t, const drs::FrameSamples& h, std::vector<double>& key, drs::PixelFn& fn) override
    {
        double r, g, b;
        m_Color->getValueAtTime(t, r, g, b);
        drs::V3 pick = pickLab(r, g, b);
        float hue = hueOf(pick), range = float(m_Range->getValueAtTime(t));
        bool show = m_Show->getValueAtTime(t);
        int target = 0;
        m_Target->getValueAtTime(t, target);
        bool captured = target == 0 && m_Ref[0]->getValueAtTime(t) > 0.5;
        drs::V3 tgt = captured ? drs::V3{float(m_Ref[1]->getValueAtTime(t)), float(m_Ref[2]->getValueAtTime(t)),
                                         float(m_Ref[3]->getValueAtTime(t))}
                               : pick;
        drs::Region cur = drs::regionMean(h, hue, range);
        auto q = [](float v) { return std::round(v * 2000.0f) / 2000.0f; };
        cur.lab = {q(cur.lab.x), q(cur.lab.y), q(cur.lab.z)};
        cur.share = std::round(cur.share * 1000.0f) / 1000.0f;
        float s = float(m_Strength->getValueAtTime(t)), mL = float(m_MatchL->getValueAtTime(t));
        key = {hue, range, s, mL, show ? 1.0 : 0.0, tgt.x, tgt.y, tgt.z, cur.lab.x, cur.lab.y, cur.lab.z, cur.share};
        drs::KeeperSettings k = drs::makeKeeper(cur, tgt, hue, range, s, mL);
        k.show = show;
        fn = [k](drs::V3 v) { return drs::keeperPixel(v, k); };
        return true;
    }

private:
    RGBParam* m_Color;
    DoubleParam* m_Range;
    ChoiceParam* m_Target;
    DoubleParam* m_Strength;
    DoubleParam* m_MatchL;
    BooleanParam* m_Show;
    StringParam* m_Status;
    DoubleParam* m_Ref[4];
};

} // namespace

mDeclarePluginFactory(ColorKeeperFactory, {}, {});

void ColorKeeperFactory::describe(ImageEffectDescriptor& desc) { drs::describeStatLut(desc, kPluginName, kPluginDescription); }

void ColorKeeperFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    drs::describeClips(desc);
    PageParamDescriptor* page = desc.definePageParam("Controls");

    RGBParamDescriptor* c = desc.defineRGBParam("color");
    c->setLabels("Color", "Color", "Color");
    c->setHint("The color to keep. Only its hue matters for what is selected; check Show Area.");
    c->setDefault(0.85, 0.55, 0.45);
    c->setAnimates(false);
    page->addChild(*c);
    drs::slider(desc, page, nullptr, "range", "Hue Range",
                "How far around the picked hue counts as this color, in degrees each way.", 30.0, 10.0, 90.0);
    BooleanParamDescriptor* show = desc.defineBooleanParam("showArea");
    show->setLabels("Show Area", "Show Area", "Show Area");
    show->setHint("Shows what is selected in color, the rest in grey. Turn it off before rendering.");
    show->setDefault(false);
    page->addChild(*show);

    PushButtonParamDescriptor* cap = desc.definePushButtonParam("capture");
    cap->setLabels("Capture Reference", "Capture Reference", "Capture Reference");
    cap->setHint("Stores how this color looks in the frame on screen (in this node).");
    page->addChild(*cap);
    StringParamDescriptor* st = desc.defineStringParam("status");
    st->setLabels("Reference", "Reference", "Reference");
    st->setStringType(eStringTypeLabel);
    st->setDefault("No reference yet (Picked Color is used)");
    st->setAnimates(false);
    st->setEvaluateOnChange(false);
    page->addChild(*st);

    ChoiceParamDescriptor* tg = desc.defineChoiceParam("target");
    tg->setLabels("Target", "Target", "Target");
    tg->setHint("Captured Reference: back to how it looked on the reference shot. Picked Color: toward the swatch.");
    tg->appendOption("Captured Reference");
    tg->appendOption("Picked Color");
    tg->setDefault(0);
    page->addChild(*tg);
    drs::slider(desc, page, nullptr, "strength", "Strength", "How far the color is pulled to the target.", 1.0, 0.0, 1.0);
    drs::slider(desc, page, nullptr, "matchL", "Match Brightness",
                "0 = only the hue and saturation follow, 1 = brightness too.", 0.5, 0.0, 1.0);
    for (const char* n : kRefNames) drs::hiddenValue(desc, page, n);
}

ImageEffect* ColorKeeperFactory::createInstance(OfxImageEffectHandle handle, ContextEnum) { return new ColorKeeperPlugin(handle); }

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static ColorKeeperFactory p(kPluginIdentifier, 1, 0);
    ids.push_back(&p);
}
