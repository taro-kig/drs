// Look Explorer - OFX plugin for DaVinci Resolve
// Shows the frame as a 3x3 contact sheet of looks. Pick the one you like, press
// Next Round, and the next 9 are variations around it. Repeat until it feels right,
// then switch View to Result. No color theory needed.
// Works on DaVinci Wide Gamut / DaVinci Intermediate, like the DCTLs in this repo.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ClLut.h"
#include "LookMath.h"
#include "Lut3D.h"

#define kPluginName "Look Explorer"
#define kPluginGrouping "drs"
#define kPluginDescription                                                                 \
    "Find a look by picking, not by tweaking.\n"                                           \
    "1. The viewer shows 9 looks. Set Pick to the number you like best.\n"                 \
    "2. Press Next Round: 9 new looks around your pick. Repeat.\n"                         \
    "3. Switch View to Result. Fine-tune with Amount, Warmth and Punch.\n"                 \
    "Working space: DaVinci Wide Gamut / DaVinci Intermediate."
#define kPluginIdentifier "io.github.taro-kig.drs.LookExplorer"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

using namespace OFX;

namespace {

enum ViewMode { kViewExplore = 0, kViewResult = 1 };

std::string pid(const char* base, int i)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%d", base, i);
    return buf;
}

// 3x5 digits 1..9, rows top to bottom, 3 bits each (MSB = left)
const unsigned char kDigits[9][5] = {
    {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

class LookProcessor : public ImageProcessor {
public:
    explicit LookProcessor(ImageEffect& effect) : ImageProcessor(effect) {}

    Image* src = nullptr;
    bool grid = true;
    int pick = 4;
    drs::LookParams looks[9];                  // exact model, for pixels outside the LUT range
    std::shared_ptr<const drs::Lut3D> luts[9]; // baked looks (index 0 in Result view)

    drs::V3 look(int i, drs::V3 c) const { return luts[i]->inDomain(c) ? luts[i]->apply(c) : drs::applyLook(c, looks[i]); }

    void multiThreadProcessImages(OfxRectI win) override
    {
        const OfxRectI b = src->getBounds();
        const int W = b.x2 - b.x1, H = b.y2 - b.y1;
        const int gap = std::max(2, W / 500);
        const int blk = std::max(2, H / 100);

        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* dst = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, dst += 4) {
                const float* s = static_cast<const float*>(src->getPixelAddress(x, y));
                float alpha = s ? s[3] : 1.0f;
                drs::V3 o;
                if (!grid) {
                    o = s ? look(0, {s[0], s[1], s[2]}) : drs::V3{0, 0, 0};
                } else {
                    o = gridPixel(x, y, b, W, H, gap, blk);
                    alpha = 1.0f;
                }
                dst[0] = o.x;
                dst[1] = o.y;
                dst[2] = o.z;
                dst[3] = alpha;
            }
        }
    }

private:
    drs::V3 sample(int x, int y, const OfxRectI& b)
    {
        x = std::clamp(x, b.x1, b.x2 - 1);
        y = std::clamp(y, b.y1, b.y2 - 1);
        const float* s = static_cast<const float*>(src->getPixelAddress(x, y));
        return s ? drs::V3{s[0], s[1], s[2]} : drs::V3{0, 0, 0};
    }

    drs::V3 gridPixel(int x, int y, const OfxRectI& b, int W, int H, int gap, int blk)
    {
        int col = std::min(2, (x - b.x1) * 3 / W);
        int row = std::min(2, (b.y2 - 1 - y) * 3 / H); // 0 = top row
        int cell = row * 3 + col;
        int cx0 = b.x1 + col * W / 3, cx1 = b.x1 + (col + 1) * W / 3;
        int cyTop = b.y2 - row * H / 3, cyBot = b.y2 - (row + 1) * H / 3;

        // frame: dark gaps, a bright frame around the current pick
        int edge = std::min(std::min(x - cx0, cx1 - 1 - x), std::min(y - cyBot, cyTop - 1 - y));
        if (edge < gap) return {0.04f, 0.04f, 0.04f};
        if (cell == pick && edge < gap * 4) return {0.62f, 0.55f, 0.12f};

        // number in the top-left corner: 5x7 blocks of dark backing, 3x5 digit inside
        int bx = (x - cx0 - gap * 5) / blk, by = (cyTop - 1 - y - gap * 5) / blk;
        if (x - cx0 >= gap * 5 && cyTop - 1 - y >= gap * 5 && bx < 5 && by < 7) {
            bool on = bx >= 1 && bx <= 3 && by >= 1 && by <= 5 && ((kDigits[cell][by - 1] >> (3 - bx)) & 1);
            if (on) return cell == pick ? drs::V3{0.62f, 0.55f, 0.12f} : drs::V3{0.58f, 0.58f, 0.58f};
            return {0.06f, 0.06f, 0.06f};
        }

        // shrunken copy of the whole frame, 3x3 box filtered
        float fx = (x - cx0 + 0.5f) / float(cx1 - cx0);
        float fy = (y - cyBot + 0.5f) / float(cyTop - cyBot);
        int sx = b.x1 + int(fx * W), sy = b.y1 + int(fy * H);
        drs::V3 acc = {0, 0, 0};
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                drs::V3 v = sample(sx + i, sy + j, b);
                acc.x += v.x;
                acc.y += v.y;
                acc.z += v.z;
            }
        return look(cell, {acc.x / 9.0f, acc.y / 9.0f, acc.z / 9.0f});
    }
};

