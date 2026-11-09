#ifndef EVP_ARCHVIZ_SUNPATHCOMPASS_HPP
#define EVP_ARCHVIZ_SUNPATHCOMPASS_HPP

// ArchViz/SunPathCompass -- scene placement, compass orientation and hover label (#179).
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

inline bool SunPathSceneDome (const double lo[3], const double hi[3], float scale, double centre[3], double& radius)
{
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite (lo[axis]) || !std::isfinite (hi[axis]) || lo[axis] > hi[axis])
            return false;
        centre[axis] = 0.5 * lo[axis] + 0.5 * hi[axis];
    }
    centre[2] = lo[2];
    radius = (std::max) (10.0, 0.6 * std::hypot (hi[0] - lo[0], hi[1] - lo[1])) * ClampSunPathScale (scale);
    return std::isfinite (radius);
}

// Use the projected circle's tangent, not a perpendicular to its projected
// radius (those differ in oblique views). No upright flip as the camera orbits.
inline double SunPathCompassAngle (double tangentX, double tangentY)
{
    return std::atan2 (tangentY, tangentX);
}

inline std::string SunPathMomentCaption (int year, int month, int day, int minute)
{
    if (year <= 0 || month < 1 || month > 12 || day < 1 || day > 31 || minute < 0 || minute >= 1440)
        return {};
    char text[96];
    std::snprintf (text, sizeof (text), "Sun: %04d-%02d-%02d  %02d:%02d", year, month, day, minute / 60, minute % 60);
    return text;
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
