#ifndef TAPIOCA_ARCHVIZ_SUNPATHBAND_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHBAND_HPP

#include "ArchViz/SunPathMoment.hpp"

#include <numeric>

namespace geomsrv::archviz {

struct SunPathBandDay {
    int year = 0, month = 0, day = 0;
    double weightDays = 1.0;
    bool discrete = false;
    std::vector<std::vector<SunPathMoment>> runs;
};

inline SunPathBandDay BuildSunPathBandDay (int year, int month, int day, double weightDays, int cadence,
                                           const std::vector<evp::sunstudy::SunStep>& steps, int from = 0,
                                           int to = 1440)
{
    SunPathBandDay result;
    result.year = year;
    result.month = month;
    result.day = day;
    result.weightDays = weightDays;
    result.discrete = cadence >= 30;
    std::vector<int> order (steps.size ());
    std::iota (order.begin (), order.end (), 0);
    std::stable_sort (order.begin (), order.end (), [&] (int a, int b) {
        const auto& x = steps[size_t (a)].time;
        const auto& y = steps[size_t (b)].time;
        return x.hour != y.hour ? x.hour < y.hour : x.minute < y.minute;
    });
    int previous = -1;
    bool gap = true;
    for (const int index : order) {
        const auto moment = SunPathStepMoment (year, month, day, steps, index);
        if (!moment.valid || moment.minute < from || moment.minute >= to) {
            gap = true;
            continue;
        }
        if (moment.minute == previous)
            continue;
        if (gap || previous < 0 || cadence <= 0 || moment.minute - previous > cadence)
            result.runs.emplace_back ();
        result.runs.back ().push_back (moment);
        previous = moment.minute;
        gap = false;
    }
    return result;
}

struct SunPathBand {
    std::vector<SunPathBandDay> days;
    size_t steps = 0;
};

inline SunPathBand BuildSunPathBand (const sunstudyhud::SunPathStudyDay& day)
{
    SunPathBand result;
    if (day.available) {
        result.days.push_back (BuildSunPathBandDay (day.year, day.month, day.day, 1.0, day.timestepMinutes, day.steps,
                                                    day.hourFrom * 60, day.hourTo * 60));
        for (const auto& run : result.days.back ().runs)
            result.steps += run.size ();
    }
    return result;
}

inline SunPathBand BuildSunPathBand (const sunstudyhud::SunPathPeriodGeometry& period)
{
    SunPathBand result;
    for (const auto& day : period.days) {
        const int cadence = std::isfinite (day.hoursPerStep) && day.hoursPerStep > 0.0 && day.hoursPerStep <= 24.0
                                ? int (std::round (day.hoursPerStep * 60.0))
                                : 0;
        result.days.push_back (
            BuildSunPathBandDay (period.year, day.month, day.day, day.weightDays, cadence, day.steps));
        for (const auto& run : result.days.back ().runs)
            result.steps += run.size ();
    }
    return result;
}

// Screen-space LOD bounds work per continuous run, not across day/horizon
// breaks. Source data remains exact. The tooltip must disclose any decimation.
inline std::vector<size_t> SunPathDisplayIndices (size_t count, size_t selected, size_t limit = 144)
{
    std::vector<size_t> result;
    if (count == 0)
        return result;
    const size_t stride =
        (std::max) (size_t (1), (count + (std::max) (limit, size_t (2)) - 1) / (std::max) (limit, size_t (2)));
    for (size_t i = 0; i < count; i += stride)
        result.push_back (i);
    result.push_back (count - 1);
    if (selected < count)
        result.push_back (selected);
    std::sort (result.begin (), result.end ());
    result.erase (std::unique (result.begin (), result.end ()), result.end ());
    return result;
}

inline std::vector<size_t> SunPathRunDisplayIndices (const std::vector<SunPathMoment>& run, size_t selected,
                                                     double from, double to)
{
    auto indices = SunPathDisplayIndices (run.size (), selected);
    for (const double edge : { from, to }) {
        const auto next =
            std::lower_bound (run.begin (), run.end (), edge,
                              [] (const SunPathMoment& point, double minute) { return point.minute < minute; });
        if (next != run.end ())
            indices.push_back (size_t (next - run.begin ()));
        if (next != run.begin ())
            indices.push_back (size_t (next - run.begin () - 1));
    }
    std::sort (indices.begin (), indices.end ());
    indices.erase (std::unique (indices.begin (), indices.end ()), indices.end ());
    return indices;
}

// Cache only normalized exact directions. Camera, centre, radius and preview
// scale affect projection, never this cache. Applied envelopes share geometry.
class SunPathBandCache {
  public:
    void Update (const sunstudyhud::SunPathStudyDay& day,
                 const std::shared_ptr<const sunstudyhud::SunPathPeriodData>& period, bool showsPeriod)
    {
        const auto geometry = showsPeriod && period != nullptr ? period->geometry : nullptr;
        const std::string id = !showsPeriod && day.available ? day.studyId : std::string ();
        const uint64_t revision = !showsPeriod && day.available ? day.revision : 0;
        if (initialized && geometry == source && id == dayId && revision == dayRevision && showsPeriod == isPeriod)
            return;
        initialized = true;
        source = geometry;
        dayId = id;
        dayRevision = revision;
        isPeriod = showsPeriod;
        band = geometry != nullptr ? BuildSunPathBand (*geometry)
                                   : (!showsPeriod ? BuildSunPathBand (day) : SunPathBand {});
        ++builds;
    }

    const SunPathBand& Data () const
    {
        return band;
    }

    uint64_t Builds () const
    {
        return builds;
    }

  private:
    bool initialized = false, isPeriod = false;
    std::shared_ptr<const sunstudyhud::SunPathPeriodGeometry> source;
    std::string dayId;
    uint64_t dayRevision = 0, builds = 0;
    SunPathBand band;
};

} // namespace geomsrv::archviz

#endif
