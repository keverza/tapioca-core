#pragma once
#include "ArchViz/LayoutReportPage.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include <cstdio>
#include <cmath>

namespace geomsrv::archviz {
inline void ApplyReportPalette (ReportTemplate type, SunStudyViewSettings& v)
{
    v.viewOverride = type == ReportTemplate::Patterns ? 6 : type == ReportTemplate::Analysis ? 5 : 0;
    v.singleLitRgba = 0xeeeeeeff;
    v.singleShadowedRgba = 0x545454ff;
    v.amPmSunlitRgba = 0xeeeeeeff;
    v.amPmMorningRgba = 0x796ab1ff;
    v.amPmEveningRgba = 0xdf9792ff;
    v.rampCount = 2;
    v.ramp[0] = { 0, 0x6b371aff };
    v.ramp[1] = { 1, 0xffe08aff };
    v.firstShadowStep = 0;
    v.endShadowStep = UINT32_MAX;
    v.bandCount = 0;
    v.lo = 0;
    v.hi = kSunHoursFilterOpenTop;
    v.blueThreshold = -1;
    v.shadowOverlay = false;
    v.hide = false;
}
template <class Texel>
std::string SeasonalMetric (const SunStudyAtlasUpload& p, const std::vector<double>& areas, Texel texel,
                            ReportTemplate type)
{
    const size_t plane = size_t (p.width) * p.height;
    if (!plane || !p.stepCount || !p.stepMasks || p.stepMasks->size () / plane < (p.stepCount + 31) / 32 || !p.texels ||
        p.texels->size () != plane)
        return "No data";
    double total = 0, am = 0, pm = 0, hours = 0, missing = 0;
    for (size_t i = 0; i < areas.size (); ++i) {
        const double a = areas[i];
        if (!std::isfinite (a) || a < 0)
            return "Invalid area";
        size_t t = size_t (texel (i));
        if (t >= plane || !std::isfinite ((*p.texels)[t]) || (*p.texels)[t] < 0) {
            missing += a;
            continue;
        }
        total += a;
        hours += a * (*p.texels)[t];
        if (type == ReportTemplate::Patterns) {
            unsigned cat = 0;
            for (unsigned k = 0; k < p.stepCount && cat != 3; ++k)
                if (!((*p.stepMasks)[plane * (k / 32) + t] & (1u << (k % 32))))
                    cat |= k < p.noonStep ? 1 : 2;
            if (cat == 1)
                am += a;
            if (cat == 2)
                pm += a;
        }
    }
    if (!total)
        return "No measured area";
    char s[128];
    if (type == ReportTemplate::Patterns)
        std::snprintf (s, sizeof (s), "%.1f%% AM only     %.1f%% PM only", 100 * am / total, 100 * pm / total);
    else
        std::snprintf (s, sizeof (s), "%.2f h mean direct sunlight", hours / total);
    std::string out = s;
    if (missing) {
        std::snprintf (s, sizeof (s), "  |  %.1f m2 missing", missing);
        out += s;
    }
    return out;
}
} // namespace geomsrv::archviz
