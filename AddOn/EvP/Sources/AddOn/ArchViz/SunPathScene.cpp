#include "ArchViz/SunPathScene.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Display colours. Yellow is the analysis; the deeper amber is what the user
// selected (the highlighted day, its slice and the sun), so selection never
// reads as just "more yellow".
constexpr int kYellow[3] = { 255, 214, 64 };
constexpr int kAmber[3] = { 255, 184, 28 };

uint32_t Yellow (int alpha)
{
    return SunPathDisplayRgba (kYellow[0], kYellow[1], kYellow[2], alpha);
}

uint32_t Amber (int alpha)
{
    return SunPathDisplayRgba (kAmber[0], kAmber[1], kAmber[2], alpha);
}

// Step suns and the selected sun scale with the dome, as the arcs do.
constexpr double kStepSunRadius = 0.009;
constexpr double kSelectedSunRadius = 0.026;

// Joined surface rows for a long every-day period: enough to read as one
// continuous patch, few enough that a year never uploads ~35k quads.
constexpr size_t kSurfaceRows = 90;

bool Highlighted (const SunPathBandDay& day, const SunPathAnalysisView& view)
{
    return view.highlightAll || (view.month > 0 && day.month == view.month && (view.day == 0 || day.day == view.day));
}

bool OneDay (const SunPathBandDay& day, const SunPathAnalysisView& view)
{
    return view.month > 0 && view.day > 0 && day.month == view.month && day.day == view.day;
}

void AppendRuns (const SunPathFrame& frame, const SunPathBandDay& day, uint32_t rgba, GhPreviewBucket& bucket)
{
    for (const auto& run : day.runs)
        AppendSunPathDirections (frame, run, rgba, bucket);
}

// The slice thickens only where it says something: a slice covering the whole
// run would just make the line fat everywhere.
void AppendSlice (const SunPathFrame& frame, const SunPathBandDay& day, const SunPathAnalysisView& view,
                  GhPreviewBucket& bucket)
{
    const uint32_t rgba = Amber (255);
    for (const auto& run : day.runs) {
        if (run.size () < 2 || (view.sliceFrom <= run.front ().minute && view.sliceTo > run.back ().minute))
            continue;
        for (size_t i = 1; i < run.size (); ++i) {
            const auto in = [&] (const SunPathMoment& m) {
                return m.minute >= view.sliceFrom && m.minute < view.sliceTo;
            };
            if (!in (run[i - 1]) || !in (run[i]))
                continue;
            AppendSunPathArcPiece (frame, run[i - 1].direction, run[i].direction, rgba, bucket);
        }
    }
}

void AppendStepSuns (const SunPathFrame& frame, const SunPathBandDay& day, int alpha, GhPreviewBucket& bucket)
{
    for (const auto& run : day.runs)
        for (const auto& moment : run) {
            float at[3];
            SunPathWorld (frame, moment.direction, at);
            AppendSunPathSphere (at, float (frame.radius * kStepSunRadius), Yellow (alpha), false, 8, 5, bucket);
        }
}

// The minute-sorted samples of one day across its runs (runs are minute-ordered
// and disjoint, so concatenation stays sorted).
std::vector<const SunPathMoment*> Flatten (const SunPathBandDay& day)
{
    std::vector<const SunPathMoment*> all;
    for (const auto& run : day.runs)
        for (const auto& moment : run)
            all.push_back (&moment);
    return all;
}

const SunPathMoment* At (const std::vector<const SunPathMoment*>& day, int minute)
{
    const auto it = std::lower_bound (day.begin (), day.end (), minute,
                                      [] (const SunPathMoment* m, int value) { return m->minute < value; });
    return it != day.end () && (*it)->minute == minute ? *it : nullptr;
}