class LookExplorerPlugin : public ImageEffect {
public:
    explicit LookExplorerPlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        m_View = fetchChoiceParam("view");
        m_Pick = fetchIntParam("pick");
        m_Variety = fetchDoubleParam("variety");
        m_Amount = fetchDoubleParam("amount");
        m_Warmth = fetchDoubleParam("warmth");
        m_Punch = fetchDoubleParam("punch");
        for (int d = 0; d < drs::kDims; ++d) {
            m_C[d] = fetchDoubleParam(pid("c", d));
            m_PC[d] = fetchDoubleParam(pid("pc", d));
        }
        m_Round = fetchIntParam("round");
        m_Seed = fetchIntParam("seed");
        m_PRound = fetchIntParam("pround");
        m_PSeed = fetchIntParam("pseed");
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) override
    {
        // the contact sheet needs the whole frame for every output pixel
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
        drs::Look center = readCenter(m_C);
        int round = m_Round->getValue();
        uint32_t seed = uint32_t(m_Seed->getValue());
        float variety = float(m_Variety->getValueAtTime(t));
        float amount = float(m_Amount->getValueAtTime(t));
        float warmth = float(m_Warmth->getValueAtTime(t));
        float punch = float(m_Punch->getValueAtTime(t));
        int view = 0;
        m_View->getValueAtTime(t, view);
        int pick = std::clamp(m_Pick->getValueAtTime(t), 1, 9) - 1;

        LookProcessor proc(*this);
        proc.setDstImg(dst.get());
        proc.setRenderWindow(args.renderWindow);
        proc.src = src.get();
        proc.grid = view == kViewExplore;
        proc.pick = pick;
        // 65^3 LUT per look (about 6 ms to bake each, only when a setting changes)
        const int cells = proc.grid ? 9 : 1;
        const int n = 65;
        for (int c = 0; c < cells; ++c) {
            drs::Look l = drs::finish(drs::variation(center, round, seed, proc.grid ? c : pick, variety), amount,
                                      warmth, punch);
            drs::LookParams k = drs::prepare(l, amount);
            std::vector<double> key(l.p, l.p + drs::kDims);
            key.push_back(amount);
            proc.looks[c] = k;
            proc.luts[c] = (proc.grid ? m_GridCache[c] : m_ResultCache).get(key, n, [k](drs::V3 v) {
                return drs::applyLook(v, k);
            });
        }

        if (args.isEnabledOpenCLRender) {
            const OfxRectI b = dst->getBounds();
            const int W = b.x2 - b.x1, H = b.y2 - b.y1;
            std::vector<const float*> ptrs;
            for (int c = 0; c < cells; ++c) ptrs.push_back(proc.luts[c]->data().data());
            drs::ClGrid grid;
            grid.on = proc.grid;
            grid.pick = pick;
            grid.gap = std::max(2, W / 500);
            grid.blk = std::max(2, H / 100);
            const char* err = nullptr;
            if (!drs::clApplyLuts(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), W, H, ptrs, n,
                                  drs::Lut3D::kLo, proc.luts[0]->scale(), grid, &err)) {
                sendMessage(Message::eMessageError, "", std::string("Look Explorer GPU: ") + (err ? err : "?"));
                throwSuiteStatusException(kOfxStatFailed);
            }
            return;
        }
        proc.process();
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) override
    {
        if (args.reason != eChangeUserEdit) return;
        if (name == "next") {
            drs::Look center = readCenter(m_C);
            int round = m_Round->getValue();
            int seed = m_Seed->getValue();
            int pick = std::clamp(m_Pick->getValue(), 1, 9) - 1;
            float variety = float(m_Variety->getValue());
            drs::Look chosen = drs::variation(center, round, uint32_t(seed), pick, variety);

            beginEditBlock("Next Round");
            saveUndo(center, round, seed);
            writeCenter(m_C, chosen);
            m_Round->setValue(round + 1);
            m_Seed->setValue(int(drs::hashUnit(uint32_t(seed), uint32_t(round), 77u) * 1.0e6f) + 1000000);
            m_Pick->setValue(5);
            m_View->setValue(kViewExplore);
            endEditBlock();
        } else if (name == "back") {
            beginEditBlock("Back");
            drs::Look prev = readCenter(m_PC);
            writeCenter(m_C, prev);
            m_Round->setValue(m_PRound->getValue());
            m_Seed->setValue(m_PSeed->getValue());
            m_Pick->setValue(5);
            m_View->setValue(kViewExplore);
            endEditBlock();
        } else if (name == "restart") {
            beginEditBlock("Start Over");
            saveUndo(readCenter(m_C), m_Round->getValue(), m_Seed->getValue());
            writeCenter(m_C, drs::Look{});
            m_Round->setValue(0);
            m_Pick->setValue(5);
            m_View->setValue(kViewExplore);
            endEditBlock();
        }
    }

