// Photo Frame - OFX plugin for DaVinci Resolve
// Puts the photo on a white / black / paper frame with an EXIF caption
// ("1/200 s · F8.0 · ISO 64 · 35mm"). OFX hosts don't tell plugins which file is being
// graded, so pick the photo file once: the plugin reads its EXIF itself (raw or JPEG) and
// fills the caption fields, which stay editable.
// GPU (OpenCL) when the host offers it, otherwise CPU.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ClLut.h"
#include "Exif.h"
#include "FrameMath.h"
#include "TextRaster.h"

#define kPluginName "Photo Frame"
#define kPluginGrouping "drs"
#define kPluginDescription                                                                      \
    "Frame (white, black or paper) with an EXIF caption.\n"                                     \
    "Pick the photo's file under Caption to read its EXIF (shutter, aperture, ISO, focal "      \
    "length...); the fields stay editable. The output is the timeline frame: the photo is "      \
    "scaled into it, so a timeline with the photo's aspect ratio gives the classic look.\n"     \
    "Colors: set Colors to match where the node sits (before or after the output transform)."
#define kPluginIdentifier "io.github.taro-kig.drs.PhotoFrame"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

using namespace OFX;

namespace {

const char* const kFieldNames[7] = {"shutter", "aperture", "iso", "focal", "camera", "lens", "date"};
const char* const kFieldLabels[7] = {"Shutter", "Aperture", "ISO", "Focal Length", "Camera", "Lens", "Date"};

class FrameProcessor : public ImageProcessor {
public:
    explicit FrameProcessor(ImageEffect& effect) : ImageProcessor(effect) {}
    Image* src = nullptr;
    drs::FrameDraw draw;

    void multiThreadProcessImages(OfxRectI win) override
    {
        auto at = [this](int x, int y) { return static_cast<const float*>(src->getPixelAddress(x, y)); };
        for (int y = win.y1; y < win.y2; ++y) {
            if (_effect.abort()) break;
            float* dst = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, dst += 4) {
                drs::V3 o = drs::framePixel(at, x, y, draw);
                dst[0] = o.x;
                dst[1] = o.y;
                dst[2] = o.z;
                dst[3] = 1.0f;
            }
        }
    }
};

