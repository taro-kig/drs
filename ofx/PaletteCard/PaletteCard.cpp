// Palette Card - OFX plugin for DaVinci Resolve
// The dominant colors of the frame as swatches: a strip at the bottom or the right, or a
// full card. Handy as a look reference, or to screenshot a palette.
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.

#include <memory>
#include <vector>

#include "FrameColor.h"
#include "PaletteLayout.h"
#include "StatLutEffect.h" // describe helpers, Measure.h

#define kPluginName "Palette Card"
#define kPluginIdentifier "io.github.taro-kig.drs.PaletteCard"
#define kPluginDescription                                                                     \
    "The dominant colors of the frame as swatches: a strip along the bottom or the right, or " \
    "a full card. Measured from the picture coming into this node (put it after the grade to " \
    "see the graded palette). Proportional widths show how much of the frame each color "      \
    "takes.\n"                                                                                  \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."

using namespace OFX;

namespace {

class PaletteProcessor : public ImageProcessor {
public:
    explicit PaletteProcessor(ImageEffect& e) : ImageProcessor(e) {}
    Image* src = nullptr;
    const drs::PaletteDraw* draw = nullptr;

    void multiThreadProcessImages(OfxRectI win) override
    {
        const OfxRectI b = _dstImg->getBounds();
        const size_t n = draw->rects.size() / 4;
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* d = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            float v = float(y - b.y1) + 0.5f;
            for (int x = win.x1; x < win.x2; ++x, d += 4) {
                const float* p = static_cast<const float*>(src ? src->getPixelAddress(x, y) : nullptr);
                float o[4] = {0, 0, 0, 0};
                if (p) o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = p[3];
                float u = float(x - b.x1) + 0.5f;
                for (size_t r = 0; r < n; ++r) {
                    const float* rc = &draw->rects[r * 4];
                    if (u >= rc[0] && u < rc[2] && v >= rc[1] && v < rc[3]) {
                        o[0] = draw->colors[r * 3], o[1] = draw->colors[r * 3 + 1], o[2] = draw->colors[r * 3 + 2];
                        o[3] = 1.0f;
                    }
                }
                d[0] = o[0], d[1] = o[1], d[2] = o[2], d[3] = o[3];
            }
        }
    }
};

class PaletteCardPlugin : public ImageEffect {
public:
    explicit PaletteCardPlugin(OfxImageEffectHandle h) : ImageEffect(h)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        m_Count = fetchIntParam("count");
        m_Pos = fetchChoiceParam("position");
        m_Order = fetchChoiceParam("order");
        m_Size = fetchDoubleParam("size");
        m_Prop = fetchBooleanParam("proportional");
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) override
    {
        rois.setRegionOfInterest(*m_Src, m_Src->getRegionOfDefinition(args.time));
    }

    void render(const RenderArguments& args) override
    {
        if (m_Dst->getPixelDepth() != eBitDepthFloat || m_Dst->getPixelComponents() != ePixelComponentRGBA)
            throwSuiteStatusException(kOfxStatErrUnsupported);
        std::unique_ptr<Image> dst(m_Dst->fetchImage(args.time));
        std::unique_ptr<Image> src(m_Src->fetchImage(args.time));
        if (!dst || !src) throwSuiteStatusException(kOfxStatFailed);
        const double t = args.time;
        const bool gpu = args.isEnabledOpenCLRender;
        const char* err = nullptr;

        drs::FrameSamples f;
        if (!drs::measureFrame(src.get(), gpu, args.pOpenCLCmdQ, f, &err)) fail(err);
        std::vector<drs::Swatch> pal = drs::paletteFromSamples(f, m_Count->getValueAtTime(t));
        int order = 0, pos = 0;
        m_Order->getValueAtTime(t, order);
        m_Pos->getValueAtTime(t, pos);
        drs::sortPalette(pal, order);
        const OfxRectI b = dst->getBounds();
        const int W = b.x2 - b.x1, H = b.y2 - b.y1;
        drs::PaletteDraw draw =
            drs::layoutPalette(pal, W, H, pos, float(m_Size->getValueAtTime(t)), m_Prop->getValueAtTime(t));

        if (gpu) {
            if (!drs::clPalette(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), W, H, draw.rects,
                                draw.colors, &err))
                fail(err);
            return;
        }
        PaletteProcessor p(*this);
        p.setDstImg(dst.get());
        p.setRenderWindow(args.renderWindow);
        p.src = src.get();
        p.draw = &draw;
        p.process();
    }

private:
    void fail(const char* err)
    {
        sendMessage(Message::eMessageError, "", std::string("Palette Card: ") + (err ? err : "?"));
        throwSuiteStatusException(kOfxStatFailed);
    }

    Clip* m_Dst;
    Clip* m_Src;
    IntParam* m_Count;
    ChoiceParam* m_Pos;
    ChoiceParam* m_Order;
    DoubleParam* m_Size;
    BooleanParam* m_Prop;
};

} // namespace

mDeclarePluginFactory(PaletteCardFactory, {}, {});

void PaletteCardFactory::describe(ImageEffectDescriptor& desc) { drs::describeStatLut(desc, kPluginName, kPluginDescription); }

void PaletteCardFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    drs::describeClips(desc);
    PageParamDescriptor* page = desc.definePageParam("Controls");
    IntParamDescriptor* n = desc.defineIntParam("count");
    n->setLabels("Colors", "Colors", "Colors");
    n->setHint("How many colors to pick out of the frame.");
    n->setDefault(5);
    n->setRange(2, 8);
    n->setDisplayRange(2, 8);
    page->addChild(*n);
    ChoiceParamDescriptor* p = desc.defineChoiceParam("position");
    p->setLabels("Position", "Position", "Position");
    p->appendOption("Bottom Strip");
    p->appendOption("Right Strip");
    p->appendOption("Full Card");
    p->setDefault(0);
    page->addChild(*p);
    ChoiceParamDescriptor* o = desc.defineChoiceParam("order");
    o->setLabels("Order", "Order", "Order");
    o->setHint("Share: biggest first. Lightness: dark to light. Hue: around the color wheel, greys last.");
    o->appendOption("Share");
    o->appendOption("Lightness");
    o->appendOption("Hue");
    o->setDefault(0);
    page->addChild(*o);
    drs::slider(desc, page, nullptr, "size", "Strip Size", "Thickness of the strip, as a share of the frame.", 0.14, 0.05,
                0.4);
    BooleanParamDescriptor* pr = desc.defineBooleanParam("proportional");
    pr->setLabels("Proportional Widths", "Proportional Widths", "Proportional Widths");
    pr->setHint("Each swatch as wide as the share of the frame it takes.");
    pr->setDefault(true);
    page->addChild(*pr);
}

ImageEffect* PaletteCardFactory::createInstance(OfxImageEffectHandle handle, ContextEnum) { return new PaletteCardPlugin(handle); }

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static PaletteCardFactory p(kPluginIdentifier, 1, 0);
    ids.push_back(&p);
}
