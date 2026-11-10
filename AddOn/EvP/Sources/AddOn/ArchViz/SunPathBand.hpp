#ifndef TAPIOCA_ARCHVIZ_SUNPATHBAND_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHBAND_HPP

#include "ArchViz/SunPathMoment.hpp"

#include <algorithm>
#include <cstdint>
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
// breaks. Source data remains exact. A period of many traced days shares one
// global sample budget: per-run decimation must not add up to tens of
// thousands of immediate-mode lines.
constexpr size_t kSunPathBandSampleBudget = 2400;
constexpr size_t kSunPathBandMaxRunSamples = 144;
constexpr size_t kSunPathBandMinRunSamples = 2; // both endpoints of the run

inline size_t SunPathBandRunLimit (size_t runCount)
{
    if (runCount == 0)
        return kSunPathBandMaxRunSamples;
    return std::clamp (kSunPathBandSampleBudget / runCount, kSunPathBandMinRunSamples, kSunPathBandMaxRunSamples);
}

// Fills the caller's vector so a frame with many runs reuses one allocation
// instead of allocating one vector per run.
inline void AppendSunPathDisplayIndices (size_t count, size_t selected, size_t limit, std::vector<size_t>& out)
{
    out.clear ();
    if (count == 0)
        return;
    const size_t bounded = (std::max) (size_t (2), limit);
    const size_t stride = (std::max) (size_t (1), (count + bounded - 1) / bounded);
    for (size_t i = 0; i < count; i += stride)
        out.push_back (i);
    out.push_back (count - 1);
    if (selected < count)
        out.push_back (selected);
    std::sort (out.begin (), out.end ());
    out.erase (std::unique (out.begin (), out.end ()), out.end ());
}

inline std::vector<size_t> SunPathDisplayIndices (size_t count, size_t selected, size_t limit = 144)
{
    std::vector<size_t> result;
    AppendSunPathDisplayIndices (count, selected, limit, result);
    return result;
}

inline void AppendSunPathRunDisplayIndices (const std::vector<SunPathMoment>& run, size_t selected, double from,
                                            double to, size_t limit, std::vector<size_t>& out)
{
    AppendSunPathDisplayIndices (run.size (), selected, limit, out);
    for (const double edge : { from, to }) {
        const auto next =
            std::lower_bound (run.begin (), run.end (), edge,
                              [] (const SunPathMoment& point, double minute) { return point.minute < minute; });
        if (next != run.end ())
            out.push_back (size_t (next - run.begin ()));
        if (next != run.begin ())
            out.push_back (size_t (next - run.begin () - 1));
    }
    std::sort (out.begin (), out.end ());
    out.erase (std::unique (out.begin (), out.end ()), out.end ());
}

inline std::vector<size_t> SunPathRunDisplayIndices (const std::vector<SunPathMoment>& run, size_t selected,
                                                     double from, double to)
{
    std::vector<size_t> indices;
    AppendSunPathRunDisplayIndices (run, selected, from, to, kSunPathBandMaxRunSamples, indices);
    return indices;
}

// Screen-space samples for one view. Building this is O(all samples); the
// renderer prepares it once per camera/band change and then only draws and
// hit-tests the decimated indices. Without the cached prefix, a decimated
// segment would either reproject every skipped sample (the old O(total) frame
// cost) or bridge across a clipped one.
struct SunPathBandSampleScreen {
    float x = 0.0f, y = 0.0f;
    bool visible = false;
};

struct SunPathBandRunScreen {
    std::vector<SunPathBandSampleScreen> samples;
    std::vector<uint32_t> clippedBefore; // count of clipped samples in [0, i)
};

struct SunPathBandScreenCache {
    bool valid = false;
    const SunPathBand* bandIdentity = nullptr;
    uint64_t bandBuilds = 0;
    float viewProj[16] = {};
    double centre[3] = {};
    double radius = 0.0;
    uint32_t width = 0, height = 0;
    size_t sampleCount = 0;
    std::vector<SunPathBandRunScreen> runs;
    std::vector<size_t> displayIndices; // per-frame scratch, never part of the key
    uint64_t rebuilds = 0;
    uint64_t projectedSamples = 0; // observable: samples projected by the last rebuild
};

// Cache only normalized exact directions. Camera, centre, radius and preview
// scale affect projection, never this cache. Applied envelopes share geometry.
class SunPathBandCache {
  public:
    void Update (const sunstudyhud::SunPathStudyDay& day,
                 const std::shared_ptr<const sunstudyhud::SunPathPeriodData>& period, bool showsPeriod)
    {
        const auto geometry = showsPeriod && period != nullptr ? period->geometry : nullptr;
        const bool usesDay = !showsPeriod && day.available;
        const std::string* id = usesDay ? &day.studyId : nullptr;
        const uint64_t revision = usesDay ? day.revision : 0;
        // Compare before copying: this runs every rendered frame, and a period
        // study's id is unchanged almost every time.
        const bool sameId = id == nullptr ? !hasDayId : (hasDayId && *id == dayId);
        if (initialized && geometry == source && sameId && revision == dayRevision && showsPeriod == isPeriod)
            return;
        initialized = true;
        source = geometry;
        hasDayId = id != nullptr;
        if (id != nullptr)
            dayId = *id;
        else
            dayId.clear ();
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
    bool initialized = false, isPeriod = false, hasDayId = false;
    std::shared_ptr<const sunstudyhud::SunPathPeriodGeometry> source;
    std::string dayId;
    uint64_t dayRevision = 0, builds = 0;
    SunPathBand band;
};

} // namespace geomsrv::archviz

#endif