// One surface row between two traced days: a quad wherever both days have the
// same two consecutive clock steps. A step either day lacks (below the horizon,
// outside the window) leaves a hole rather than inventing a direction.
// The part of the row between fractions t0 and t1 of the way from a to b.
// Inner edges interpolate the two days' matched directions (normalised lerp):
// display only, and every row still starts and ends on exact stored steps.
size_t AppendStrip (const SunPathFrame& frame, const SunPathBandDay& a, const SunPathBandDay& b, double t0, double t1,
                    uint32_t rgba, GhPreviewBucket& bucket)
{
    const auto other = Flatten (b);
    size_t quads = 0;
    for (const auto& run : a.runs)
        for (size_t i = 1; i < run.size (); ++i) {
            const SunPathMoment* b0 = At (other, run[i - 1].minute);
            const SunPathMoment* b1 = At (other, run[i].minute);
            if (b0 == nullptr || b1 == nullptr)
                continue;
            const auto base = uint32_t (bucket.meshVertices.size ());
            const auto corner = [&] (const float* from, const float* to, double t) {
                float direction[3];
                double d[3], length = 0.0;
                for (int axis = 0; axis < 3; ++axis) {
                    d[axis] = double (from[axis]) * (1.0 - t) + double (to[axis]) * t;
                    length += d[axis] * d[axis];
                }
                length = length > 0.0 ? std::sqrt (length) : 1.0;
                for (int axis = 0; axis < 3; ++axis)
                    direction[axis] = float (d[axis] / length);
                float at[3];
                SunPathWorld (frame, direction, at);
                bucket.meshVertices.push_back ({ at[0], at[1], at[2], 0.0f, 0.0f, 0.0f, rgba });
            };
            corner (run[i - 1].direction, b0->direction, t0);
            corner (run[i].direction, b1->direction, t0);
            corner (run[i].direction, b1->direction, t1);
            corner (run[i - 1].direction, b0->direction, t1);
            for (const uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
                bucket.meshIndices.push_back (base + index);
            ++quads;
        }
    return quads;
}

void AppendSurface (const SunPathFrame& frame, const SunPathBand& band, const SunPathAnalysisView& view, size_t stride,
                    int alpha, int hotAlpha, SunPathSceneGeometry& out)
{
    const size_t count = band.days.size ();
    if (count < 2)
        return;
    out.surfaceStride = stride;
    size_t from = 0;
    // Each row is split at its middle and each half belongs to the traced days
    // on its side. A Preview month is one mid-month day, so it owns half of the
    // row before it and half of the row after: its own calendar span, not the
    // two months the whole rows covered (#179).
    const auto hotBetween = [&] (size_t first, size_t last) {
        bool hot = view.highlightAll;
        for (size_t d = first; d <= last && !hot; ++d)
            hot = Highlighted (band.days[d], view);
        return hot;
    };
    while (from + 1 < count) {
        const size_t to = (std::min) (from + stride, count - 1);
        const size_t middle = from + (to - from) / 2;
        // Highlighted rows change hue as well as alpha: a stronger yellow alone
        // was barely visible over a whole-year surface (#179).
        const bool firstHot = hotBetween (from, middle);
        const bool secondHot = hotBetween (middle + 1, to); // middle < to always
        out.surfaceQuads += AppendStrip (frame, band.days[from], band.days[to], 0.0, 0.5,
                                         firstHot ? Amber (hotAlpha) : Yellow (alpha), out.arcs.translucent);
        out.surfaceQuads += AppendStrip (frame, band.days[from], band.days[to], 0.5, 1.0,
                                         secondHot ? Amber (hotAlpha) : Yellow (alpha), out.arcs.translucent);
        from = to;
    }
}

// The selected/hovered day: whole length in amber, its slice bold, its suns.
// A whole highlighted month (no single day) is bold over its whole length: it
// is the month-by-month player's only visible cue on the dome.
void AppendChosenDay (const SunPathFrame& frame, const SunPathBandDay& day, const SunPathAnalysisView& view,
                      SunPathSceneGeometry& out)
{
    if (view.day == 0) {
        AppendRuns (frame, day, Amber (255), out.bold.depthTested);
        ++out.dayLines;
        return;
    }
    AppendRuns (frame, day, Amber (255), out.arcs.depthTested);
    AppendSlice (frame, day, view, out.bold.depthTested);
    if (day.discrete)
        AppendStepSuns (frame, day, 220, out.arcs.translucent);
    ++out.dayLines;
}

} // namespace

uint32_t SunPathDisplayRgba (int r, int g, int b, int a)
{
    const auto linear = [] (int value) {
        const double c = std::clamp (value, 0, 255) / 255.0;
        const double l = c <= 0.04045 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4);
        return uint32_t (std::lround (l * 255.0));
    };
    return linear (r) << 24 | linear (g) << 16 | linear (b) << 8 | uint32_t (std::clamp (a, 0, 255));
}

