#ifndef EVP_ARCHVIZ_SUNPATHCOMPASS_HPP
#define EVP_ARCHVIZ_SUNPATHCOMPASS_HPP

// ArchViz/SunPathCompass -- ground-compass caption and preview scale (#179).
// Pure display helpers: the renderer supplies the displayed result, never a
// pending period request, and scaling never enters the analysis request.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace geomsrv::archviz {

inline float ClampSunPathScale (float scale)
{
    if (!std::isfinite (scale))
        return 1.0f;
    return std::clamp (scale, 0.25f, 4.0f);
}

inline std::string SunPathAnalysisCaption (bool showsPeriod, const std::string& periodLabel, int shownDate, int runYear,
                                           int runMonth, int runDay)
{
    if (showsPeriod && !periodLabel.empty ())
        return "Analysis period: " + periodLabel;
    int year = runYear, month = runMonth, day = runDay;
    if (shownDate > 0) {
        year = shownDate / 10000;
        month = shownDate / 100 % 100;
        day = shownDate % 100;
    }
    if (year <= 0 || month < 1 || month > 12 || day < 1 || day > 31)
        return {};
    char text[96];
    std::snprintf (text, sizeof (text), "Analysis date: %04d-%02d-%02d", year, month, day);
    return text;
}

} // namespace geomsrv::archviz

#endif
