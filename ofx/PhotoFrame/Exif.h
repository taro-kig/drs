// Exif.h - minimal EXIF reader for the caption (no OFX dependency, tested alone)
//
// Reads the TIFF-structured EXIF found in raw files (ARW, DNG, NEF, CR2, ORF, RW2...) and
// in JPEG (APP1 "Exif"). Only the few tags a caption needs: exposure time, f-number, ISO,
// focal length, camera, lens and date. Reads by seeking, so huge raw files are fine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace drs {

struct ExifInfo {
    bool ok = false;
    double exposure = 0;  // seconds
    double fnumber = 0;
    int iso = 0;
    double focal = 0;     // mm
    int focal35 = 0;      // mm, 35 mm equivalent
    std::string make, model, lens, date;
};

namespace exif_detail {

class Reader {
public:
    explicit Reader(const std::string& path) : m_F(path, std::ios::binary) {}
    bool ok() const { return bool(m_F); }

    bool read(uint64_t pos, void* dst, size_t n)
    {
        m_F.clear();
        m_F.seekg(std::streamoff(pos));
        m_F.read(static_cast<char*>(dst), std::streamsize(n));
        return size_t(m_F.gcount()) == n;
    }
    uint16_t u16(uint64_t pos, bool le)
    {
        uint8_t b[2] = {};
        read(pos, b, 2);
        return le ? uint16_t(b[0] | b[1] << 8) : uint16_t(b[0] << 8 | b[1]);
    }
    uint32_t u32(uint64_t pos, bool le)
    {
        uint8_t b[4] = {};
        read(pos, b, 4);
        return le ? uint32_t(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24)
                  : uint32_t(uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    }

private:
    std::ifstream m_F;
};

struct Tiff {
    Reader& r;
    uint64_t base; // offset of the TIFF header in the file
    bool le;

    // value location of an entry: inline if it fits in 4 bytes
    uint64_t valuePos(uint64_t entry, uint16_t type, uint32_t count)
    {
        static const int sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        size_t sz = (type < 13 ? sizes[type] : 1) * size_t(count);
        return sz <= 4 ? entry + 8 : base + r.u32(entry + 8, le);
    }
    std::string ascii(uint64_t entry, uint32_t count)
    {
        count = std::min<uint32_t>(count, 256);
        std::string s(count, '\0');
        if (count) r.read(valuePos(entry, 2, count), &s[0], count);
        while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
        return s;
    }
    double rational(uint64_t entry, uint16_t type)
    {
        uint64_t p = valuePos(entry, type, 1);
        uint32_t n = r.u32(p, le), d = r.u32(p + 4, le);
        if (type == 10) return d ? double(int32_t(n)) / double(int32_t(d)) : 0.0;
        return d ? double(n) / double(d) : 0.0;
    }
    uint32_t integer(uint64_t entry, uint16_t type)
    {
        uint64_t p = valuePos(entry, type, 1);
        return type == 3 ? r.u16(p, le) : r.u32(p, le);
    }

