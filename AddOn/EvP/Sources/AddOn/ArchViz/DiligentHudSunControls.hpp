#ifndef EVP_ARCHVIZ_DILIGENTHUDSUNCONTROLS_HPP
#define EVP_ARCHVIZ_DILIGENTHUDSUNCONTROLS_HPP

#include "ArchViz/GradientLibrary.hpp"

#include <imgui.h>

#include <functional>
#include <span>
#include <string>

namespace evp::sunstudy {
struct PointDay;
}

namespace geomsrv::sunstudyhud {
struct PeriodProfile;
}

namespace geomsrv::archviz {
std::string SunStudyCutSliceTooltip (double fromMinute, double toMinute);
std::string SunStudyCutSurfaceTooltip (int minute);
struct HudState;

// The day diagram is a SEMICIRCLE from the first to the last measured sun
// (#169): the domain's start on the left, its end on the right, time running
// over the top. Radians in ImGui's y-down frame: pi at the left, 3pi/2 at the
// top, 2pi at the right. A degenerate domain maps everything to the top.
inline double SunStudyArcAngle (double minute, double from, double to)
{
    constexpr double pi = 3.14159265358979323846;
    if (!(to > from))
        return 1.5 * pi;
    const double t = (minute - from) / (to - from);
    return pi + pi * (t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
}
// Inverse, from a pointer's x/y offset from the arc's centre, clamped to
// [from, to]: below the diameter the nearer end wins.
double SunStudyArcMinute (double x, double y, double from, double to);

// A click selects one measured step; a drag either way selects a half-open
// minute range; a click inside the diagram but off its bars clears the slice.
// No selection has step -1, slice false and clear false.
struct SunStudyClockSelection {
    int step = -1;
    bool slice = false;
    bool clear = false;
    double fromMinute = 0.0, toMinute = 0.0;
};

// The committed selection the diagram keeps drawn between gestures: the time
// slice (minutes, full day = none) the shadow range or point player holds.
struct SunStudyClockShown {
    double sliceFrom = 0.0, sliceTo = 1440.0;
};

// The day's sampled daylight domain: first measured step to the end of the
// last one. False when the day has no measured step.
bool SunStudyDayDomain (const evp::sunstudy::PointDay& day, double& from, double& to);

// The sun study's diagrams, for their info-circle (DiligentHudSunChartInfo).
enum class SunChart { Day, LiftedDay, PeriodDay, Season, MonthBars, DaysHistogram, ProfileGrid, DayStrip };
// The chart a period frame id names ("##periodbars", ...); Day otherwise.
SunChart SunStudyChartOf (const char* id);
// The info-circle in the top-right corner of a chart `width` wide at
// `topLeft`; on hover, the chart's name, what it shows at the analysis point,
// its data and its calculation. True while hovered: the chart's own tooltip
// then stands down.
bool SunStudyChartInfo (ImVec2 topLeft, float width, SunChart chart);

// A point's day as a sunrise-to-sunset semicircle of radial bars, with a
// lit/shadowed state row and a morning/day/evening time row outside it. Null
// reserves the same space with emptyNote; highlightedStep is a step index, or
// -1 for none; `shown` keeps the committed slice drawn.
void DrawSunStudyDayClock (HudState& state, const evp::sunstudy::PointDay* day, const char* id, int highlightedStep,
                           const char* emptyNote, SunStudyClockSelection* selection = nullptr, float sizeCap = 220.0f,
                           const SunStudyClockShown* shown = nullptr, SunChart chart = SunChart::Day);
// Apply a clock gesture without changing the current Sunstudy/Shadows view.
void ApplySunStudyDaySelection (HudState& state, const SunStudyClockSelection& selection);
bool DrawSunStudyRange (const char* id, float& from, float& to, float minimum, float maximum, bool duration);
void DrawSunStudySeasonalClocks (HudState& state, const geomsrv::sunstudyhud::PeriodProfile* profile, bool estimate);
// `bands` 0 draws the smooth ramp; kSunBandsMin..kSunBandsMax draws that many
// flat bands split at `edges`, each with a grip above the bar to drag.
// `ramp` is the hours colours' stops (SunStudyRampStops); empty, the legacy bins.
void DrawSunStudyGradient (bool blue, float& threshold, int bands = 0, float* edges = nullptr,
                           unsigned lowSunHex = 0x1F5FD1, std::span<const overlaylayers::ColourStop> ramp = {});
// The hours ramp itself, smooth or stepped, and the translucent low-sun box
// below `blueBelow` hours (< 0: none). ⚠️ THE ONE PAINTER for the panel legend
// and the screenshot legend, so the two cannot show different colours.
// `labels` writes each stepped cell's start hour inside it (SunStudyBandLabel),
// left out of a cell too narrow for its text.
void DrawSunStudyRampBar (ImDrawList* draw, ImVec2 start, ImVec2 size, int bands, const float* edges, float blueBelow,
                          unsigned lowSunHex = 0x1F5FD1, bool labels = true,
                          std::span<const overlaylayers::ColourStop> ramp = {});
void DrawSunStudyInspectControl (int& mode, const char* first, const char* second);
// ONE LINE, NEVER WRAPPED: `line` in the disabled colour, cut with "..." at
// the panel edge, and on hover the whole line followed by `detail`. Every
// explanation in the sun panels goes through it, so a long sentence never
// pushes the panel down by a line when the panel narrows or the text changes.
void SunStudyNote (const char* line, const char* detail = nullptr);

struct HudTimeline;
// A player over the steps first..last: to start/back/play/forward/end, a speed
// (0.25x-4x of one step per `stepSeconds`), loop, and a slider reading `label`.
// `hold` stops the clock while the previous step is still being drawn. True
// when `index` moved. Shared by the shadow views' day and the period's months.
bool DrawSunStudyTimeline (const char* id, int& index, int first, int last, HudTimeline& play, double stepSeconds,
                           const char* label, bool hold = false);
// The callback runs after buttons, playback and slider input, so its text names
// the frame actually selected, not the previous one. It is never stored.
bool DrawSunStudyTimeline (const char* id, int& index, int first, int last, HudTimeline& play, double stepSeconds,
                           const std::function<std::string ()>& label, bool hold = false);
} // namespace geomsrv::archviz
#endif
