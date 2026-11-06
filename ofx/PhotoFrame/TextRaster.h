// TextRaster.h - render one line of UTF-8 text to an 8-bit alpha mask with stb_truetype,
// using a font file from the system (or one the user picks). No OFX dependency.
#pragma once

#include <string>
#include <vector>

namespace drs {

struct TextMask {
    int w = 0, h = 0;
    std::vector<unsigned char> alpha; // w x h, row 0 = top
};

// Arial (Windows / macOS) or DejaVu Sans (Linux); empty if none is found.
std::string defaultFontPath();

// pixelHeight = full line height (ascent + descent). Returns an empty mask and sets `err`
// when the font cannot be read. Missing glyphs are drawn as the font's box glyph.
TextMask rasterizeText(const std::string& utf8, const std::string& fontPath, float pixelHeight, std::string* err);

} // namespace drs