void AppendSunPathArcPiece (const SunPathFrame& frame, const float a[3], const float b[3], uint32_t rgba,
                            GhPreviewBucket& bucket)
{
    const double la = std::sqrt (double (a[0]) * a[0] + double (a[1]) * a[1] + double (a[2]) * a[2]);
    const double lb = std::sqrt (double (b[0]) * b[0] + double (b[1]) * b[1] + double (b[2]) * b[2]);
    if (!(la > 0.0) || !(lb > 0.0))
        return;
    const double cosine =
        std::clamp ((double (a[0]) * b[0] + double (a[1]) * b[1] + double (a[2]) * b[2]) / (la * lb), -1.0, 1.0);
    // Bounded: a malformed half-circle hop must not emit thousands of pieces.
    const int pieces = std::clamp (int (std::ceil (std::acos (cosine) / kSunPathMaxPieceRadians)), 1, 256);
    float previous[3];
    SunPathWorld (frame, a, previous);
    for (int i = 1; i <= pieces; ++i) {
        float point[3];
        if (i == pieces)
            SunPathWorld (frame, b, point);
        else {
            const double t = double (i) / pieces;
            double d[3];
            for (int axis = 0; axis < 3; ++axis)
                d[axis] = double (a[axis]) / la * (1.0 - t) + double (b[axis]) / lb * t;
            const double length = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const double scale = length > 0.0 ? (la * (1.0 - t) + lb * t) / length : 0.0;
            const float direction[3] = { float (d[0] * scale), float (d[1] * scale), float (d[2] * scale) };
            SunPathWorld (frame, direction, point);
        }
        AppendPreviewSegment (previous, point, rgba, bucket);
        std::copy (point, point + 3, previous);
    }
}

void SunPathWorld (const SunPathFrame& frame, const float direction[3], float out[3])
{
    for (int axis = 0; axis < 3; ++axis)
        out[axis] = float (frame.centre[axis] + frame.radius * double (direction[axis]));
}

SunPathPreviewForm SunPathPreviewFormOf (const SunPathBand& band, bool period, bool estimate)
{
    if (band.days.empty ())
        return SunPathPreviewForm::None;
    if (!period)
        return SunPathPreviewForm::Day;
    return estimate ? SunPathPreviewForm::MonthlyPreview : SunPathPreviewForm::EveryDay;
}

void AppendSunPathCompass (const SunPathFrame& frame, double northRadians, SunPathSceneGeometry& out)
{
    const auto at = [&] (double angle, double ring, float point[3]) {
        point[0] = float (frame.centre[0] + frame.radius * ring * std::cos (angle));
        point[1] = float (frame.centre[1] + frame.radius * ring * std::sin (angle));
        point[2] = float (frame.groundZ);
    };
    constexpr int kSegments = 720; // half-degree pieces: a smooth ring
    constexpr double kRings[] = { 1.0, 1.035, 1.07 };
    for (int ring = 0; ring < 3; ++ring) {
        const uint32_t rgba = SunPathDisplayRgba (96, 96, 100, ring == 0 ? 200 : 140);
        for (int i = 0; i < kSegments; ++i) {
            float a[3], b[3];
            at (2.0 * kPi * double (i) / kSegments, kRings[ring], a);
            at (2.0 * kPi * double (i + 1) / kSegments, kRings[ring], b);
            AppendPreviewSegment (a, b, rgba, out.fine.depthTested);
        }
    }
    // Thirty-two ticks clockwise from project north; the eight principal ones
    // reach the inner ring. North is red and on the arc layer's wider line.
    for (int i = 0; i < 32; ++i) {
        const double angle = northRadians - double (i) * 2.0 * kPi / 32.0;
        float a[3], b[3];
        at (angle, i % 4 == 0 ? 1.0 : 1.035, a);
        at (angle, 1.07, b);
        if (i == 0)
            AppendPreviewSegment (a, b, SunPathDisplayRgba (220, 60, 60, 255), out.arcs.depthTested);
        else
            AppendPreviewSegment (a, b, SunPathDisplayRgba (90, 90, 96, i % 4 == 0 ? 230 : 160), out.fine.depthTested);
    }
}

