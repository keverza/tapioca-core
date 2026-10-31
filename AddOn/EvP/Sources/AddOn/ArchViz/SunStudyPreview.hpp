#ifndef EVP_ARCHVIZ_SUNSTUDYPREVIEW_HPP
#define EVP_ARCHVIZ_SUNSTUDYPREVIEW_HPP

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>

namespace geomsrv::archviz {
inline std::string SunStudyClock (double hours, bool duration = false)
{
    const int minutes = int (std::round ((std::max) (0.0, hours) * 60.0));
    char text[32] = {};
    std::snprintf (text, sizeof (text), "%d:%02d%s", minutes / 60, minutes % 60, duration ? "h" : "");
    return text;
}

// Clock-time selection is [from,to), like the study's hourFrom/hourTo inputs.
// It is a view of the measured steps, not a recalculation.
inline std::pair<uint32_t, uint32_t> SunStudyPreviewSteps (const std::vector<uint16_t>& minutes, float from, float to)
{
    const auto first = std::lower_bound (minutes.begin (), minutes.end (), std::lround (from * 60.0f));
    const auto last = std::lower_bound (minutes.begin (), minutes.end (), std::lround (to * 60.0f));
    return { uint32_t (first - minutes.begin ()), uint32_t (last - minutes.begin ()) };
}

// ---- the hours gradient's STEPPED style ---------------------------------------
//
// ⚠️ ONE CONTRACT FOR THE LEGEND AND THE TINT. kArchVizSunTintPS's SunBand and
// SunBandCentre are these, line for line, and test_sunstudypreview.cpp runs the
// real shader against them. `edges` holds count - 1 ascending hour boundaries
// the person drags on the legend; band i is [edges[i-1], edges[i]), the first
// starts at 0 and the LAST IS OPEN-ENDED. Each band is one flat colour, the
// smooth ramp's at the band's centre, the open band's centre taken against the
// legend's 10 h top. Display only: the measured hours never change.
constexpr int kSunBandsMin = 3;
constexpr int kSunBandsMax = 12;
constexpr int kSunBandsDefault = 4;
constexpr float kSunRampTop = 10.0f; // the legend's right end, read "9:00h+"
constexpr float kSunBandGap = 0.25f; // the edges' quarter-hour grid, and their least spacing
// Hours a few ulps under an edge (a 10-minute study's 3:00 h) still reach it.
constexpr float kSunBandSlack = 1.0e-3f;

// The LOW-SUN category: every hours value below the HUD's cutoff, one solid
// saturated blue on the surface and in the legend alike -- a category, not a
// wash over the warm ramp. ⚠️ kArchVizSunTintPS's SunRamp carries the same
// sRGB triple (31, 95, 209), and the D3D test reads it back.
constexpr unsigned kSunLowSunHex = 0x1F5FD1;
// The low-sun swatch and the shadow overlay are the user's to recolour; these
// are the defaults, 0xRRGGBBAA in sRGB. The overlay's alpha is how far a
// shadowed patch moves toward its colour: black at 45% darkens by 0.55.
constexpr uint32_t kSunLowSunRgba = (kSunLowSunHex << 8) | 0xffu;
constexpr uint32_t kSunShadowOverlayRgba = 0x00000073u;

// sRGB 0..1 channels of an 0xRRGGBBAA colour, alpha last.
inline void SunStudyRgbaChannels (uint32_t rgba, float out[4])
{
    for (int channel = 0; channel < 4; ++channel)
        out[channel] = float ((rgba >> (24 - 8 * channel)) & 0xffu) / 255.0f;
}

// Evenly spaced with the open band at 9 h+, so ten bands are exactly the
// smooth ramp's ten one-hour bins.
inline void SunStudyDefaultBandEdges (int count, float edges[kSunBandsMax - 1])
{
    count = std::clamp (count, kSunBandsMin, kSunBandsMax);
    for (int i = 0; i < kSunBandsMax - 1; ++i)
        edges[i] = i < count - 1 ? std::round (9.0f * float (i + 1) / float (count - 1) * 4.0f) / 4.0f : kSunRampTop;
}

// Ascending, on the quarter-hour grid, at least one gap apart, inside (0, top).
inline void SunStudyClampBandEdges (int count, float edges[kSunBandsMax - 1])
{
    count = std::clamp (count, kSunBandsMin, kSunBandsMax);
    for (int i = 0; i < count - 1; ++i) {
        const float lo = kSunBandGap * float (i + 1);
        const float hi = kSunRampTop - kSunBandGap * float (count - 1 - i);
        edges[i] = std::clamp (std::round (edges[i] * 4.0f) / 4.0f, lo, hi);
        if (i > 0)
            edges[i] = (std::max) (edges[i], edges[i - 1] + kSunBandGap);
    }
}

inline int SunStudyBand (float hours, int count, const float* edges)
{
    int band = 0;
    for (int i = 0; i < count - 1; ++i)
        if (hours + kSunBandSlack >= edges[i])
            band = i + 1;
    return band;
}

inline float SunStudyBandCentre (int band, int count, const float* edges)
{
    const float lo = band > 0 ? edges[band - 1] : 0.0f;
    const float hi = band < count - 1 ? edges[band] : kSunRampTop;
    return 0.5f * (lo + hi);
}

// Display-only intensity. Short shadows no longer classify a whole half-day.
inline float SunStudyShadowFraction (const std::vector<uint32_t>& words, uint32_t first, uint32_t last)
{
    if (first >= last)
        return 0.0f;
    uint32_t shadowed = 0;
    for (uint32_t step = first; step < last; ++step)
        if (step / 32 < words.size () && ((words[step / 32] >> (step % 32)) & 1u) == 0)
            ++shadowed;
    return float (shadowed) / float (last - first);
}
} // namespace geomsrv::archviz
#endif
