#ifndef TAPIOCA_ARCHVIZ_SUNPATHMOMENT_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHMOMENT_HPP

#include "SunStudy/SunPathStudyData.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::archviz {

struct SunPathMoment {
    bool valid = false;
    int year = 0, month = 0, day = 0, minute = -1;
    int step = -1;
    float direction[3] = {};
};

inline SunPathMoment SunPathStepMoment (int year, int month, int day, const std::vector<evp::sunstudy::SunStep>& steps,
                                        int index)
{
    SunPathMoment result;
    if (year <= 0 || month < 1 || month > 12 || day < 1 || day > 31 || index < 0 || size_t (index) >= steps.size ())
        return result;
    const auto& step = steps[size_t (index)];
    if (step.time.hour < 0 || step.time.hour > 23 || step.time.minute < 0 || step.time.minute > 59 ||
        !std::isfinite (step.direction[0]) || !std::isfinite (step.direction[1]) ||
        !std::isfinite (step.direction[2]) || step.direction[2] <= 0.0)
        return result;
    result.valid = true;
    result.year = year;
    result.month = month;
    result.day = day;
    result.minute = step.time.hour * 60 + step.time.minute;
    result.step = index;
    for (int axis = 0; axis < 3; ++axis)
        result.direction[axis] = float (step.direction[axis]);
    return result;
}

// Source indices are retained even for unsorted/duplicate clocks. A tie uses
// the earlier clock, then the earlier source index, deterministically.
inline SunPathMoment SunPathNearestMoment (int year, int month, int day,
                                           const std::vector<evp::sunstudy::SunStep>& steps, int minute, int from = 0,
                                           int to = 1440)
{
    SunPathMoment result;
    int bestGap = (std::numeric_limits<int>::max) ();
    for (size_t i = 0; i < steps.size (); ++i) {
        const auto candidate = SunPathStepMoment (year, month, day, steps, int (i));
        if (!candidate.valid || candidate.minute < from || candidate.minute >= to)
            continue;
        const int gap = std::abs (candidate.minute - std::clamp (minute, 0, 1439));
        if (gap < bestGap || (gap == bestGap && candidate.minute < result.minute)) {
            bestGap = gap;
            result = candidate;
        }
    }
    return result;
}

inline SunPathMoment SunPathAppliedMoment (const sunstudyhud::SunPathPeriodData& period)
{
    if (period.geometry == nullptr || period.applied.frame != 1 || period.applied.day < 0 ||
        size_t (period.applied.day) >= period.geometry->days.size ())
        return {};
    const auto& day = period.geometry->days[size_t (period.applied.day)];
    return SunPathStepMoment (period.geometry->year, day.month, day.day, day.steps, period.applied.step);
}

inline SunPathMoment SunPathMonthMoment (const sunstudyhud::SunPathPeriodGeometry& period, int month, int minute,
                                         int from = 0, int to = 1440)
{
    SunPathMoment result;
    int bestGap = (std::numeric_limits<int>::max) ();
    for (const auto& day : period.days) {
        if (day.month != month)
            continue;
        const auto candidate = SunPathNearestMoment (period.year, day.month, day.day, day.steps, minute, from, to);
        if (!candidate.valid)
            continue;
        const int gap = std::abs (candidate.minute - std::clamp (minute, 0, 1439));
        if (gap < bestGap || (gap == bestGap && candidate.day < result.day)) {
            bestGap = gap;
            result = candidate;
        }
    }
    return result;
}

enum class SunPathSyncSource { None, MapPlayer, Shadow, PointPlayer };

struct SunPathSelection {
    SunPathMoment hover, map, shadow, point, day;
    bool hoverCurrent = false;
    bool mapActive = false, shadowActive = false, pointActive = false;
    bool aggregate = false;
    SunPathSyncSource source = SunPathSyncSource::None;
};

inline SunPathMoment ResolveSunPathMoment (const SunPathSelection& selection)
{
    if (selection.hoverCurrent && selection.hover.valid)
        return selection.hover;
    if (selection.source == SunPathSyncSource::PointPlayer && selection.pointActive)
        return selection.point;
    if (selection.source == SunPathSyncSource::Shadow && selection.shadowActive)
        return selection.shadow;
    if (selection.source == SunPathSyncSource::MapPlayer && selection.mapActive)
        return selection.map;
    if (selection.shadowActive)
        return selection.shadow;
    if (selection.mapActive || selection.aggregate)
        return selection.map;
    return selection.day;
}

} // namespace geomsrv::archviz

#endif