class PhotoFramePlugin : public ImageEffect {
public:
    explicit PhotoFramePlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        m_Color = fetchChoiceParam("frameColor");
        m_Custom = fetchRGBParam("customColor");
        m_Size = fetchDoubleParam("photoSize");
        m_Pos = fetchDoubleParam("position");
        m_Edge = fetchDoubleParam("edgeLine");
        m_Bars = fetchBooleanParam("detectBars");
        m_Space = fetchChoiceParam("colorSpace");
        m_Bright = fetchDoubleParam("frameBrightness");
        m_File = fetchStringParam("photoFile");
        m_ShowCaption = fetchBooleanParam("showCaption");
        m_Template = fetchStringParam("template");
        for (int i = 0; i < 7; ++i) m_Fields[i] = fetchStringParam(kFieldNames[i]);
        m_TextSize = fetchDoubleParam("textSize");
        m_TextGap = fetchDoubleParam("textGap");
        m_Align = fetchChoiceParam("align");
        m_TextColorMode = fetchChoiceParam("textColorMode");
        m_TextColor = fetchRGBParam("textColor");
        m_Font = fetchStringParam("fontFile");
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) override
    {
        rois.setRegionOfInterest(*m_Src, m_Src->getRegionOfDefinition(args.time));
    }

    void changedParam(const InstanceChangedArgs& args, const std::string& name) override
    {
        if (args.reason != eChangeUserEdit) return;
        if (name != "photoFile" && name != "readExif") return;
        std::string path;
        m_File->getValue(path);
        if (path.empty()) return;
        drs::ExifInfo e = drs::readExif(path);
        if (!e.ok) {
            sendMessage(Message::eMessageError, "", "Photo Frame: no EXIF found in " + path);
            return;
        }
        const std::string vals[7] = {drs::fmtShutter(e.exposure), drs::fmtAperture(e.fnumber), drs::fmtIso(e.iso),
                                     drs::fmtFocal(e.focal),      drs::fmtCamera(e),           e.lens,
                                     e.date.substr(0, std::min<size_t>(e.date.size(), 10))};
        beginEditBlock("Read EXIF");
        for (int i = 0; i < 7; ++i) m_Fields[i]->setValue(vals[i]);
        endEditBlock();
    }

    void render(const RenderArguments& args) override
    {
        if (m_Dst->getPixelDepth() != eBitDepthFloat || m_Dst->getPixelComponents() != ePixelComponentRGBA) {
            throwSuiteStatusException(kOfxStatErrUnsupported);
        }
        std::unique_ptr<Image> dst(m_Dst->fetchImage(args.time));
        std::unique_ptr<Image> src(m_Src->fetchImage(args.time));
        if (!dst || !src) throwSuiteStatusException(kOfxStatFailed);
        const double t = args.time;
        const OfxRectI b = dst->getBounds();
        const int W = b.x2 - b.x1, H = b.y2 - b.y1;
        const bool gpu = args.isEnabledOpenCLRender;
        const char* err = nullptr;

        // 1. where the photo is in the source (without Resolve's black bars)
        int cx0 = 0, cy0 = 0, cw = W, ch = H;
        if (m_Bars->getValueAtTime(t)) {
            std::vector<float> colMax, rowMax;
            if (gpu) {
                if (!drs::clContentMax(args.pOpenCLCmdQ, src->getPixelData(), W, H, colMax, rowMax, &err)) fail(err);
            } else {
                colMax.assign(size_t(W), 0.0f);
                rowMax.assign(size_t(H), 0.0f);
                for (int y = 0; y < H; ++y) {
                    const float* s = static_cast<const float*>(src->getPixelAddress(b.x1, b.y1 + y));
                    if (!s) continue;
                    for (int x = 0; x < W; ++x, s += 4) {
                        float m = std::max(std::fabs(s[0]), std::max(std::fabs(s[1]), std::fabs(s[2])));
                        colMax[x] = std::max(colMax[x], m);
                        rowMax[y] = std::max(rowMax[y], m);
                    }
                }
            }
            drs::contentRect(colMax, rowMax, cx0, cy0, cw, ch);
        }

        // 2. caption bitmap
        const float shortSide = float(std::min(W, H));
        std::shared_ptr<const drs::TextMask> mask;
        if (m_ShowCaption->getValueAtTime(t)) {
            std::string tmpl, f[7], font;
            m_Template->getValueAtTime(t, tmpl);
            for (int i = 0; i < 7; ++i) m_Fields[i]->getValueAtTime(t, f[i]);
            m_Font->getValueAtTime(t, font);
            if (font.empty()) font = drs::defaultFontPath();
            std::string text = drs::fillCaption(tmpl, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
            float px = float(m_TextSize->getValueAtTime(t)) * 0.01f * shortSide;
            mask = captionMask(text, font, px);
        }

        // 3. layout and colors
        int space = 0, colorMode = 0, align = 0, textMode = 0;
        m_Space->getValueAtTime(t, space);
        m_Color->getValueAtTime(t, colorMode);
        m_Align->getValueAtTime(t, align);
        m_TextColorMode->getValueAtTime(t, textMode);
        double r, g, bl;
        m_Custom->getValueAtTime(t, r, g, bl);
        drs::V3 bgDisp = drs::frameDisplayColor(colorMode, {float(r), float(g), float(bl)});
        m_TextColor->getValueAtTime(t, r, g, bl);
        drs::V3 textDisp = textMode == 0 ? drs::autoTextColor(bgDisp) : drs::V3{float(r), float(g), float(bl)};
        float stops = float(m_Bright->getValueAtTime(t));

        drs::FrameDraw d;
        int tw = mask ? mask->w : 0;
        float gap = float(m_TextGap->getValueAtTime(t)) * 0.01f * shortSide;
        d.layout = drs::computeLayout(W, H, float(cw), float(ch), float(m_Size->getValueAtTime(t)),
                                      float(m_Pos->getValueAtTime(t)), align, tw, gap);
        d.cx0 = cx0, d.cy0 = cy0, d.cw = cw, d.ch = ch;
        d.bg = drs::frameOutputColor(bgDisp, space, stops);
        d.text = drs::frameOutputColor(textDisp, space, stops);
        d.edge = drs::frameOutputColor(drs::edgeColor(bgDisp), space, stops);
        d.edgeWidth = float(m_Edge->getValueAtTime(t)) * shortSide / 2000.0f; // in px of a 2000 px image
        if (mask && mask->w > 0) {
            d.mask = mask->alpha.data();
            d.tw = mask->w;
            d.th = mask->h;
        }

        // 4. draw
        if (gpu) {
            drs::ClFrameArgs a;
            a.cx0 = cx0, a.cy0 = cy0, a.cw = cw, a.ch = ch;
            a.px0 = d.layout.photo.x0, a.py0 = d.layout.photo.y0, a.pw = d.layout.photo.w, a.ph = d.layout.photo.h;
            const float cols[10] = {d.bg.x, d.bg.y, d.bg.z, d.text.x, d.text.y, d.text.z,
                                    d.edge.x, d.edge.y, d.edge.z, d.edgeWidth};
            std::copy(cols, cols + 10, a.colors);
            a.mask = d.mask, a.tw = d.tw, a.th = d.th, a.tx = d.layout.textX, a.ttop = d.layout.textTop;
            if (!drs::clFrame(args.pOpenCLCmdQ, src->getPixelData(), dst->getPixelData(), W, H, a, &err)) fail(err);
            return;
        }
        FrameProcessor proc(*this);
        proc.setDstImg(dst.get());
        proc.setRenderWindow(args.renderWindow);
        proc.src = src.get();
        proc.draw = d;
        proc.process();
    }

private:
    [[noreturn]] void fail(const char* err)
    {
        sendMessage(Message::eMessageError, "", std::string("Photo Frame GPU: ") + (err ? err : "?"));
        throwSuiteStatusException(kOfxStatFailed);
        throw std::runtime_error("unreachable");
    }

    // the caption only changes with its text, font or size
    std::shared_ptr<const drs::TextMask> captionMask(const std::string& text, const std::string& font, float px)
    {
        std::lock_guard<std::mutex> lock(m_MaskMutex);
        if (m_Mask && text == m_MaskText && font == m_MaskFont && px == m_MaskPx) return m_Mask;
        std::string err;
        auto m = std::make_shared<drs::TextMask>(drs::rasterizeText(text, font, px, &err));
        if (!err.empty()) sendMessage(Message::eMessageWarning, "", "Photo Frame: " + err);
        m_Mask = m, m_MaskText = text, m_MaskFont = font, m_MaskPx = px;
        return m_Mask;
    }

    std::mutex m_MaskMutex;
    std::shared_ptr<const drs::TextMask> m_Mask;
    std::string m_MaskText, m_MaskFont;
    float m_MaskPx = 0;

    Clip* m_Dst;
    Clip* m_Src;
    ChoiceParam* m_Color;
    RGBParam* m_Custom;
    DoubleParam* m_Size;
    DoubleParam* m_Pos;
    DoubleParam* m_Edge;
    BooleanParam* m_Bars;
    ChoiceParam* m_Space;
    DoubleParam* m_Bright;
    StringParam* m_File;
    BooleanParam* m_ShowCaption;
    StringParam* m_Template;
    StringParam* m_Fields[7];
    DoubleParam* m_TextSize;
    DoubleParam* m_TextGap;
    ChoiceParam* m_Align;
    ChoiceParam* m_TextColorMode;
    RGBParam* m_TextColor;
    StringParam* m_Font;
};

