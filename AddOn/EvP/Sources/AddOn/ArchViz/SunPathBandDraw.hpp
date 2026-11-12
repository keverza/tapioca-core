#ifndef TAPIOCA_ARCHVIZ_SUNPATHBANDDRAW_HPP
#define TAPIOCA_ARCHVIZ_SUNPATHBANDDRAW_HPP

#include "ArchViz/SunPathBand.hpp"

#include <imgui.h>

namespace geomsrv::archviz {

struct SunPathBandDrawing {
    size_t shown = 0, total = 0;
    bool decimated = false, hot = false;
};

inline float SunPathSegmentDistance (ImVec2 mouse, ImVec2 a, ImVec2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    const float t =
        length2 > 0.0f ? std::clamp (((mouse.x - a.x) * dx + (mouse.y - a.y) * dy) / length2, 0.0f, 1.0f) : 0.0f;
    return std::hypot (mouse.x - a.x - t * dx, mouse.y - a.y - t * dy);
}

// Warm yellow, deliberately not a grey trace. Alpha decays with the number of
// traced days to limit overdraw, but never below a legible floor: the old
// integer division (55 / dayCount) collapsed to 1 for studies longer than 55
// days, which read as invisible.
constexpr int kSunPathBandInactiveAlphaFloor = 20;
constexpr int kSunPathBandActiveAlpha = 235;

inline int SunPathBandInactiveAlpha (size_t dayCount)
{
    const size_t days = (std::max) (size_t (1), dayCount);
    return (std::max) (kSunPathBandInactiveAlphaFloor, int (55 / days));
}

// Highlighted/current lines must stand clearly above the inactive field even
// when a whole-year selection is highlighted.
inline int SunPathBandActiveAlphaFor (size_t highlightedDayCount)
{
    return highlightedDayCount <= 2 ? kSunPathBandActiveAlpha : 205;
}

inline ImU32 SunPathBandColour (int alpha)
{
    return IM_COL32 (255, 225, 90, std::clamp (alpha, 0, 255));
}

// Runs are minute-sorted by BuildSunPathBandDay. Equal-minute duplicates are
// still checked by their exact source step, so window filtering or a duplicate
// clock cannot preserve a different sample that merely resembles the selection.
inline size_t SunPathSelectedRunIndex (const std::vector<SunPathMoment>& run, const SunPathMoment& selected)
{
    if (!selected.valid)
        return run.size ();
    const auto first = std::lower_bound (run.begin (), run.end (), selected.minute,
                                         [] (const SunPathMoment& point, int minute) { return point.minute < minute; });
    const auto last = std::upper_bound (first, run.end (), selected.minute,
                                        [] (int minute, const SunPathMoment& point) { return minute < point.minute; });
    for (auto it = first; it != last; ++it)
        if (it->step == selected.step)
            return size_t (it - run.begin ());
    return run.size ();
}

inline bool SameSunPathScreenKey (const SunPathBandScreenCache& cache, const SunPathBand& band, uint64_t bandBuilds,
                                  const float viewProj[16], const double centre[3], double radius, uint32_t width,
                                  uint32_t height)
{
    if (!cache.valid || cache.bandIdentity != &band || cache.bandBuilds != bandBuilds || cache.width != width ||
        cache.height != height || cache.radius != radius || cache.sampleCount != band.steps)
        return false;
    for (int i = 0; i < 3; ++i)
        if (cache.centre[i] != centre[i])
            return false;
    return std::equal (viewProj, viewProj + 16, cache.viewProj);
}

// Projects all samples exactly once per view change. The result is immutable
// for that view, so the per-frame path below never touches samples the LOD
// omitted and never reparses the run to find clipping breaks.
template <class Projection>
void UpdateSunPathBandScreenCache (SunPathBandScreenCache& cache, const SunPathBand& band, uint64_t bandBuilds,
                                   const float viewProj[16], const double centre[3], double radius, uint32_t width,
                                   uint32_t height, const Projection& at, bool force = false)
{
    if (!force && SameSunPathScreenKey (cache, band, bandBuilds, viewProj, centre, radius, width, height))
        return;
    cache.valid = true;
    cache.bandIdentity = &band;
    cache.bandBuilds = bandBuilds;
    cache.width = width;
    cache.height = height;
    cache.radius = radius;
    cache.sampleCount = band.steps;
    for (int i = 0; i < 16; ++i)
        cache.viewProj[i] = viewProj != nullptr ? viewProj[i] : 0.0f;
    for (int i = 0; i < 3; ++i)
        cache.centre[i] = centre != nullptr ? centre[i] : 0.0;
    cache.runs.clear ();
    uint64_t projected = 0;
    for (const auto& traced : band.days) {
        for (const auto& run : traced.runs) {
            cache.runs.emplace_back ();
            auto& screenRun = cache.runs.back ();
            screenRun.samples.resize (run.size ());
            screenRun.clippedBefore.assign (run.size () + 1, 0);
            uint32_t clipped = 0;
            for (size_t i = 0; i < run.size (); ++i) {
                ImVec2 screen;
                const bool visible = at (run[i].direction, screen);
                auto& sample = screenRun.samples[i];
                if (visible) {
                    sample.x = screen.x;
                    sample.y = screen.y;
                }
                sample.visible = visible;
                if (!visible)
                    ++clipped;
                screenRun.clippedBefore[i + 1] = clipped;
            }
            projected += run.size ();
        }
    }
    cache.projectedSamples = projected;
    ++cache.rebuilds;
}

// A supplied cache is trusted only for the same normalized band generation.
// Checking every run's storage as well as its totals makes the convenience
// fallback safe even for a stale cache with identical sample/run counts.
inline bool SunPathBandScreenCacheMatches (const SunPathBandScreenCache& cache, const SunPathBand& band,
                                           uint64_t bandBuilds)
{
    if (!cache.valid || cache.bandIdentity != &band || cache.bandBuilds != bandBuilds ||
        cache.sampleCount != band.steps)
        return false;
    size_t ordinal = 0;
    for (const auto& traced : band.days)
        for (const auto& run : traced.runs) {
            if (ordinal >= cache.runs.size ())
                return false;
            const auto& screenRun = cache.runs[ordinal++];
            if (screenRun.samples.size () != run.size () || screenRun.clippedBefore.size () != run.size () + 1)
                return false;
        }
    return ordinal == cache.runs.size ();
}

// `draw` may be null: the production sunpath draws its band as depth-tested
// scene geometry (SunPathScene.hpp) and uses this only to hit-test the hover.
template <class Projection>
SunPathBandDrawing DrawSunPathBand (ImDrawList* draw, const SunPathBand& band, const SunPathMoment& selected, int month,
                                    int day, bool highlightAll, double sliceFrom, double sliceTo, const Projection& at,
                                    SunPathBandScreenCache* screens = nullptr, uint64_t bandBuilds = 0)
{
    SunPathBandDrawing result;
    result.total = band.steps;
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    size_t highlightedDays = 0, runCount = 0;
    for (const auto& traced : band.days) {
        runCount += traced.runs.size ();
        if ((month == 0 || traced.month == month) && (day == 0 || traced.day == day))
            ++highlightedDays;
    }
    const int baseAlpha = SunPathBandInactiveAlpha (band.days.size ());
    const int hotAlpha = SunPathBandActiveAlphaFor (highlightedDays);
    const size_t runLimit = SunPathBandRunLimit (runCount);

    SunPathBandScreenCache localCache;
    SunPathBandScreenCache* cache = screens != nullptr ? screens : &localCache;
    if (!SunPathBandScreenCacheMatches (*cache, band, bandBuilds)) {
        // Direct callers still get exact clipping through a safe local cache.
        // A stale supplied cache is repaired rather than indexed out of bounds.
        const float noView[16] = {};
        const double noCentre[3] = {};
        UpdateSunPathBandScreenCache (*cache, band, bandBuilds, noView, noCentre, 0.0, 0, 0, at, true);
    }

    // Immediate-mode draw lists painter-order overlapping geometry. Emit the
    // complete inactive field first so a later day can never overpaint the
    // selected/current day, for both ribbon segments and discrete discs.
    for (int pass = 0; pass < 2; ++pass) {
        const bool activePass = pass != 0;
        size_t runOrdinal = 0;
        for (const auto& traced : band.days) {
            const bool highlighted =
                highlightAll || (month > 0 && traced.month == month && (day == 0 || traced.day == day));
            for (const auto& run : traced.runs) {
                const auto& screenRun = cache->runs[runOrdinal++];
                const size_t selectedIndex = selected.valid && traced.year == selected.year &&
                                                     traced.month == selected.month && traced.day == selected.day
                                                 ? SunPathSelectedRunIndex (run, selected)
                                                 : run.size ();
                AppendSunPathRunDisplayIndices (run, selectedIndex, sliceFrom, sliceTo, runLimit,
                                                cache->displayIndices);
                const auto& indices = cache->displayIndices;
                if (!activePass) {
                    result.shown += indices.size ();
                    result.decimated = result.decimated || indices.size () < run.size ();
                }
                ImVec2 previous;
                bool had = false;
                int previousMinute = -1;
                size_t previousVisible = run.size (); // sentinel: no visible displayed sample yet
                for (const size_t i : indices) {
                    const auto& sample = screenRun.samples[i];
                    if (!sample.visible) {
                        had = false;
                        continue;
                    }
                    // A decimated gap that contains a clipped sample starts a
                    // new ribbon rather than bridging across it.
                    if (previousVisible < run.size () && i > previousVisible + 1 &&
                        screenRun.clippedBefore[i] != screenRun.clippedBefore[previousVisible + 1])
                        had = false;
                    const ImVec2 screen { sample.x, sample.y };
                    const bool inSlice = run[i].minute >= sliceFrom && run[i].minute < sliceTo;
                    const bool active = highlighted && inSlice;
                    if (traced.discrete) {
                        if (active == activePass) {
                            if (draw != nullptr)
                                draw->AddCircleFilled (screen, 3.5f, SunPathBandColour (active ? hotAlpha : baseAlpha),
                                                       12);
                            result.hot = result.hot || std::hypot (mouse.x - screen.x, mouse.y - screen.y) <= 7.0f;
                        }
                    }
                    else if (had) {
                        const bool previousInSlice = previousMinute >= sliceFrom && previousMinute < sliceTo;
                        const bool activeSegment = highlighted && inSlice && previousInSlice;
                        if (activeSegment == activePass) {
                            if (draw != nullptr)
                                draw->AddLine (previous, screen,
                                               SunPathBandColour (activeSegment ? hotAlpha : baseAlpha), 8.0f);
                            result.hot = result.hot || SunPathSegmentDistance (mouse, previous, screen) <= 7.0f;
                        }
                    }
                    else if (active == activePass) {
                        if (draw != nullptr)
                            draw->AddCircleFilled (screen, 3.5f, SunPathBandColour (active ? hotAlpha : baseAlpha), 12);
                        result.hot = result.hot || std::hypot (mouse.x - screen.x, mouse.y - screen.y) <= 7.0f;
                    }
                    previous = screen;
                    previousMinute = run[i].minute;
                    previousVisible = i;
                    had = true;
                }
            }
        }
    }
    return result;
}

inline std::string SunPathBandCaption (const SunPathBandDrawing& drawing, bool estimate)
{
    if (drawing.total == 0)
        return "No analysis suns in this window";
    std::string text = drawing.decimated
                           ? "LOD preview: " + std::to_string (drawing.shown) + " of " +
                                 std::to_string (drawing.total) + " analysis steps (before clipping)"
                           : "All " + std::to_string (drawing.total) + " analysis steps (before clipping)";
    if (estimate)
        text += "\nPreview estimate: traced days retain their calendar weights";
    return text;
}

} // namespace geomsrv::archviz

#endif
