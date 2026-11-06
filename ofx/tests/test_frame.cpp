// Checks for Photo Frame: EXIF reading (TIFF/raw and JPEG, both byte orders), caption
// formatting, layout, black-bar detection, text rendering and the CPU compositor.
// test_frame [out.bmp] also writes a preview of the three frame colors.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Exif.h"
#include "FrameMath.h"
#include "TextRaster.h"

using namespace drs;

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

// --- a tiny TIFF with IFD0 (Make, Model, ExifIFD) and an Exif IFD ---
struct TiffWriter {
    bool le;
    std::vector<uint8_t> b;
    void u16(uint16_t v) { le ? (b.push_back(v & 0xFF), b.push_back(v >> 8)) : (b.push_back(v >> 8), b.push_back(v & 0xFF)); }
    void u32(uint32_t v)
    {
        if (le) for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
        else for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
    }
    void put32(size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i) b[at + i] = le ? uint8_t(v >> (8 * i)) : uint8_t(v >> (8 * (3 - i)));
    }
};

static std::vector<uint8_t> makeTiff(bool le)
{
    TiffWriter w{le, {}};
    w.b.push_back(le ? 'I' : 'M');
    w.b.push_back(le ? 'I' : 'M');
    w.u16(42);
    w.u32(8); // IFD0 at 8
    struct E {
        uint16_t tag, type;
        uint32_t count;
        std::vector<uint8_t> data; // raw value bytes in file order (already endian-correct)
        uint32_t inl;              // inline value when data is empty
    };
    auto ascii = [](const char* s) { std::vector<uint8_t> v(s, s + std::strlen(s) + 1); return v; };
    auto rat = [&](uint32_t n, uint32_t d) {
        TiffWriter t{le, {}};
        t.u32(n);
        t.u32(d);
        return t.b;
    };
    std::string make = "SONY", model = "ILCE-7RM5";
    std::vector<E> ifd0 = {{0x010F, 2, uint32_t(make.size() + 1), ascii(make.c_str()), 0},
                           {0x0110, 2, uint32_t(model.size() + 1), ascii(model.c_str()), 0},
                           {0x8769, 4, 1, {}, 0}}; // patched below
    std::vector<E> exif = {{0x829A, 5, 1, rat(1, 200), 0},
                           {0x829D, 5, 1, rat(8, 1), 0},
                           {0x8827, 3, 1, {}, 64},
                           {0x920A, 5, 1, rat(35, 1), 0},
                           {0xA434, 2, 23, ascii("FE 35mm F1.4 GM (SEL35)"), 0},
                           {0x9003, 2, 20, ascii("2026:10:31 14:02:11"), 0}};
    // layout: IFD0 at 8, then its data, then the Exif IFD, then its data
    auto writeIfd = [&](std::vector<E>& es, size_t& ptrFix) {
        size_t start = w.b.size();
        w.u16(uint16_t(es.size()));
        size_t dataAt = start + 2 + es.size() * 12 + 4;
        std::vector<std::pair<size_t, const E*>> blobs;
        for (const E& e : es) {
            w.u16(e.tag);
            w.u16(e.type);
            w.u32(e.count);
            if (e.data.size() > 4) {
                w.u32(uint32_t(dataAt));
                blobs.push_back({dataAt, &e});
                dataAt += e.data.size() + (e.data.size() & 1);
            } else if (!e.data.empty()) {
                std::vector<uint8_t> v = e.data;
                v.resize(4, 0);
                w.b.insert(w.b.end(), v.begin(), v.end());
            } else if (e.type == 3) {
                w.u16(uint16_t(e.inl));
                w.u16(0);
            } else {
                if (e.tag == 0x8769) ptrFix = w.b.size();
                w.u32(e.inl);
            }
        }
        w.u32(0); // next IFD
        for (auto& bl : blobs) {
            w.b.resize(bl.first, 0);
            w.b.insert(w.b.end(), bl.second->data.begin(), bl.second->data.end());
            if (bl.second->data.size() & 1) w.b.push_back(0);
        }
    };
    size_t fix = 0, none = 0;
    writeIfd(ifd0, fix);
    if (w.b.size() & 1) w.b.push_back(0);
    w.put32(fix, uint32_t(w.b.size()));
    writeIfd(exif, none);
    return w.b;
}

