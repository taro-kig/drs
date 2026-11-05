// HeroMath.h - "make this color the hero of the look" for Color Look (no OFX dependency)
//
// The user only says which colors should lead and how much (7 sliders, 0..1). Everything
// else is decided here from the frame:
//   - the hero hue leans the whole picture: warm heroes go into the highlights, cool ones
//     into the shadows, and a complementary color takes the other end (orange -> teal
//     shadows, cyan -> a little amber in the highlights)
//   - the less of the hero color the frame already has, the more the picture leans to it;
//     the more complement-colored the frame is (sky, foliage...), the more of the
//     complement it gets
//   - the hero band itself gets cleaner and richer, but no hue moves: on pixels that are
//     already colorful the tints only push along the pixel's own hue (richer or a little
//     quieter), so wood, skin and leaves keep their own colors
// Working space: DaVinci Wide Gamut / DaVinci Intermediate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ColorMath.h"
#include "LookMath.h"
#include "MixerMath.h"

namespace drs {

// --- frame colour histogram (Oklab hue x chroma x lightness) ---
// Kept small and integer-only so the GPU can fill it with atomic_inc. The OpenCL kernel
// `colorHist` in ClLut.cpp mirrors colorBin() exactly.
constexpr int kHueBins = 36, kChromaBins = 8, kLightBins = 8;
constexpr int kColorBins = kHueBins * kChromaBins * kLightBins;
constexpr float kChromaBinWidth = 0.03f;

inline int colorBin(V3 di)
{
    V3 lab = xyzToOklab(mul(kDwgToXyz, {diToLin(di.x), diToLin(di.y), diToLin(di.z)}));
    if (!(lab.x == lab.x)) return -1; // NaN
    float C = std::sqrt(lab.y * lab.y + lab.z * lab.z);
    float h = std::atan2(lab.z, lab.y) * 57.29578f;
    if (h < 0.0f) h += 360.0f;
    int hb = std::clamp(int(h / 10.0f), 0, kHueBins - 1);
    int cb = std::clamp(int(C / kChromaBinWidth), 0, kChromaBins - 1);
    int lb = std::clamp(int(lab.x * kLightBins), 0, kLightBins - 1);
    return (hb * kChromaBins + cb) * kLightBins + lb;
}

// What the frame looks like, per Color Mixer band.
struct ColorStats {
    float coverage[kMixColors] = {}; // share of the frame that clearly is this color
    float neutral = 1.0f;            // share of the frame that is grey-ish
    bool valid = false;
};

inline ColorStats statsFromColorHist(const std::vector<uint32_t>& hist)
{
    ColorStats st;
    if (int(hist.size()) != kColorBins) return st;
    double total = 0, neutral = 0, cov[kMixColors] = {};
    for (int hb = 0; hb < kHueBins; ++hb) {
        float w[kMixColors];
        mixWeights(hb * 10.0f + 5.0f, w);
        for (int cb = 0; cb < kChromaBins; ++cb) {
            float C = (cb + 0.5f) * kChromaBinWidth;
            float colored = smoothstep(0.02f, 0.07f, C);
            for (int lb = 0; lb < kLightBins; ++lb) {
                double n = hist[size_t((hb * kChromaBins + cb) * kLightBins + lb)];
                if (n == 0) continue;
                float L = (lb + 0.5f) / kLightBins;
                total += n;
                neutral += n * (1.0f - colored);
                double g = n * colored * smoothstep(0.1f, 0.25f, L); // near-black hues don't count
                for (int i = 0; i < kMixColors; ++i) cov[i] += g * w[i];
            }
        }
    }
    if (total <= 0) return st;
    for (int i = 0; i < kMixColors; ++i) st.coverage[i] = float(cov[i] / total);
    st.neutral = float(neutral / total);
    st.valid = true;
    return st;
}

// share of the frame near Oklab hue `hdeg` (band coverages blended like the mixer does)
inline float coverageAt(const ColorStats& st, float hdeg)
{
    float w[kMixColors];
    mixWeights(hdeg, w);
    float c = 0;
    for (int i = 0; i < kMixColors; ++i) c += w[i] * st.coverage[i];
    return c;
}

// --- hero colours -> look ---

constexpr float kDeg = 0.01745329f;

// warm (+1, orange) .. cool (-1, blue)
inline float warmth(float hdeg) { return std::cos((hdeg - 60.0f) * kDeg); }

// Complement of a hue, pulled up to 30 degrees toward the film axis (amber 55 / teal 215),
// so orange gets teal rather than blue and cyan gets amber rather than red.
inline float complementHue(float hdeg)
{
    float c = std::fmod(hdeg + 180.0f, 360.0f);
    auto diff = [](float a, float b) { // signed b - a in -180..180
        float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
        return d;
    };
    float dA = diff(c, 55.0f), dB = diff(c, 215.0f);
    float d = std::fabs(dA) < std::fabs(dB) ? dA : dB;
    c += std::clamp(d, -30.0f, 30.0f);
    return std::fmod(c + 360.0f, 360.0f);
}

struct HeroPlan {
    bool on = false;
    float strength = 0;      // largest slider value
    float hue = 0;           // Oklab hue of the hero, degrees
    float complement = 0;    // Oklab hue of its partner
    float heroHigh = 0.5f;   // 1 = hero in the highlights, 0 = in the shadows
    float castAmt = 0, compAmt = 0, coherence = 1;
    float colorful = 1;      // 1..2: frames with little grey get a stronger lean (tints have less room)
    bool paired = false;     // two far-apart heroes: the second one takes the complement's place
};

inline float hueGapDeg(float a, float b) { return std::fabs(std::fmod(a - b + 540.0f, 360.0f) - 180.0f); }

// strength-weighted circular mean of the heroes in `use`; returns false if none
inline bool heroGroup(const float hero[kMixColors], const bool use[kMixColors], float& hue, float& strength,
                      float& coherence)
{
    float sum = 0, x = 0, y = 0;
    strength = 0;
    for (int i = 0; i < kMixColors; ++i) {
        float s = use[i] ? std::clamp(hero[i], 0.0f, 1.0f) : 0.0f;
        if (s <= 0.0f) continue;
        strength = std::max(strength, s);
        sum += s;
        x += s * std::cos(kMixCenters[i] * kDeg);
        y += s * std::sin(kMixCenters[i] * kDeg);
    }
    if (sum <= 0.0f) return false;
    hue = std::atan2(y, x) / kDeg;
    if (hue < 0.0f) hue += 360.0f;
    coherence = std::clamp(std::sqrt(x * x + y * y) / sum, 0.0f, 1.0f);
    return true;
}

// Heroes close to the strongest one (within 75 degrees: Red + Orange, Blue + Magenta) blend
// into one hero. Heroes further away form a second group that takes the complement's place,
// so Orange + Aqua is teal and orange with both colors as chosen, not an average (green).
inline HeroPlan planHero(const float hero[kMixColors], const ColorStats& st)
{
    HeroPlan p;
    int top = -1;
    for (int i = 0; i < kMixColors; ++i)
        if (hero[i] > 0.0f && (top < 0 || hero[i] > hero[top])) top = i;
    if (top < 0) return p;
    bool near[kMixColors], far[kMixColors];
    for (int i = 0; i < kMixColors; ++i) {
        near[i] = hueGapDeg(kMixCenters[i], kMixCenters[top]) <= 75.0f;
        far[i] = !near[i];
    }
    p.on = heroGroup(hero, near, p.hue, p.strength, p.coherence);
    float farHue = 0, farStrength = 0, farCoh = 1;
    p.paired = heroGroup(hero, far, farHue, farStrength, farCoh);

    float neutral = st.valid ? st.neutral : 0.5f;
    p.colorful = 1.0f + (1.0f - std::clamp(neutral / 0.5f, 0.0f, 1.0f));
    float heroCov = st.valid ? coverageAt(st, p.hue) : 0.05f;
    p.castAmt = 0.5f + 0.5f * (1.0f - std::clamp(heroCov / 0.15f, 0.0f, 1.0f));

    if (p.paired) {
        // the user picked the partner: it goes where the complement would, at its own strength,
        // and the warmer of the two takes the highlights
        p.complement = farHue;
        p.compAmt = std::clamp(farStrength / p.strength, 0.0f, 1.0f) * farCoh;
        p.heroHigh = smoothstep(-0.3f, 0.3f, warmth(p.hue) - warmth(farHue));
        return p;
    }
    p.complement = complementHue(p.hue);
    p.heroHigh = smoothstep(-0.4f, 0.4f, warmth(p.hue));
    float compCov = st.valid ? coverageAt(st, p.complement) : 0.05f;
    p.compAmt = 0.6f + 0.4f * std::clamp(compCov / 0.10f, 0.0f, 1.0f);
    return p;
}

// Adds the hero part to a look (dims), a mixer (band moves) and, after prepare(), to the
// tints of the LookParams. `amount` is the plugin's Amount.
inline void heroLookDims(const HeroPlan& p, Look& l)
{
    if (!p.on) return;
    float S = p.strength;
    l.p[kFilmSat] += 0.2f * S;
    l.p[kDye] += 0.25f * S;
    l.p[kContrast] += 0.15f * S;
    l.p[kHueGreen] += 0.15f * S * std::max(warmth(p.hue), 0.0f); // warm heroes: greens lean a touch teal
}

inline void heroMixer(const HeroPlan& p, const float hero[kMixColors], const ColorStats& st, MixerSettings& m)
{
    if (!p.on) return;
    for (int i = 0; i < kMixColors; ++i) {
        float s = std::clamp(hero[i], 0.0f, 1.0f);
        if (s <= 0.0f) continue;
        float cov = st.valid ? st.coverage[i] : 0.05f;
        m.sat[i] += 0.3f * s * (1.0f - 0.5f * std::clamp(cov / 0.3f, 0.0f, 1.0f)); // already dominant: go easy
        m.lum[i] += 0.12f * s * warmth(kMixCenters[i]); // warm glows, cool deepens
    }
    float w[kMixColors];
    mixWeights(p.complement, w);
    // the complement pops a little with warm heroes (teal against orange); with a cool hero the
    // complement is amber, and pushing it would pull the picture away from the hero
    float pop = p.paired ? 0.0f : 0.15f * p.compAmt * p.strength * p.coherence * p.heroHigh;
    for (int i = 0; i < kMixColors; ++i) m.sat[i] += pop * w[i];
}

inline void heroTints(const HeroPlan& p, float amount, LookParams& k)
{
    if (!p.on) return;
    float a = std::clamp(amount, 0.0f, 1.5f) * p.strength * p.coherence;
    float hx = std::cos(p.hue * kDeg), hy = std::sin(p.hue * kDeg);
    float cx = std::cos(p.complement * kDeg), cy = std::sin(p.complement * kDeg);
    // cool heroes keep their warm complement light: the picture should still read as the hero
    float heroT = 0.03f * a, compT = 0.034f * a * p.compAmt * (p.paired ? 1.0f : 0.5f + 0.5f * p.heroHigh), castT = 0.02f * a * p.castAmt * p.colorful;
    float hiHero = heroT * p.heroHigh, shHero = heroT * (1.0f - p.heroHigh);
    float hiComp = compT * (1.0f - p.heroHigh), shComp = compT * p.heroHigh;
    k.hiA += hiHero * hx + hiComp * cx;
    k.hiB += hiHero * hy + hiComp * cy;
    k.shA += shHero * hx + shComp * cx;
    k.shB += shHero * hy + shComp * cy;
    k.castA += castT * hx;
    k.castB += castT * hy;
    // colorful pixels keep their hue but still lean (see applyLook), so skin and wood stay
    // themselves and still belong to the look
    k.tintProtect = std::max(k.tintProtect, 0.7f);
}

} // namespace drs