DoubleParamDescriptor* slider(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* grp,
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
    p->setParent(*grp);
    page->addChild(*p);
    return p;
}

ChoiceParamDescriptor* choice(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* grp,
                              const char* name, const char* label, const char* hint,
                              std::initializer_list<const char*> options, int def)
{
    ChoiceParamDescriptor* p = desc.defineChoiceParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    for (const char* o : options) p->appendOption(o);
    p->setDefault(def);
    p->setAnimates(false);
    p->setParent(*grp);
    page->addChild(*p);
    return p;
}

StringParamDescriptor* text(ImageEffectDescriptor& desc, PageParamDescriptor* page, GroupParamDescriptor* grp,
                            const char* name, const char* label, const char* hint, const char* def,
                            StringTypeEnum type = eStringTypeSingleLine)
{
    StringParamDescriptor* p = desc.defineStringParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setStringType(type);
    p->setAnimates(false);
    p->setParent(*grp);
    page->addChild(*p);
    return p;
}

GroupParamDescriptor* group(ImageEffectDescriptor& desc, PageParamDescriptor* page, const char* name,
                            const char* label, bool open)
{
    GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    page->addChild(*g);
    return g;
}

} // namespace

mDeclarePluginFactory(PhotoFrameFactory, {}, {});

void PhotoFrameFactory::describe(ImageEffectDescriptor& desc)
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

void PhotoFrameFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum /*context*/)
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

    GroupParamDescriptor* fr = group(desc, page, "frame", "Frame", true);
    choice(desc, page, fr, "frameColor", "Frame Color", "White, black, or a warm, slightly dull paper.",
           {"White", "Black", "Paper", "Custom"}, drs::kFramePaper);
    RGBParamDescriptor* custom = desc.defineRGBParam("customColor");
    custom->setLabels("Custom Color", "Custom Color", "Custom Color");
    custom->setHint("Frame color when Frame Color is Custom (as it should look on screen).");
    custom->setDefault(0.85, 0.83, 0.80);
    custom->setParent(*fr);
    page->addChild(*custom);
    slider(desc, page, fr, "photoSize", "Photo Size", "How much of the frame the photo fills (1 = no border).",
           0.85, 0.3, 1.0);
    slider(desc, page, fr, "position", "Position",
           "Vertical position: 0 = top, 0.5 = centered. The default leaves room for the caption below.", 0.17, 0.0, 1.0);
    slider(desc, page, fr, "edgeLine", "Edge Line", "Thin line around the photo (width in px of a 2000 px image).",
           0.0, 0.0, 6.0);
    BooleanParamDescriptor* bars = desc.defineBooleanParam("detectBars");
    bars->setLabels("Ignore Black Bars", "Ignore Black Bars", "Ignore Black Bars");
    bars->setHint("Leave out the black bars Resolve adds when the photo and timeline aspect ratios differ.");
    bars->setDefault(true);
    bars->setParent(*fr);
    page->addChild(*bars);
    choice(desc, page, fr, "colorSpace", "Colors",
           "Before Output Transform: the node sits in the DaVinci Wide Gamut / Intermediate grade (color managed "
           "projects). After Output Transform: the node comes after the conversion to the display.",
           {"Before Output Transform (DWG / DI)", "After Output Transform (Display)"}, drs::kSpaceDI);
    slider(desc, page, fr, "frameBrightness", "Frame Brightness",
           "Fine-tune the frame and caption brightness (stops) if your output transform renders them off.", 0.0,
           -3.0, 3.0);

    GroupParamDescriptor* cap = group(desc, page, "caption", "Caption", true);
    BooleanParamDescriptor* show = desc.defineBooleanParam("showCaption");
    show->setLabels("Show Caption", "Show Caption", "Show Caption");
    show->setDefault(true);
    show->setParent(*cap);
    page->addChild(*show);
    text(desc, page, cap, "photoFile", "Photo File",
         "The photo's file (raw or JPEG). Its EXIF fills the fields below.", "", eStringTypeFilePath);
    PushButtonParamDescriptor* read = desc.definePushButtonParam("readExif");
    read->setLabels("Read EXIF Again", "Read EXIF Again", "Read EXIF Again");
    read->setHint("Re-read the EXIF of Photo File (overwrites the fields below).");
    read->setParent(*cap);
    page->addChild(*read);
    text(desc, page, cap, "template", "Caption",
         "Text of the caption. Fields: {exposure} = shutter · aperture · ISO · focal, or {shutter} {aperture} {iso} "
         "{focal} {camera} {lens} {date}.",
         "{exposure}");
    for (int i = 0; i < 7; ++i)
        text(desc, page, cap, kFieldNames[i], kFieldLabels[i], "From EXIF; edit freely.", "");

    GroupParamDescriptor* ty = group(desc, page, "type", "Text", false);
    slider(desc, page, ty, "textSize", "Text Size", "Line height, % of the frame's short side.", 1.2, 0.3, 5.0);
    slider(desc, page, ty, "textGap", "Text Gap", "Space between photo and caption, % of the short side.", 3.0, 0.0,
           10.0);
    choice(desc, page, ty, "align", "Align", "Caption alignment along the photo's edges.", {"Left", "Center", "Right"},
           drs::kAlignLeft);
    choice(desc, page, ty, "textColorMode", "Text Color", "Auto: dark on light frames, light on black.",
           {"Auto", "Custom"}, 0);
    RGBParamDescriptor* tc = desc.defineRGBParam("textColor");
    tc->setLabels("Custom Text Color", "Custom Text Color", "Custom Text Color");
    tc->setDefault(0.1, 0.1, 0.1);
    tc->setParent(*ty);
    page->addChild(*tc);
    text(desc, page, ty, "fontFile", "Font File", "A .ttf / .otf / .ttc font. Empty = Arial.", "",
         eStringTypeFilePath);
}

ImageEffect* PhotoFrameFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new PhotoFramePlugin(handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static PhotoFrameFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