static void writeFile(const std::string& p, const std::vector<uint8_t>& d)
{
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

static void checkExif(const char* what, const ExifInfo& e)
{
    std::string cap = fillCaption("{exposure}", fmtShutter(e.exposure), fmtAperture(e.fnumber), fmtIso(e.iso),
                                  fmtFocal(e.focal), fmtCamera(e), e.lens, e.date);
    std::printf("       %-18s \"%s\" | %s | %s | %s\n", what, cap.c_str(), fmtCamera(e).c_str(), e.lens.c_str(),
                e.date.c_str());
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s: all fields read", what);
    check(e.ok && cap == "1/200 s \xC2\xB7 F8.0 \xC2\xB7 ISO 64 \xC2\xB7 35mm" && fmtCamera(e) == "SONY ILCE-7RM5" &&
              e.lens == "FE 35mm F1.4 GM (SEL35)" && e.date.rfind("2026:10:31", 0) == 0,
          msg);
}

int main(int argc, char** argv)
{
    // EXIF in raw-style TIFF (both byte orders) and in a JPEG
    std::string tmp = argc > 2 ? argv[2] : ".";
    for (bool le : {true, false}) {
        std::string p = tmp + (le ? "/exif_le.arw" : "/exif_be.nef");
        writeFile(p, makeTiff(le));
        checkExif(le ? "TIFF little-endian" : "TIFF big-endian", readExif(p));
    }
    std::vector<uint8_t> tiff = makeTiff(true), jpg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xE1};
    uint16_t len = uint16_t(tiff.size() + 8);
    jpg.push_back(uint8_t(len >> 8));
    jpg.push_back(uint8_t(len & 0xFF));
    for (char ch : std::string("Exif\0\0", 6)) jpg.push_back(uint8_t(ch));
    jpg.insert(jpg.end(), tiff.begin(), tiff.end());
    jpg.push_back(0xFF);
    jpg.push_back(0xD9);
    writeFile(tmp + "/exif.jpg", jpg);
    checkExif("JPEG APP1", readExif(tmp + "/exif.jpg"));
    check(!readExif(tmp + "/does_not_exist.arw").ok, "missing file -> not ok, no crash");
    writeFile(tmp + "/garbage.arw", std::vector<uint8_t>(300, 0x5A));
    check(!readExif(tmp + "/garbage.arw").ok, "garbage file -> not ok, no crash");

    // formatting
    check(fmtShutter(1.0 / 8000) == "1/8000 s" && fmtShutter(0.5) == "1/2 s" && fmtShutter(2.0) == "2 s" &&
              fmtShutter(1.3) == "1.3 s",
          "shutter formats: 1/8000 s, 1/2 s, 2 s, 1.3 s");
    check(fmtAperture(1.4) == "F1.4" && fmtFocal(23.0) == "23mm" && fmtFocal(4.3) == "4.3mm", "aperture / focal");
    check(fillCaption("{exposure}", "1/200 s", "", "ISO 64", "", "", "", "") == "1/200 s \xC2\xB7 ISO 64",
          "missing fields are skipped without stray dots");
    check(fillCaption("{camera} | {lens}", "", "", "", "", "A", "B", "") == "A | B", "custom template");

    // layout: 2:3 photo on a 2:3 canvas, defaults
    FrameLayout L = computeLayout(1333, 2000, 1333, 2000, 0.85f, 0.17f, kAlignLeft, 200, 40);
    std::printf("       photo %.0f x %.0f at x %.0f, top %.0f, bottom strip %.0f px\n", L.photo.w, L.photo.h,
                L.photo.x0, 2000 - (L.photo.y0 + L.photo.h), L.photo.y0);
    check(std::fabs(L.photo.w - 1133) < 1 && std::fabs(L.photo.x0 - 100) < 1 &&
              std::fabs(2000 - (L.photo.y0 + L.photo.h) - 51) < 2 && L.photo.y0 > 240,
          "default layout matches the reference (7.5% sides, thin top, wide bottom)");
    FrameLayout Lc = computeLayout(2000, 1333, 2000, 1333, 0.85f, 0.5f, kAlignCenter, 200, 40);
    check(std::fabs(Lc.textX - (1000 - 100)) <= 1 && std::fabs(Lc.photo.y0 - (1333 - Lc.photo.h) / 2) < 1,
          "landscape, centered: caption centered under the photo");

    // black bars
    std::vector<float> col(100, 0.0f), row(60, 0.0f);
    for (int x = 20; x < 80; ++x) col[x] = 0.3f;
    for (int y = 0; y < 60; ++y) row[y] = 0.3f;
    int bx, by, bw, bh;
    contentRect(col, row, bx, by, bw, bh);
    check(bx == 20 && bw == 60 && by == 0 && bh == 60, "pillarbox bars are found");

    // text
    std::string font = defaultFontPath(), err;
    std::printf("       font: %s\n", font.c_str());
    TextMask m = rasterizeText("1/200 s \xC2\xB7 F8.0 \xC2\xB7 ISO 64 \xC2\xB7 35mm", font, 24, &err);
    int ink = 0;
    for (unsigned char a : m.alpha) ink += a > 128;
    std::printf("       caption mask %d x %d, %d solid px %s\n", m.w, m.h, ink, err.c_str());
    check(!font.empty() && m.w > 150 && m.w < 400 && m.h >= 24 && ink > 200, "caption renders with the system font");
    TextMask none = rasterizeText("abc", "nope.ttf", 24, &err);
    check(none.w == 0 && !err.empty(), "missing font -> empty mask and a message");

    // compositor: a 300 x 450 canvas, photo = flat 0.5 grey
    const int W = 300, H = 450;
    std::vector<float> img(size_t(W) * H * 4, 0.5f);
    auto at = [&](int x, int y) { return &img[(size_t(y) * W + x) * 4]; };
    FrameDraw d;
    TextMask cm = rasterizeText("1/200 s", font, 12, &err);
    d.layout = computeLayout(W, H, W, H, 0.85f, 0.17f, kAlignLeft, cm.w, 6);
    d.cw = W, d.ch = H;
    d.bg = frameDisplayColor(kFramePaper, {});
    d.text = autoTextColor(d.bg);
    d.mask = cm.alpha.data(), d.tw = cm.w, d.th = cm.h;
    V3 corner = framePixel(at, 0, 0, d), center = framePixel(at, W / 2, H / 2, d);
    check(std::fabs(corner.x - d.bg.x) < 1e-6f && std::fabs(center.x - 0.5f) < 1e-4f, "frame around, photo inside");
    bool dark = false;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) dark = dark || framePixel(at, x, y, d).x < 0.3f;
    check(dark, "caption pixels are drawn");

    // DI colors: paper is bright but not white, black is near black, order kept
    V3 pw = frameOutputColor(frameDisplayColor(kFrameWhite, {}), kSpaceDI, 0);
    V3 pp = frameOutputColor(frameDisplayColor(kFramePaper, {}), kSpaceDI, 0);
    V3 pb = frameOutputColor(frameDisplayColor(kFrameBlack, {}), kSpaceDI, 0);
    std::printf("       DI: white %.3f %.3f %.3f | paper %.3f %.3f %.3f | black %.3f\n", pw.x, pw.y, pw.z, pp.x,
                pp.y, pp.z, pb.y);
    check(pw.y > pp.y && pp.y > 0.4f && pb.y < 0.1f && pp.x > pp.z, "DI frame colors: white > paper (warm) > black");

    // optional preview: three frames side by side, display space
    if (argc > 1) {
        const int CW = 400, CH = 600, PW = CW * 3;
        std::vector<uint8_t> out(size_t(PW) * CH * 3);
        std::vector<float> photo(size_t(CW) * CH * 4);
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                float* p = &photo[(size_t(y) * CW + x) * 4];
                float sky = y / float(CH);
                bool tree = (x * 7 + y * 3) % 97 < 30 && y < CH * 0.55f;
                p[0] = tree ? 0.20f : 0.45f + 0.2f * sky;
                p[1] = tree ? 0.28f : 0.62f + 0.1f * sky;
                p[2] = tree ? 0.12f : 0.70f + 0.1f * sky;
                p[3] = 1;
            }
        auto pat = [&](int x, int y) { return &photo[(size_t(y) * CW + x) * 4]; };
        TextMask pm = rasterizeText("1/200 s \xC2\xB7 F8.0 \xC2\xB7 ISO 64 \xC2\xB7 35mm", font, 0.012f * CW, &err);
        for (int k = 0; k < 3; ++k) {
            FrameDraw f;
            f.layout = computeLayout(CW, CH, CW, CH, 0.85f, 0.17f, kAlignLeft, pm.w, 0.03f * CW);
            f.cw = CW, f.ch = CH;
            f.bg = frameDisplayColor(k == 0 ? kFrameWhite : k == 1 ? kFramePaper : kFrameBlack, {});
            f.text = autoTextColor(f.bg);
            f.mask = pm.alpha.data(), f.tw = pm.w, f.th = pm.h;
            for (int y = 0; y < CH; ++y)
                for (int x = 0; x < CW; ++x) {
                    V3 c = framePixel(pat, x, y, f);
                    uint8_t* o = &out[(size_t(CH - 1 - y) * PW + k * CW + x) * 3];
                    o[0] = uint8_t(std::clamp(c.x, 0.0f, 1.0f) * 255 + 0.5f);
                    o[1] = uint8_t(std::clamp(c.y, 0.0f, 1.0f) * 255 + 0.5f);
                    o[2] = uint8_t(std::clamp(c.z, 0.0f, 1.0f) * 255 + 0.5f);
                }
        }
        int rowBytes = (PW * 3 + 3) & ~3;
        uint8_t hdr[54] = {'B', 'M'};
        auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) hdr[o + i] = uint8_t(v >> (8 * i)); };
        p32(2, 54 + rowBytes * CH), p32(10, 54), p32(14, 40), p32(18, PW), p32(22, CH), p32(34, rowBytes * CH);
        hdr[26] = 1, hdr[28] = 24;
        std::ofstream bmp(argv[1], std::ios::binary);
        bmp.write(reinterpret_cast<char*>(hdr), 54);
        std::vector<uint8_t> rowBuf(rowBytes, 0);
        for (int y = CH - 1; y >= 0; --y) { // BMP: bottom row first, BGR
            for (int x = 0; x < PW; ++x)
                for (int c = 0; c < 3; ++c) rowBuf[x * 3 + c] = out[(size_t(y) * PW + x) * 3 + 2 - c];
            bmp.write(reinterpret_cast<char*>(rowBuf.data()), rowBytes);
        }
        std::printf("       wrote %s\n", argv[1]);
    }

    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