    // walks one IFD; returns the Exif sub-IFD offset if found (0 otherwise)
    uint32_t walk(uint32_t ifd, ExifInfo& info, bool isExif)
    {
        if (ifd == 0) return 0;
        uint16_t n = r.u16(base + ifd, le);
        if (n == 0 || n > 512) return 0;
        uint32_t exifPtr = 0;
        for (uint16_t i = 0; i < n; ++i) {
            uint64_t e = base + ifd + 2 + 12ull * i;
            uint16_t tag = r.u16(e, le), type = r.u16(e + 2, le);
            uint32_t count = r.u32(e + 4, le);
            switch (tag) {
            case 0x010F: if (info.make.empty()) info.make = ascii(e, count); break;
            case 0x0110: if (info.model.empty()) info.model = ascii(e, count); break;
            case 0x8769: exifPtr = integer(e, type); break;
            case 0x829A: if (type == 5 || type == 10) { info.exposure = rational(e, type); info.ok = true; } break;
            case 0x829D: if (type == 5 || type == 10) { info.fnumber = rational(e, type); info.ok = true; } break;
            case 0x8827: if (!info.iso) { info.iso = int(integer(e, type)); info.ok = true; } break;
            case 0x8833: if (type == 4 && count == 1) info.iso = int(integer(e, type)); break; // ISOSpeed (exact, > 65535)
            case 0x920A: if (type == 5) { info.focal = rational(e, type); info.ok = true; } break;
            case 0xA405: info.focal35 = int(integer(e, type)); break;
            case 0xA434: info.lens = ascii(e, count); break;
            case 0x9003: if (isExif) info.date = ascii(e, count); break;
            default: break;
            }
        }
        return exifPtr;
    }
};

inline bool parseTiff(Reader& r, uint64_t base, ExifInfo& info)
{
    uint8_t h[4] = {};
    if (!r.read(base, h, 4)) return false;
    bool le;
    if (h[0] == 'I' && h[1] == 'I') le = true;
    else if (h[0] == 'M' && h[1] == 'M') le = false;
    else return false;
    Tiff t{r, base, le};
    // ORF ("IIRO") and RW2 ("IIU") use other magic numbers but the same layout
    uint32_t ifd0 = r.u32(base + 4, le);
    uint32_t exif = t.walk(ifd0, info, false);
    t.walk(exif, info, true);
    return info.ok;
}

} // namespace exif_detail

inline ExifInfo readExif(const std::string& path)
{
    using namespace exif_detail;
    ExifInfo info;
    Reader r(path);
    if (!r.ok()) return info;
    uint8_t m[2] = {};
    if (!r.read(0, m, 2)) return info;
    if (m[0] == 0xFF && m[1] == 0xD8) { // JPEG: look for APP1 "Exif"
        uint64_t pos = 2;
        for (int guard = 0; guard < 64; ++guard) {
            uint8_t seg[4] = {};
            if (!r.read(pos, seg, 4) || seg[0] != 0xFF) break;
            uint16_t len = uint16_t(seg[2] << 8 | seg[3]);
            if (seg[1] == 0xE1) {
                char id[6] = {};
                r.read(pos + 4, id, 6);
                if (std::string(id, 4) == "Exif") {
                    parseTiff(r, pos + 10, info);
                    break;
                }
            }
            if (seg[1] == 0xDA) break; // start of image data
            pos += 2 + len;
        }
        return info;
    }
    parseTiff(r, 0, info); // raw files are TIFF based
    return info;
}

// --- caption text ---

inline std::string fmtShutter(double t)
{
    if (t <= 0) return "";
    char b[32];
    if (t < 0.99) std::snprintf(b, sizeof(b), "1/%d s", int(std::lround(1.0 / t)));
    else if (std::fabs(t - std::round(t)) < 0.01) std::snprintf(b, sizeof(b), "%d s", int(std::lround(t)));
    else std::snprintf(b, sizeof(b), "%.1f s", t);
    return b;
}

inline std::string fmtAperture(double f)
{
    if (f <= 0) return "";
    char b[16];
    std::snprintf(b, sizeof(b), "F%.1f", f);
    return b;
}

inline std::string fmtIso(int iso) { return iso > 0 ? "ISO " + std::to_string(iso) : ""; }

inline std::string fmtFocal(double mm)
{
    if (mm <= 0) return "";
    char b[16];
    if (std::fabs(mm - std::round(mm)) < 0.05) std::snprintf(b, sizeof(b), "%dmm", int(std::lround(mm)));
    else std::snprintf(b, sizeof(b), "%.1fmm", mm);
    return b;
}

inline std::string fmtCamera(const ExifInfo& e)
{
    // "SONY" + "ILCE-7RM5" -> "SONY ILCE-7RM5", but don't repeat a make the model already has
    if (e.model.empty()) return e.make;
    if (e.make.empty() || e.model.find(e.make) == 0) return e.model;
    return e.make + " " + e.model;
}

// "1/200 s · F8.0 · ISO 64 · 35mm", skipping missing parts (UTF-8 middle dot)
inline std::string joinParts(const std::vector<std::string>& parts, const std::string& sep = " \xC2\xB7 ")
{
    std::string out;
    for (const std::string& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}

// Fills {exposure} {shutter} {aperture} {iso} {focal} {camera} {lens} {date} in a template.
inline std::string fillCaption(const std::string& tmpl, const std::string& shutter, const std::string& aperture,
                               const std::string& iso, const std::string& focal, const std::string& camera,
                               const std::string& lens, const std::string& date)
{
    struct Tok {
        const char* key;
        std::string val;
    };
    const Tok toks[] = {{"{exposure}", joinParts({shutter, aperture, iso, focal})},
                        {"{shutter}", shutter}, {"{aperture}", aperture}, {"{iso}", iso}, {"{focal}", focal},
                        {"{camera}", camera},   {"{lens}", lens},         {"{date}", date}};
    std::string s = tmpl;
    for (const Tok& t : toks) {
        size_t p;
        while ((p = s.find(t.key)) != std::string::npos) s.replace(p, std::strlen(t.key), t.val);
    }
    return s;
}

} // namespace drs
