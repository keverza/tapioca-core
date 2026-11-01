#ifndef EVP_ARCHVIZ_SUNSTUDYPREVIEW_HPP
#define EVP_ARCHVIZ_SUNSTUDYPREVIEW_HPP

#include "ArchViz/GradientLibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <span>

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
// The default is ten bands, the smooth ramp's one-hour bins (#102: the owner
// reads whole hours off the legend, 0:00 to 9:00+).
constexpr int kSunBandsMin = 3;
constexpr int kSunBandsMax = 12;
constexpr int kSunBandsDefault = 10;
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

// What a band's cell on the legend reads: the hours it starts at, the sum of
// every band before it, and "+" on the open last one (#102).
inline std::string SunStudyBandLabel (int band, int count, const float* edges)
{
    const float lo = band > 0 ? edges[band - 1] : 0.0f;
    return SunStudyClock (lo) + (band == count - 1 ? "+" : "");
}

// ---- the hours gradient's COLOURS (#102) --------------------------------------
//
// ⚠️ ONE CONTRACT FOR THE LEGEND AND THE TINT, again: kArchVizSunTintPS's SunWarm
// is SunStudyRampColour line for line. The ramp's first stop is the colour at
// 0.5 h and its last at 9.5 h -- the ten hourly bins' centres -- whatever preset
// the user picks; the default is the warm sunlight preset. Display only.
constexpr const char* kSunRampDefaultPreset = "sun-hours-classic"; // the owner's default, 2026-11-01
constexpr size_t kSunRampMaxStops = overlaylayers::kMaxStops;
// The original ten hourly bins, the colours a scene draws before any HUD chose
// a preset. DiligentSceneConstants' defaults are these (test_sunstudypreview.cpp).
constexpr overlaylayers::ColourStop kSunLegacyRamp[10] = {
    { 0.0f / 9.0f, 0x6B3D18FFu }, { 1.0f / 9.0f, 0x8A4F1FFFu }, { 2.0f / 9.0f, 0x9C5A23FFu },
    { 3.0f / 9.0f, 0xB06A28FFu }, { 4.0f / 9.0f, 0xC07D33FFu }, { 5.0f / 9.0f, 0xCF8F44FFu },
    { 6.0f / 9.0f, 0xDCA157FFu }, { 7.0f / 9.0f, 0xE7B674FFu }, { 8.0f / 9.0f, 0xF0CB96FFu },
    { 9.0f / 9.0f, 0xF7E3C2FFu },
};

// sRGB 0xRRGGBBAA of `hours` along `stops` (2..16, ascending); fewer than two
// stops reads the legacy ramp.
inline uint32_t SunStudyRampColour (std::span<const overlaylayers::ColourStop> stops, float hours)
{
    if (stops.size () < 2)
        stops = kSunLegacyRamp;
    const float t = std::clamp ((hours - 0.5f) / 9.0f, 0.0f, 1.0f);
    float rgb[3] = {};
    const auto channels = [] (uint32_t rgba, float out[3]) {
        for (int c = 0; c < 3; ++c)
            out[c] = float ((rgba >> (24 - 8 * c)) & 0xffu);
    };
    channels (stops[0].rgba, rgb);
    for (size_t i = 1; i < stops.size (); ++i) {
        const float a = stops[i - 1].at, b = stops[i].at;
        if (t < a)
            continue;
        float lo[3], hi[3];
        channels (stops[i - 1].rgba, lo);
        channels (stops[i].rgba, hi);
        const float mix = b > a ? std::clamp ((t - a) / (b - a), 0.0f, 1.0f) : 1.0f;
        for (int c = 0; c < 3; ++c)
            rgb[c] = lo[c] + (hi[c] - lo[c]) * mix;
    }
    uint32_t out = 0xffu;
    for (int c = 0; c < 3; ++c)
        out |= uint32_t (std::lround (std::clamp (rgb[c], 0.0f, 255.0f))) << (24 - 8 * c);
    return out;
}

// The stops of the preset the HUD chose: `id`'s, the user's `custom` ones for
// kCustomGradientId, or the default's for an unknown id -- end for end when
// `flipped`. Never empty.
inline std::vector<overlaylayers::ColourStop>
SunStudyRampStops (const std::string& id, std::span<const overlaylayers::ColourStop> custom = {}, bool flipped = false)
{
    std::span<const overlaylayers::ColourStop> stops;
    if (id == overlaylayers::kCustomGradientId && custom.size () >= 2 && custom.size () <= kSunRampMaxStops)
        stops = custom;
    else {
        const overlaylayers::GradientPreset* preset = overlaylayers::FindGradientPreset (id);
        if (preset == nullptr)
            preset = overlaylayers::FindGradientPreset (kSunRampDefaultPreset);
        stops = preset != nullptr ? preset->Stops () : std::span<const overlaylayers::ColourStop> (kSunLegacyRamp);
    }
    return flipped ? overlaylayers::ReversedStops (stops)
                   : std::vector<overlaylayers::ColourStop> (stops.begin (), stops.end ());
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
