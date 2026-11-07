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
struct HudState;

// Radians on the 24-hour study clock: 00:00/24:00 at the top, then clockwise.
inline double SunStudyClockAngle (double minute)
{
    return minute * (6.28318530717958647692 / 1440.0) - 1.57079632679489661923;
}
// Inverse, from a pointer's x/y offset from the clock centre: [0, 1440).
double SunStudyClockMinute (double x, double y);

// A click selects one measured step; a clockwise drag selects a half-open
// minute range. No selection has step -1 and slice false.
struct SunStudyClockSelection {
    int step = -1;
    bool slice = false;
    double fromMinute = 0.0, toMinute = 0.0;
};

// A placed point's day as a 24-hour radial bar chart. Null reserves the same
// space with emptyNote; highlightedStep is a step index, or -1 for none.
void DrawSunStudyDayClock (HudState& state, const evp::sunstudy::PointDay* day, const char* id, int highlightedStep,
                           const char* emptyNote, SunStudyClockSelection* selection = nullptr, float sizeCap = 220.0f);
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
