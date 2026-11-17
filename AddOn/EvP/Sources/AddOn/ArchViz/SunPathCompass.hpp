#ifndef EVP_ARCHVIZ_SUNPATHCOMPASS_HPP
#define EVP_ARCHVIZ_SUNPATHCOMPASS_HPP

// ArchViz/SunPathCompass -- scene placement, compass orientation and hover label (#179),
// and the screenshot legend's location and period text (#156).
// Pure display helpers: the renderer supplies the displayed result, never a
// pending period request, and scaling never enters the analysis request.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

// The sun's hover tooltip is deliberately only the sun's time and the shown
// analysis date/period. Band/LOD diagnostics belong to the panel status, not
// to the object the user is pointing at.
inline std::string SunPathHoverCaption (const std::string& moment, const std::string& analysis)
{
    if (moment.empty ())
        return analysis;
    return analysis.empty () ? moment : moment + "\n" + analysis;
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

// ---- the screenshot legend's text columns (#156) ----------------------------
// The strip under a screenshot is three columns: Location, Analysis period and
// the colour legend. These build the first two, one string per line, so the
// image says where and when without the panel beside it.

// Where: the place Archicad's sun came from and the project north. `known`
// false (no SetEnvironment ever arrived) says so instead of printing 0, 0.
// `northDegrees` is model space, counterclockwise from +X (DiligentSceneStats).
inline std::vector<std::string> SunStudyCaptureLocationLines (bool known, float latitude, float longitude,
                                                              float altitudeMetres, float northDegrees)
{
    if (!known || !std::isfinite (latitude) || !std::isfinite (longitude))
        return { "Not received from Archicad" };
    std::vector<std::string> lines;
    char text[96];
    std::snprintf (text, sizeof (text), "%.4f %c, %.4f %c", std::abs (latitude), latitude < 0.0f ? 'S' : 'N',
                   std::abs (longitude), longitude < 0.0f ? 'W' : 'E');
    lines.push_back (text);
    if (std::isfinite (altitudeMetres)) {
        std::snprintf (text, sizeof (text), "Altitude %.0f m", altitudeMetres);
        lines.push_back (text);
    }
    if (std::isfinite (northDegrees)) {
        std::snprintf (text, sizeof (text), "North %.1f deg CCW from +X", northDegrees);
        lines.push_back (text);
    }
    return lines;
}

// When: the period (or the one day), the traced clock span and step, and --
// for a period whose image shows shadows -- the one day those shadows are of,
// so a single-day frame is never read as the period's.
struct SunStudyCapturePeriod {
    bool showsPeriod = false;
    std::string periodLabel;
    bool estimate = false;
    std::string carrierDate;
    int shownDate = 0; // yyyymmdd of the shown single-day study, 0 unknown
    int runYear = 0, runMonth = 0, runDay = 0;
    int firstMinute = -1, lastMinute = -1; // traced steps' clock span, minutes after midnight
    int stepMinutes = 0;
    bool shadowsOfOneDay = false; // the image's colours are one day's shadows
};

inline std::vector<std::string> SunStudyCapturePeriodLines (const SunStudyCapturePeriod& period)
{
    std::vector<std::string> lines;
    if (period.showsPeriod && !period.periodLabel.empty ())
        lines.push_back (period.periodLabel + (period.estimate ? " (estimate)" : ""));
    else {
        const std::string caption =
            SunPathAnalysisCaption (false, "", period.shownDate, period.runYear, period.runMonth, period.runDay);
        const std::string prefix = "Analysis date: ";
        lines.push_back (caption.empty () ? std::string ("Date unknown")
                                          : caption.substr (prefix.size ()) + ", one day");
    }
    char text[96];
    if (period.firstMinute >= 0 && period.lastMinute >= period.firstMinute && period.lastMinute < 1440) {
        const int n = std::snprintf (text, sizeof (text), "Traced %02d:%02d-%02d:%02d", period.firstMinute / 60,
                                     period.firstMinute % 60, period.lastMinute / 60, period.lastMinute % 60);
        if (period.stepMinutes > 0 && n > 0 && size_t (n) < sizeof (text))
            std::snprintf (text + n, sizeof (text) - size_t (n), ", every %d min", period.stepMinutes);
        lines.push_back (text);
    }
    if (period.showsPeriod && period.shadowsOfOneDay)
        lines.push_back ("Shadows of " +
                         (period.carrierDate.empty () ? std::string ("one traced day") : period.carrierDate) + " only");
    return lines;
}

} // namespace geomsrv::archviz

#endif