private:
    drs::Look readCenter(DoubleParam* const* ps)
    {
        drs::Look l;
        for (int d = 0; d < drs::kDims; ++d) l.p[d] = float(ps[d]->getValue());
        return l;
    }

    void writeCenter(DoubleParam* const* ps, const drs::Look& l)
    {
        for (int d = 0; d < drs::kDims; ++d) ps[d]->setValue(l.p[d]);
    }

    void saveUndo(const drs::Look& center, int round, int seed)
    {
        writeCenter(m_PC, center);
        m_PRound->setValue(round);
        m_PSeed->setValue(seed);
    }

    drs::LutCache m_GridCache[9];
    drs::LutCache m_ResultCache;
    Clip* m_Dst;
    Clip* m_Src;
    ChoiceParam* m_View;
    IntParam* m_Pick;
    DoubleParam* m_Variety;
    DoubleParam* m_Amount;
    DoubleParam* m_Warmth;
    DoubleParam* m_Punch;
    DoubleParam* m_C[drs::kDims];
    DoubleParam* m_PC[drs::kDims];
    IntParam* m_Round;
    IntParam* m_Seed;
    IntParam* m_PRound;
    IntParam* m_PSeed;
};

void defineSlider(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label,
                  const char* hint, double def, double lo, double hi)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(lo, hi);
    p->setIncrement(0.01);
    p->setDoubleType(eDoubleTypePlain);
    page->addChild(*p);
}

void defineButton(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name, const char* label,
                  const char* hint)
{
    PushButtonParamDescriptor* p = desc.definePushButtonParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    page->addChild(*p);
}

void defineHiddenDouble(ImageEffectDescriptor& desc, const std::string& name)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setDefault(0.0);
    p->setIsSecret(true);
    p->setAnimates(false);
}

void defineHiddenInt(ImageEffectDescriptor& desc, const char* name, int def)
{
    IntParamDescriptor* p = desc.defineIntParam(name);
    p->setDefault(def);
    p->setRange(-2147483647, 2147483647);
    p->setIsSecret(true);
    p->setAnimates(false);
}

} // namespace

mDeclarePluginFactory(LookExplorerFactory, {}, {});

void LookExplorerFactory::describe(ImageEffectDescriptor& desc)
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

void LookExplorerFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
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

    ChoiceParamDescriptor* view = desc.defineChoiceParam("view");
    view->setLabels("View", "View", "View");
    view->setHint("Explore = 9 looks side by side. Result = the picked look on the full frame.");
    view->appendOption("Explore 3x3");
    view->appendOption("Result");
    view->setDefault(kViewExplore);
    view->setAnimates(false);
    page->addChild(*view);

    IntParamDescriptor* pick = desc.defineIntParam("pick");
    pick->setLabels("Pick", "Pick", "Pick");
    pick->setHint("The look you like best (numbers in the corners, 1 = top left, 5 = center).");
    pick->setDefault(5);
    pick->setRange(1, 9);
    pick->setDisplayRange(1, 9);
    pick->setAnimates(false);
    page->addChild(*pick);

    defineButton(desc, page, "next", "Next Round", "Keep the picked look and show 9 variations around it.");
    defineButton(desc, page, "back", "Back", "Go back one round.");
    defineButton(desc, page, "restart", "Start Over", "Back to the 9 starting looks.");

    defineSlider(desc, page, "variety", "Variety",
                 "How different the variations are. They also get closer each round.", 0.6, 0.1, 1.5);
    defineSlider(desc, page, "amount", "Amount", "Strength of the whole look. 0 = untouched.", 1.0, 0.0, 1.5);
    defineSlider(desc, page, "warmth", "Warmth", "Warmer (+) or cooler (-) on top of the look.", 0.0, -1.0, 1.0);
    defineSlider(desc, page, "punch", "Punch", "More (+) or less (-) contrast and color.", 0.0, -1.0, 1.0);

    // state, hidden from the user
    for (int d = 0; d < drs::kDims; ++d) {
        defineHiddenDouble(desc, pid("c", d));
        defineHiddenDouble(desc, pid("pc", d));
    }
    defineHiddenInt(desc, "round", 0);
    defineHiddenInt(desc, "seed", 12345);
    defineHiddenInt(desc, "pround", 0);
    defineHiddenInt(desc, "pseed", 12345);
}

ImageEffect* LookExplorerFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new LookExplorerPlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static LookExplorerFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
