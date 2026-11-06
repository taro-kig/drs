// TextRaster.cpp - see TextRaster.h
#include "TextRaster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace drs {
namespace {

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

std::mutex g_FontMutex;
std::map<std::string, std::shared_ptr<Font>> g_Fonts;

std::shared_ptr<Font> loadFont(const std::string& path, std::string* err)
{
    std::lock_guard<std::mutex> lock(g_FontMutex);
    auto it = g_Fonts.find(path);
    if (it != g_Fonts.end()) return it->second;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open font: " + path;
        return nullptr;
    }
    auto font = std::make_shared<Font>();
    font->data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    int offset = stbtt_GetFontOffsetForIndex(font->data.data(), 0); // first face of a .ttc
    if (offset < 0 || !stbtt_InitFont(&font->info, font->data.data(), offset)) {
        if (err) *err = "not a TrueType / OpenType font: " + path;
        return nullptr;
    }
    g_Fonts[path] = font;
    return font;
}

std::vector<int> decodeUtf8(const std::string& s)
{
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int cp = 0, n = 0;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 0x6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 0xE) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 0x1E) cp = c & 0x07, n = 4;
        else { ++i; continue; } // stray byte
        if (i + n > s.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

bool exists(const std::string& p) { return std::ifstream(p, std::ios::binary).good(); }

} // namespace

std::string defaultFontPath()
{
    std::vector<std::string> candidates;
#ifdef _WIN32
    const char* win = std::getenv("WINDIR");
    std::string fonts = std::string(win ? win : "C:\\Windows") + "\\Fonts\\";
    candidates = {fonts + "arial.ttf", fonts + "segoeui.ttf"};
#elif defined(__APPLE__)
    candidates = {"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
                  "/System/Library/Fonts/Helvetica.ttc"};
#else
    candidates = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                  "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
#endif
    for (const std::string& c : candidates)
        if (exists(c)) return c;
    return "";
}

TextMask rasterizeText(const std::string& utf8, const std::string& fontPath, float pixelHeight, std::string* err)
{
    TextMask m;
    std::vector<int> cps = decodeUtf8(utf8);
    if (cps.empty() || pixelHeight < 2.0f) return m;
    std::shared_ptr<Font> font = loadFont(fontPath, err);
    if (!font) return m;
    const stbtt_fontinfo* fi = &font->info;

    float scale = stbtt_ScaleForPixelHeight(fi, pixelHeight);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(fi, &ascent, &descent, &gap);
    float base = ascent * scale;

    // measure
    float pen = 0.0f;
    std::vector<float> xs(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        xs[i] = pen;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(fi, cps[i], &adv, &lsb);
        pen += adv * scale;
        if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(fi, cps[i], cps[i + 1]) * scale;
    }
    m.w = int(std::ceil(pen)) + 2;
    m.h = int(std::ceil((ascent - descent) * scale)) + 2;
    m.alpha.assign(size_t(m.w) * m.h, 0);

    // draw, glyph by glyph, with sub-pixel x positions
    std::vector<unsigned char> glyph;
    for (size_t i = 0; i < cps.size(); ++i) {
        float x = xs[i] + 1.0f;
        int ix = int(std::floor(x));
        float sub = x - ix;
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(fi, cps[i], scale, scale, sub, 0.0f, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        glyph.assign(size_t(gw) * gh, 0);
        stbtt_MakeCodepointBitmapSubpixel(fi, glyph.data(), gw, gh, gw, scale, scale, sub, 0.0f, cps[i]);
        int ox = ix + x0, oy = int(std::lround(base)) + y0 + 1;
        for (int y = 0; y < gh; ++y) {
            int dy = oy + y;
            if (dy < 0 || dy >= m.h) continue;
            for (int xg = 0; xg < gw; ++xg) {
                int dx = ox + xg;
                if (dx < 0 || dx >= m.w) continue;
                unsigned char& d = m.alpha[size_t(dy) * m.w + dx];
                d = std::max(d, glyph[size_t(y) * gw + xg]);
            }
        }
    }
    return m;
}

} // namespace drs
