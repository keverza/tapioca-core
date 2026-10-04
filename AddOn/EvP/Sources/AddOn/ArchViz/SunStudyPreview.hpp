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
    const int minutes = int (std::round (std::max (0.0, hours) * 60.0));
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