void AppendSunPathAnalysis (const SunPathFrame& frame, const SunPathBand& band, SunPathPreviewForm form,
                            const SunPathAnalysisView& view, SunPathSceneGeometry& out)
{
    const auto& days = band.days;
    switch (form) {
        case SunPathPreviewForm::None:
            return;
        case SunPathPreviewForm::Day:
            // One day, read along its length: the arc, its steps, the slice.
            for (const auto& day : days) {
                AppendRuns (frame, day, Yellow (235), out.arcs.depthTested);
                AppendSlice (frame, day, view, out.bold.depthTested);
                if (day.discrete)
                    AppendStepSuns (frame, day, 200, out.arcs.translucent);
                ++out.dayLines;
            }
            return;
        case SunPathPreviewForm::EveryDay: {
            // Every day was traced: one continuous boundary surface, its two
            // edges solid, a faint line where each month begins.
            const size_t stride = (std::max) (size_t (1), (days.size () + kSurfaceRows - 1) / kSurfaceRows);
            AppendSurface (frame, band, view, stride, 40, 130, out);
            for (size_t d = 0; d < days.size (); ++d) {
                const auto& day = days[d];
                if (OneDay (day, view))
                    continue; // drawn last, on top
                const bool edge = d == 0 || d + 1 == days.size ();
                const bool monthStart = d > 0 && days[d - 1].month != day.month;
                const bool monthEdge = view.day == 0 && view.month > 0 && day.month == view.month &&
                                       (d == 0 || days[d - 1].month != day.month || d + 1 == days.size () ||
                                        days[d + 1].month != day.month);
                if (monthEdge)
                    AppendRuns (frame, day, Amber (255), out.bold.depthTested);
                else if (edge)
                    AppendRuns (frame, day, Yellow (220), out.arcs.depthTested);
                else if (monthStart)
                    AppendRuns (frame, day, Yellow (140), out.fine.depthTested);
                else
                    continue;
                ++out.dayLines;
            }
            break;
        }
        case SunPathPreviewForm::MonthlyPreview:
            // One representative day per month: each is its own line, and a
            // lighter surface between them shows the span it stands for.
            AppendSurface (frame, band, view, 1, 30, 130, out);
            for (const auto& day : days) {
                if (OneDay (day, view))
                    continue;
                if (Highlighted (day, view) && !view.highlightAll) {
                    AppendChosenDay (frame, day, view, out);
                    continue;
                }
                AppendRuns (frame, day, Yellow (150), out.arcs.depthTested);
                ++out.dayLines;
            }
            break;
    }
    for (const auto& day : days)
        if (OneDay (day, view))
            AppendChosenDay (frame, day, view, out);
}

void AppendSunPathSphere (const float at[3], float radius, uint32_t rgba, bool shaded, int slices, int stacks,
                          GhPreviewBucket& bucket)
{
    if (!(radius > 0.0f) || slices < 3 || stacks < 2)
        return;
    const auto base = uint32_t (bucket.meshVertices.size ());
    for (int s = 0; s <= stacks; ++s) {
        const double phi = kPi * double (s) / stacks;
        for (int i = 0; i <= slices; ++i) {
            const double theta = 2.0 * kPi * double (i) / slices;
            const float n[3] = { float (std::sin (phi) * std::cos (theta)), float (std::sin (phi) * std::sin (theta)),
                                 float (std::cos (phi)) };
            bucket.meshVertices.push_back ({ at[0] + radius * n[0], at[1] + radius * n[1], at[2] + radius * n[2],
                                             shaded ? n[0] : 0.0f, shaded ? n[1] : 0.0f, shaded ? n[2] : 0.0f, rgba });
        }
    }
    const uint32_t row = uint32_t (slices + 1);
    for (int s = 0; s < stacks; ++s)
        for (int i = 0; i < slices; ++i) {
            const uint32_t a = base + uint32_t (s) * row + uint32_t (i), b = a + row;
            for (const uint32_t index : { a, b, a + 1, a + 1, b, b + 1 })
                bucket.meshIndices.push_back (index);
        }
}

void AppendSunPathSun (const SunPathFrame& frame, const float direction[3], SunPathSceneGeometry& out)
{
    float at[3];
    SunPathWorld (frame, direction, at);
    AppendSunPathSphere (at, float (frame.radius * kSelectedSunRadius), Amber (255), true, 18, 12,
                         out.arcs.depthTested);
}

std::string SunPathSceneCaption (SunPathPreviewForm form, const SunPathBand& band, const SunPathSceneGeometry& scene,
                                 bool estimate)
{
    if (form == SunPathPreviewForm::None || band.steps == 0)
        return "No analysis suns in this window";
    const std::string steps = "All " + std::to_string (band.steps) + " analysis steps";
    std::string text;
    switch (form) {
        case SunPathPreviewForm::Day:
            text = steps + " of the day, drawn on its arc";
            break;
        case SunPathPreviewForm::EveryDay:
            text = steps + " over " + std::to_string (band.days.size ()) + " traced days: one surface across them";
            if (scene.surfaceStride > 1)
                text += " (rows joined every " + std::to_string (scene.surfaceStride) + " traced days)";
            text += ", the first and last day, and a line where each month starts";
            break;
        default:
            text = steps + ": one representative day per month, a surface between them";
            break;
    }
    if (estimate)
        text += "\nPreview estimate: traced days retain their calendar weights";
    return text;
}

} // namespace geomsrv::archviz
