// Check View - OFX plugin for DaVinci Resolve
// A checking node: false color for exposure, skin tones, and colors outside Rec.709 / P3.
// Exact per pixel (no LUT), on the GPU (OpenCL) or all CPU cores. Turn it off to render.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.

#include <memory>
#include <vector>

#include "CheckMath.h"
#include "StatLutEffect.h" // describe helpers

#define kPluginName "Check View"
#define kPluginIdentifier "io.github.taro-kig.drs.CheckView"
#define kPluginDescription                                                                           \
    "A checking node, not a look: turn it off (or bypass it) before rendering.\n"                     \
    "False Color: exposure from 18% grey. Purple = crushed (< -6 stops), blue = deep shadow "        \
    "(-6..-4), green = middle grey (+/-0.5), pink = bright skin (+1..+1.8), yellow = highlights "    \
    "(+3..+4.5), orange = near clip (+4.5..+6), red = over +6. Everything else in grey.\n"           \
    "Skin Tones: only skin-colored pixels in color.\n"                                               \
    "Gamut: colors a Rec.709 / P3 display cannot show, in magenta.\n"                                \
    "Place it after your grade, before the output transform. Working space: DaVinci Wide Gamut / "   \
    "DaVinci Intermediate."

using namespace OFX;

namespace {

class CheckProcessor : public ImageProcessor {
public:
    explicit CheckProcessor(ImageEffect& e) : ImageProcessor(e) {}
    Image* src = nullptr;
    drs::CheckSettings s;

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* d = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, d += 4) {
                const float* p = static_cast<const float*>(src ? src->getPixelAddress(x, y) : nullptr);
                if (!p) {
                    d[0] = d[1] = d[2] = d[3] = 0.0f;
                    continue;
                }
                drs::V3 o = drs::checkPixel({p[0], p[1], p[2]}, s);
                d[0] = o.x, d[1] = o.y, d[2] = o.z, d[3] = p[3];
            }
        }
    }
};

class CheckViewPlugin : public ImageEffect {
public:
    explicit CheckViewPlugin(OfxImageEffectHandle h) : ImageEffect(h)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        m_Mode = fetchChoiceParam("mode");
    }

    void render(const RenderArguments& args) override
    {
        if (m_Dst->getPixelDepth() != eBitDepthFloat || m_Dst->getPixelComponents() != ePixelComponentRGBA)
            throwSuiteStatusException(kOfxStatErrUnsupported);
        std::unique_ptr<Image> dst(m_Dst->fetchImage(args.time));
        std::unique_ptr<Image> src(m_Src->fetchImage(args.time));
        if (!dst || !src) throwSuiteStatusException(kOfxStatFailed);
        int mode = 0;
        m_Mode->getValueAtTime(args.time, mode);
        drs::CheckSettings s = drs::makeCheck(mode);

        if (args.isEnabledOpenCLRender) {
            std::vector<float> tab(s.zone, s.zone + drs::kZones * 3);
            tab.insert(tab.end(), drs::kZoneLo, drs::kZoneLo + drs::kZones);
            tab.insert(tab.end(), drs::kZoneHi, drs::kZoneHi + drs::kZones);
            tab.insert(tab.end(), s.gamut, s.gamut + 9);
            const OfxRectI b = dst->getBounds();
            const char* err = nullptr;
            if (!drs::clCheckView(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), b.x2 - b.x1, b.y2 - b.y1,
                                  mode, tab, &err)) {
                sendMessage(Message::eMessageError, "", std::string("Check View GPU: ") + (err ? err : "?"));
                throwSuiteStatusException(kOfxStatFailed);
            }
            return;
        }
        CheckProcessor p(*this);
        p.setDstImg(dst.get());
        p.setRenderWindow(args.renderWindow);
        p.src = src.get();
        p.s = s;
        p.process();
    }

private:
    Clip* m_Dst;
    Clip* m_Src;
    ChoiceParam* m_Mode;
};

} // namespace

mDeclarePluginFactory(CheckViewFactory, {}, {});

void CheckViewFactory::describe(ImageEffectDescriptor& desc) { drs::describeStatLut(desc, kPluginName, kPluginDescription); }

void CheckViewFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    drs::describeClips(desc);
    PageParamDescriptor* page = desc.definePageParam("Controls");
    ChoiceParamDescriptor* m = desc.defineChoiceParam("mode");
    m->setLabels("View", "View", "View");
    m->setHint("False Color: exposure zones. Skin Tones: skin in color, the rest grey. Gamut: what a "
               "Rec.709 / P3 display cannot show, in magenta.");
    m->appendOption("False Color");
    m->appendOption("Skin Tones");
    m->appendOption("Gamut: Rec.709");
    m->appendOption("Gamut: P3");
    m->setDefault(0);
    page->addChild(*m);
}

ImageEffect* CheckViewFactory::createInstance(OfxImageEffectHandle handle, ContextEnum) { return new CheckViewPlugin(handle); }

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static CheckViewFactory p(kPluginIdentifier, 1, 0);
    ids.push_back(&p);
}
