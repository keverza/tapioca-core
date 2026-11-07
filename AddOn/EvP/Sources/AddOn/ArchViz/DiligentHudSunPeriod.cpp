// The finished period's view in the sun study panel (#152): which quantity the
// map shows (each with its own colours), how the player steps through the
// period -- every kept step, one clock time a day, day by day, or month by
// month -- and the study point's period: its months from the stored totals,
// and on demand its month x hour profile re-traced against the period's suns.
//
// ⚠️ ITS OWN TRANSLATION UNIT, for the reason DiligentHudSunRun.cpp is: the HUD
// files sit near the size cap, and this is one concern -- looking through a
// finished period, where that file asks for one.
//
// ⚠️ FIXED HEIGHTS. Every row and diagram here is drawn at its size whether or
// not it has data (grey when it has none), and explanations are one line with
// the rest on hover (SunStudyNote): a panel that grew and shrank as the cursor
// moved, or as a trace finished, read as a glitch.
//
// RENDER THREAD. Every host or store call goes through the runner
// (NativeCommands/SunStudyHudRunner), which owns the retained period and the
// one worker that presents a view or traces a profile; this file only asks
// and draws what comes back.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz {

namespace {

constexpr const char* kMonthNames[] = { "January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December" };
constexpr const char* kMonthShort[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
constexpr const char* kMonthLetters[] = { "J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D" };
constexpr double kMonthStepSeconds = 0.8; // the month player's 1x
constexpr double kDayStepSeconds = 0.25;  // day by day and fixed hour
constexpr double kStepSeconds = 0.1;      // every step

enum PlayMode { kEveryStep = 0, kFixedHour = 1, kDayByDay = 2, kMonthByMonth = 3 };
constexpr const char* kPlayModes[] = { "Every step", "Fixed hour", "Day by day", "Month by month" };
constexpr const char* kPlayModeTips[] = {
    "Every kept step of every kept day in turn: the period's shadows, continuously.",
    "One clock time of every kept day: how the shadow at that hour moves through the period.",
    "Each kept day's hours of the chosen quantity, one day after another.",
    "Each month's mean hours a day; the whole period first.",
};

struct Quantity {
    const char* button;
    const char* line; // one line in the panel
    const char* meaning;
};

// The order is the runner's PeriodView::quantity.
constexpr Quantity kQuantities[] = {
    { "Direct", "Direct sun, hours a day",
      "Direct sun: hours a day with an unobstructed view of the sun (mean of the days shown)." },
    { "Possible", "Possible sun, hours a day",
      "Possible sun: hours a day the sun is in front of the surface -- the most it could get with nothing in the "
      "way. Depends only on orientation and the sky." },
    { "Lost", "Lost to obstruction, hours a day",
      "Lost to obstruction: hours a day the sun was in front of the surface but blocked by the model. Time the sun "
      "is behind a surface is never counted as lost." },
    { "Access", "Sun access, % of possible",
      "Sun access: the share of its possible sun a surface gets. Independent of orientation, so facades and roofs "
      "compare." },
    { "Days", "Days at or above a threshold, %",
      "Days at or above: the share of the period's days with at least the chosen hours of direct sun. Whole period "
      "only." },
};
constexpr int kQuantityCount = int (std::size (kQuantities));

bool IsShare (int quantity)
{
    return quantity == 3 || quantity == 4;
}

bool Instant (int mode)
{
    return mode == kEveryStep || mode == kFixedHour;
}

// Each quantity keeps its own colours; the person's choice for the one being
// left is remembered for when they come back.
void SwitchColours (HudState& state, int quantity)
{
    const int from = std::clamp (state.sunPeriodPresetsFor, 0, kQuantityCount - 1);
    state.sunPeriodPresets[from] = state.sunGradientPreset;
    state.sunGradientPreset = state.sunPeriodPresets[std::clamp (quantity, 0, kQuantityCount - 1)];
    state.sunPeriodPresetsFor = quantity;
}

void QuantityButtons (HudState& state, int mode)
{
    const float spacing = ImGui::GetStyle ().ItemSpacing.x * 0.5f;
    const float width =
        (ImGui::GetContentRegionAvail ().x - spacing * float (kQuantityCount - 1)) / float (kQuantityCount);
    for (int q = 0; q < kQuantityCount; ++q) {
        if (q > 0)
            ImGui::SameLine (0.0f, spacing);
        // An instant is lit or not: only Direct means anything. Days is a
        // whole-period count, so a single day or step has none.
        const bool usable = Instant (mode) ? q == 0 : !(q == 4 && mode != kMonthByMonth);
        const bool active = state.sunPeriodQuantity == q;
        ImGui::PushStyleColor (ImGuiCol_Button,
                               ImGui::GetStyleColorVec4 (active ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        ImGui::PushID (q);
        ImGui::BeginDisabled (!usable);
        if (ImGui::Button (kQuantities[q].button, ImVec2 (width, 0.0f)) && !active) {
            state.sunPeriodQuantity = q;
            SwitchColours (state, q);
        }
        ImGui::EndDisabled ();
        ImGui::PopID ();
        ImGui::PopStyleColor ();
        if (usable)
            ImGui::SetItemTooltip ("%s", kQuantities[q].meaning);
        else if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip ("%s\n\n%s", kQuantities[q].meaning,
                               Instant (mode) ? "An instant shows lit or not lit: only Direct applies. Step Day by day "
                                                "or Month by month for the others."
                                              : "Whole period only: step Month by month.");
    }
}

std::string HoursText (double hours)
{
    return SunStudyClock (hours, true);
}

std::string ClockText (int minutes)
{
    char text[8];
    std::snprintf (text, sizeof (text), "%02d:%02d", minutes / 60, minutes % 60);
    return text;
}

// ---- the player -----------------------------------------------------------

// The mode row: how the player steps, and the fixed hour beside it. One row
// whatever the mode, so the timeline below never moves.
void PlayModeRow (HudState& state, const sunstudyhud::PeriodInfo& period)
{
    if (!period.daysKept)
        state.sunPeriodPlayMode = kMonthByMonth;
    const float half = (ImGui::GetContentRegionAvail ().x - ImGui::GetStyle ().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth (half);
    if (ImGui::BeginCombo ("##periodplaymode", kPlayModes[std::clamp (state.sunPeriodPlayMode, 0, 3)])) {
        for (int m = 0; m < 4; ++m) {
            const bool enabled = m == kMonthByMonth || period.daysKept;
            if (ImGui::Selectable (kPlayModes[m], m == state.sunPeriodPlayMode,
                                   enabled ? 0 : ImGuiSelectableFlags_Disabled)) {
                state.sunPeriodPlayMode = m;
                state.sunPeriodPlay.playing = false;
                if (Instant (m) && state.sunPeriodQuantity != 0) {
                    state.sunPeriodQuantity = 0;
                    SwitchColours (state, 0);
                }
                if (m != kMonthByMonth && state.sunPeriodQuantity == 4) {
                    state.sunPeriodQuantity = 0;
                    SwitchColours (state, 0);
                }
            }
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip ("%s%s", kPlayModeTips[m],
                                   enabled ? ""
                                           : "\n\nWhole-map frames need \"Keep whole-map days for playback\". For "
                                             "one point, use Detailed point playback below; no day archive is needed.");
        }
        ImGui::EndCombo ();
    }
    ImGui::SetItemTooltip ("How the player steps through the period.%s",
                           period.daysKept ? ""
                                           : "\n\nWhole-map steps need \"Keep whole-map days for playback\". The "
                                             "study point below can instead re-trace and play only its own cells.");
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (-1.0f);
    ImGui::BeginDisabled (state.sunPeriodPlayMode != kFixedHour);
    int quarter = std::clamp (state.sunPeriodClock / 15, 0, 95);
    if (ImGui::SliderInt ("##periodclock", &quarter, 0, 95, ClockText (quarter * 15).c_str ()))
        state.sunPeriodClock = quarter * 15;
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip ("Fixed hour: the clock time shown on every day (the nearest traced step).");
}

// The month timeline: "whole period" and every month the period reaches.
void MonthTimeline (HudState& state, const sunstudyhud::PeriodInfo& period, sunstudyhud::PeriodView& want)
{
    std::vector<int> months { 0 };
    for (int m = 1; m <= 12; ++m)
        if (period.monthDays[m - 1] > 0.0)
            months.push_back (m);
    int index = 0;
    for (int i = 0; i < int (months.size ()); ++i)
        if (months[i] == state.sunPeriodMonth)
            index = i;
    const auto label = [&] {
        // Once playing, the whole-period summary is not a playback frame.
        if (state.sunPeriodPlay.playing && index == 0 && months.size () > 1)
            index = 1;
        char text[64];
        if (months[index] == 0)
            std::snprintf (text, sizeof (text), "Whole period (%.0f days)", period.representedDays);
        else
            std::snprintf (text, sizeof (text), "%s (%.0f days)", kMonthNames[months[index] - 1],
                           period.monthDays[months[index] - 1]);
        return std::string (text);
    };
    DrawSunStudyTimeline ("periodmonths", index, 0, int (months.size ()) - 1, state.sunPeriodPlay, kMonthStepSeconds,
                          label, period.busy);
    state.sunPeriodMonth = months[index];
    want.frame = 0;
    want.month = state.sunPeriodMonth;
}

// The step nearest `clock`, or one past the last when the sun is down then.
int StepAt (const std::vector<uint16_t>& minutes, double hoursPerStep, int clock)
{
    int best = int (minutes.size ());
    int distance = 1 << 30;
    for (size_t s = 0; s < minutes.size (); ++s)
        if (const int d = std::abs (int (minutes[s]) - clock); d < distance) {
            distance = d;
            best = int (s);
        }
    const int step = hoursPerStep > 0.0 ? int (hoursPerStep * 60.0) : 60;
    return distance <= step / 2 ? best : int (minutes.size ());
}

int StepAt (const sunstudyhud::KeptDay& day, int clock)
{
    return StepAt (day.minutes, day.hoursPerStep, clock);
}

void DayTimeline (HudState& state, const sunstudyhud::PeriodInfo& period, const std::vector<sunstudyhud::KeptDay>& kept,
                  sunstudyhud::PeriodView& want)
{
    const int last = int (kept.size ()) - 1;
    if (state.sunPeriodPlayMode == kEveryStep) {
        // Every step of every day, flattened; a sunless day has none.
        std::vector<int> first (kept.size () + 1, 0);
        for (size_t d = 0; d < kept.size (); ++d)
            first[d + 1] = first[d] + int (kept[d].minutes.size ());
        const int total = first.back ();
        if (total == 0) {
            DrawSunStudyTimeline ("periodsteps", state.sunPeriodStep, 0, 0, state.sunPeriodPlay, kStepSeconds,
                                  "no sun on any kept day", true);
            return;
        }
        int index =
            std::clamp (first[size_t (std::clamp (state.sunPeriodDay, 0, last))] + state.sunPeriodStep, 0, total - 1);
        const auto locate = [&] (int at, int& day, int& step) {
            day = int (std::upper_bound (first.begin (), first.end (), at) - first.begin ()) - 1;
            step = at - first[size_t (day)];
        };
        int day = 0, step = 0;
        const auto label = [&] {
            int labelDay = 0, labelStep = 0;
            locate (index, labelDay, labelStep);
            char text[64];
            std::snprintf (
                text, sizeof (text), "%s %d  %s", kMonthShort[std::clamp (kept[size_t (labelDay)].month, 1, 12) - 1],
                kept[size_t (labelDay)].day, ClockText (kept[size_t (labelDay)].minutes[size_t (labelStep)]).c_str ());
            return std::string (text);
        };
        DrawSunStudyTimeline ("periodsteps", index, 0, total - 1, state.sunPeriodPlay, kStepSeconds, label,
                              period.busy);
        locate (index, day, step);
        state.sunPeriodDay = day;
        state.sunPeriodStep = step;
        want.frame = 1;
        want.day = day;
        want.step = step;
        return;
    }
    int day = std::clamp (state.sunPeriodDay, 0, last);
    const auto label = [&] {
        const sunstudyhud::KeptDay& shown = kept[size_t (day)];
        char text[64];
        if (state.sunPeriodPlayMode == kFixedHour)
            std::snprintf (text, sizeof (text), "%s %d  %s", kMonthShort[std::clamp (shown.month, 1, 12) - 1],
                           shown.day, ClockText (state.sunPeriodClock).c_str ());
        else
            std::snprintf (text, sizeof (text), "%s %d  (%d / %d)", kMonthShort[std::clamp (shown.month, 1, 12) - 1],
                           shown.day, day + 1, last + 1);
        return std::string (text);
    };
    DrawSunStudyTimeline ("perioddays", day, 0, last, state.sunPeriodPlay, kDayStepSeconds, label, period.busy);
    state.sunPeriodDay = day;
    want.day = day;
    if (state.sunPeriodPlayMode == kFixedHour) {
        want.frame = 1;
        want.step = StepAt (kept[size_t (day)], state.sunPeriodClock);
    }
    else
        want.frame = 2;
}

void ThresholdChoice (HudState& state, const sunstudyhud::PeriodInfo& period)
{
    char preview[32];
    const int edge = std::clamp (state.sunPeriodThreshold, 0, 12);
    std::snprintf (preview, sizeof (preview), "at least %s", HoursText (period.edges[edge]).c_str ());
    ImGui::SetNextItemWidth (-1.0f);
    if (ImGui::BeginCombo ("##periodthreshold", preview)) {
        for (int e = 0; e < 13; ++e) {
            char item[32];
            std::snprintf (item, sizeof (item), "at least %s", HoursText (period.edges[e]).c_str ());
            if (ImGui::Selectable (item, e == edge))
                state.sunPeriodThreshold = e;
        }
        ImGui::EndCombo ();
    }
    ImGui::SetItemTooltip ("Whole period only: days are binned by their hours as they were traced.");
}

// ---- the study point's diagrams ---------------------------------------------

// The colour the map gives `fraction` (0-1 of its ramp) in the current
// gradient: the diagrams use the map's own colours.
struct Ramp {
    std::vector<overlaylayers::ColourStop> stops;
    ImU32 At (double fraction) const
    {
        const uint32_t rgba = SunStudyRampColour (stops, float (std::clamp (fraction, 0.0, 1.0)) * kSunRampTop);
        return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
    }
};

constexpr ImU32 kGhost = IM_COL32 (120, 126, 136, 90);
constexpr ImU32 kBehind = IM_COL32 (70, 74, 82, 255);

// A diagram's frame, reserved at its size; grey with `note` when it has no data.
ImVec2 ChartFrame (const char* id, float height, bool& hovered, const char* note = nullptr)
{
    const float width = ImGui::GetContentRegionAvail ().x;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton (id, ImVec2 (width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (start, ImVec2 (start.x + width, start.y + height), ImGui::GetColorU32 (ImGuiCol_FrameBg));
    hovered = ImGui::IsItemHovered () && !SunStudyChartInfo (start, width, SunStudyChartOf (id));
    if (note != nullptr && note[0] != 0) {
        const ImVec2 size = ImGui::CalcTextSize (note);
        draw->AddText (ImVec2 (start.x + (width - size.x) * 0.5f, start.y + (height - size.y) * 0.5f),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), note);
    }
    return start;
}

float BarsHeight ()
{
    return ImGui::GetTextLineHeight () * 6.0f;
}

// What a month's bar is for each quantity: the value (hours a day, or a 0-1
// share), its ghost behind it (what it is a part of), and the ramp fraction.
struct MonthValue {
    double value = 0.0, ghost = 0.0;
};

MonthValue MonthOf (int quantity, const sunstudyhud::PeriodInfo& period, const sunstudyhud::PeriodPoint& point, int m)
{
    const double days = period.monthDays[m];
    if (days <= 0.0)
        return {};
    const double direct = point.unoccluded[m] / days, facing = point.facing[m] / days;
    const double up = period.siteDaylight[m] / days;
    switch (quantity) {
        case 1:
            return { facing, up };
        case 2:
            return { (std::max) (0.0, facing - direct), facing };
        case 3:
            return { facing > 0.0 ? direct / facing : 0.0, 1.0 };
        default:
            return { direct, facing };
    }
}

// The month on screen, from what is APPLIED -- not what was asked -- so the
// highlight moves with the map, never ahead of it.
int ShownMonth (const sunstudyhud::PeriodInfo& period, const std::vector<sunstudyhud::KeptDay>* kept)
{
    const sunstudyhud::PeriodView& shown = period.applied;
    if (shown.frame == 0)
        return shown.month;
    if (kept != nullptr && shown.day >= 0 && size_t (shown.day) < kept->size ())
        return (*kept)[size_t (shown.day)].month;
    return 0;
}

// Twelve columns: the quantity's bar in the map's colours over its ghost.
void MonthBars (int quantity, const sunstudyhud::PeriodInfo& period, const sunstudyhud::PeriodPoint* point,
                int shownMonth, const Ramp& ramp)
{
    bool hovered = false;
    const float height = BarsHeight ();
    const ImVec2 start = ChartFrame ("##periodbars", height + ImGui::GetTextLineHeight (), hovered,
                                     point == nullptr ? "no study point" : nullptr);
    if (point == nullptr)
        return;
    const bool share = quantity == 3;
    double hoursTop = 1.0, daylight = 0.0, days = 0.0;
    for (int m = 0; m < 12; ++m)
        if (period.monthDays[m] > 0.0) {
            hoursTop = (std::max) (hoursTop, period.siteDaylight[m] / period.monthDays[m]);
            daylight += period.siteDaylight[m];
            days += period.monthDays[m];
        }
    const double top = share ? 1.0 : hoursTop;
    // The map's ramp top for hours: the period's mean daylight a day.
    const double rampTop = share ? 1.0 : (days > 0.0 ? daylight / days : 1.0);
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float width = ImGui::GetContentRegionAvail ().x;
    const float column = width / 12.0f;
    const float base = start.y + height;
    const int hoveredMonth = hovered ? std::clamp (int ((ImGui::GetIO ().MousePos.x - start.x) / column), 0, 11) : -1;
    for (int m = 0; m < 12; ++m) {
        const float x0 = start.x + column * float (m) + 2.0f, x1 = x0 + column - 4.0f;
        if (m + 1 == shownMonth || m == hoveredMonth)
            draw->AddRectFilled (ImVec2 (x0 - 2.0f, start.y), ImVec2 (x1 + 2.0f, base), IM_COL32 (255, 255, 255, 22));
        if (period.monthDays[m] > 0.0) {
            const MonthValue v = MonthOf (quantity, period, *point, m);
            const auto y = [&] (double value) { return base - float (std::clamp (value / top, 0.0, 1.0)) * height; };
            if (!share)
                draw->AddRectFilled (ImVec2 (x0, y (v.ghost)), ImVec2 (x1, base), kGhost);
            draw->AddRectFilled (ImVec2 (x0 + 1.0f, y (v.value)), ImVec2 (x1 - 1.0f, base),
                                 ramp.At (v.value / rampTop));
        }
        const ImVec2 letter = ImGui::CalcTextSize (kMonthLetters[m]);
        draw->AddText (ImVec2 (x0 + (x1 - x0 - letter.x) * 0.5f, base + 1.0f),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), kMonthLetters[m]);
    }
    if (hoveredMonth >= 0 && period.monthDays[hoveredMonth] > 0.0) {
        const double d = period.monthDays[hoveredMonth];
        const double direct = point->unoccluded[hoveredMonth] / d, facing = point->facing[hoveredMonth] / d;
        ImGui::SetTooltip (
            "%s, a day on average:\n  direct sun   %s\n  in front     %s\n  lost         %s\n  sun up       "
            "%s\n  access       %d%%",
            kMonthNames[hoveredMonth], HoursText (direct).c_str (), HoursText (facing).c_str (),
            HoursText ((std::max) (0.0, facing - direct)).c_str (),
            HoursText (period.siteDaylight[hoveredMonth] / d).c_str (),
            facing > 0.0 ? int (100.0 * direct / facing + 0.5) : 0);
    }
}

// The point's days by their hours of direct sun (the threshold bins), the
// bins at or above the threshold in the map's colour.
void DaysHistogram (const sunstudyhud::PeriodInfo& period, const sunstudyhud::PeriodPoint* point, int threshold,
                    const Ramp& ramp)
{
    bool hovered = false;
    const float height = BarsHeight ();
    const ImVec2 start = ChartFrame ("##perioddays", height + ImGui::GetTextLineHeight (), hovered,
                                     point == nullptr ? "no study point" : nullptr);
    if (point == nullptr)
        return;
    double top = 1.0, above = 0.0;
    for (int b = 0; b < 14; ++b) {
        top = (std::max) (top, double (point->daily[b]));
        if (b > threshold)
            above += point->daily[b];
    }
    const double share = period.representedDays > 0.0 ? above / period.representedDays : 0.0;
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float column = ImGui::GetContentRegionAvail ().x / 14.0f;
    const float base = start.y + height;
    for (int b = 0; b < 14; ++b) {
        const float x0 = start.x + column * float (b) + 1.0f, x1 = x0 + column - 2.0f;
        const float y = base - float (double (point->daily[b]) / top) * height;
        draw->AddRectFilled (ImVec2 (x0, y), ImVec2 (x1, base), b > threshold ? ramp.At (share) : kGhost);
        if (b % 2 == 1) {
            const std::string edge = SunStudyClock (period.edges[b - 1], false);
            draw->AddText (ImVec2 (x0, base + 1.0f), ImGui::GetColorU32 (ImGuiCol_TextDisabled), edge.c_str ());
        }
    }
    const float tx = start.x + column * float (threshold + 1);
    draw->AddLine (ImVec2 (tx, start.y), ImVec2 (tx, base), ImGui::GetColorU32 (ImGuiCol_Text), 1.0f);
    if (hovered) {
        const int b = std::clamp (int ((ImGui::GetIO ().MousePos.x - start.x) / column), 0, 13);
        const std::string from = b == 0 ? "0:00" : SunStudyClock (period.edges[b - 1], false);
        const std::string to = b == 13 ? "" : SunStudyClock (period.edges[b], false);
        ImGui::SetTooltip ("%.0f day(s) with %s%s%s h of direct sun", double (point->daily[b]), from.c_str (),
                           b == 13 ? "+" : " to ", to.c_str ());
    }
}

// One legend entry: its swatch and word, its meaning on hover.
void LegendItem (ImU32 colour, const char* text, const char* meaning)
{
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const float size = ImGui::GetTextLineHeight () * 0.7f;
    ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (at.x, at.y + size * 0.2f),
                                                ImVec2 (at.x + size, at.y + size * 1.2f), colour);
    ImGui::Dummy (ImVec2 (size, size));
    ImGui::SameLine (0.0f, 4.0f);
    ImGui::TextDisabled ("%s", text);
    if (meaning != nullptr && ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", meaning);
    ImGui::SameLine ();
}

constexpr const char* kMeanDirect = "Direct sun: hours with a clear line to the sun.";
constexpr const char* kMeanFront = "In front: hours the sun is on the surface's side -- the most it could get with "
                                   "nothing in the way. Depends only on orientation.";
constexpr const char* kMeanUp = "Sun up: hours the sun is above the horizon in the study's time slice. The same for "
                                "every surface, so it is the ceiling, not a property of the point.";
constexpr const char* kMeanLost = "Lost: hours the sun was in front of the surface but the model blocked it.";
constexpr const char* kMeanAccess = "Access: direct sun as a share of the sun in front -- how much of what it could "
                                    "get it actually gets.";

void BarsLegend (int quantity, const Ramp& ramp)
{
    switch (quantity) {
        case 1:
            LegendItem (ramp.At (0.7), "in front", kMeanFront);
            LegendItem (kGhost, "sun up", kMeanUp);
            break;
        case 2:
            LegendItem (ramp.At (0.7), "lost", kMeanLost);
            LegendItem (kGhost, "in front", kMeanFront);
            break;
        case 3:
            LegendItem (ramp.At (0.7), "access %", kMeanAccess);
            break;
        case 4:
            LegendItem (ramp.At (0.7), "days at or above", "Days whose direct sun reached the threshold (line).");
            LegendItem (kGhost, "below", "Days below the threshold.");
            break;
        default:
            LegendItem (ramp.At (0.7), "direct", kMeanDirect);
            LegendItem (kGhost, "in front", kMeanFront);
            break;
    }
    ImGui::NewLine ();
}

// The profile's cell value for a quantity, as a 0-1 share; < 0 when the sun
// is behind the surface for an access view (no possible sun to share).
double CellShare (int quantity, double up, double facing, double lit)
{
    switch (quantity) {
        case 1:
            return facing / up;
        case 2:
            return (std::max) (0.0, facing - lit) / up;
        case 3:
            return facing > 0.0 ? lit / facing : -1.0;
        default:
            return lit / up;
    }
}

// Month x clock hour in the map's colours: the share of the sun-up time the
// cell had the quantity. Empty where the sun is down.
void ProfileGrid (int quantity, const sunstudyhud::PeriodProfile* profile, const char* note, int shownMonth,
                  const Ramp& ramp)
{
    const float row = ImGui::GetTextLineHeight () * 0.8f;
    bool hovered = false;
    const ImVec2 start = ChartFrame ("##periodprofile", row * 12.0f + ImGui::GetTextLineHeight (), hovered, note);
    if (profile == nullptr)
        return;
    int firstHour = 24, lastHour = -1;
    for (int m = 0; m < 12; ++m)
        for (int h = 0; h < 24; ++h)
            if (profile->up[m][h] > 0.0) {
                firstHour = (std::min) (firstHour, h);
                lastHour = (std::max) (lastHour, h);
            }
    if (lastHour < firstHour)
        return;
    const int hours = lastHour - firstHour + 1;
    const float label = ImGui::CalcTextSize ("M").x + 6.0f;
    const float width = ImGui::GetContentRegionAvail ().x - label;
    const float cell = width / float (hours);
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    for (int m = 0; m < 12; ++m) {
        const float y = start.y + row * float (m);
        draw->AddText (ImVec2 (start.x + 2.0f, y - 1.0f),
                       m + 1 == shownMonth ? ImGui::GetColorU32 (ImGuiCol_Text)
                                           : ImGui::GetColorU32 (ImGuiCol_TextDisabled),
                       kMonthLetters[m]);
        for (int h = firstHour; h <= lastHour; ++h) {
            const double up = profile->up[m][h];
            if (up <= 0.0)
                continue;
            const float x = start.x + label + cell * float (h - firstHour);
            const double share = CellShare (quantity, up, profile->facing[m][h], profile->lit[m][h]);
            draw->AddRectFilled (ImVec2 (x, y), ImVec2 (x + cell - 1.0f, y + row - 1.0f),
                                 share < 0.0 ? kBehind : ramp.At (share));
        }
    }
    for (int h = firstHour; h <= lastHour; h += 3) {
        char text[8];
        std::snprintf (text, sizeof (text), "%d", h);
        draw->AddText (ImVec2 (start.x + label + cell * float (h - firstHour), start.y + row * 12.0f),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), text);
    }
    if (hovered) {
        const ImVec2 mouse = ImGui::GetIO ().MousePos;
        const int m = int ((mouse.y - start.y) / row), h = firstHour + int ((mouse.x - start.x - label) / cell);
        if (m >= 0 && m < 12 && h >= firstHour && h <= lastHour && profile->up[m][h] > 0.0) {
            const double up = profile->up[m][h], facing = profile->facing[m][h], lit = profile->lit[m][h];
            ImGui::SetTooltip ("%s, %02d:00-%02d:00, of the time the sun is up:\n  lit          %d%%\n  shaded       "
                               "%d%%\n  sun behind   %d%%",
                               kMonthNames[m], h, h + 1, int (100.0 * lit / up + 0.5),
                               int (100.0 * (std::max) (0.0, facing - lit) / up + 0.5),
                               int (100.0 * (std::max) (0.0, up - facing) / up + 0.5));
        }
    }
}

// One column per traced day: the quantity's hours that day (or share) in the
// map's colours, over the sun-up hours.
void DayStrip (int quantity, const sunstudyhud::PeriodProfile* profile, int shownDay, const Ramp& ramp)
{
    bool hovered = false;
    const float height = ImGui::GetTextLineHeight () * 3.0f;
    const ImVec2 start = ChartFrame ("##periodlines", height, hovered);
    if (profile == nullptr || profile->dayLit.empty ())
        return;
    const size_t days = profile->dayLit.size ();
    double top = 1.0;
    for (const double up : profile->dayUp)
        top = (std::max) (top, up);
    const bool share = quantity == 3;
    const float column = ImGui::GetContentRegionAvail ().x / float (days);
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float base = start.y + height;
    const auto value = [&] (size_t d) {
        const double facing = profile->dayFacing[d], lit = profile->dayLit[d];
        switch (quantity) {
            case 1:
                return facing;
            case 2:
                return (std::max) (0.0, facing - lit);
            case 3:
                return facing > 0.0 ? lit / facing : 0.0;
            default:
                return lit;
        }
    };
    for (size_t d = 0; d < days; ++d) {
        const float x0 = start.x + column * float (d),
                    x1 = x0 + (std::max) (1.0f, column - (column > 3.0f ? 1.0f : 0.0f));
        if (!share)
            draw->AddRectFilled (ImVec2 (x0, base - float (profile->dayUp[d] / top) * height), ImVec2 (x1, base),
                                 kGhost);
        const double v = value (d);
        const double fraction = share ? v : v / top;
        draw->AddRectFilled (ImVec2 (x0, base - float (fraction) * height), ImVec2 (x1, base), ramp.At (fraction));
    }
    if (shownDay >= 0 && size_t (shownDay) < days) {
        const float x = start.x + column * (float (shownDay) + 0.5f);
        draw->AddLine (ImVec2 (x, start.y), ImVec2 (x, base), ImGui::GetColorU32 (ImGuiCol_Text), 1.0f);
    }
    if (hovered) {
        const size_t d = (std::min) (days - 1, size_t ((std::max) (0.0f, ImGui::GetIO ().MousePos.x - start.x) /
                                                       (std::max) (column, 0.001f)));
        ImGui::SetTooltip ("%s %d: %s direct, %s in front, %s sun up",
                           kMonthNames[std::clamp (profile->dayMonth[d], 1, 12) - 1], profile->dayOfMonth[d],
                           HoursText (profile->dayLit[d]).c_str (), HoursText (profile->dayFacing[d]).c_str (),
                           HoursText (profile->dayUp[d]).c_str ());
    }
}

// Detailed playback of only the placed point's re-traced cells. This has its
// own timeline and never changes the whole-map PeriodView.
void PointPlayback (HudState& state, const sunstudyhud::PeriodProfile* profile, const char* note)
{
    const float half = (ImGui::GetContentRegionAvail ().x - ImGui::GetStyle ().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth (half);
    const int mode = std::clamp (state.sunPointPlayMode, 0, 2);
    if (ImGui::BeginCombo ("##pointplaymode", kPlayModes[mode])) {
        for (int m = 0; m < 3; ++m)
            if (ImGui::Selectable (kPlayModes[m], m == mode)) {
                state.sunPointPlayMode = m;
                state.sunPointPlay.playing = false;
                state.sunPointPlayStep = -1;
            }
        ImGui::EndCombo ();
    }
    ImGui::SetItemTooltip ("Detailed playback of the study point only. It uses the on-demand re-trace and needs no "
                           "whole-map day archive.");
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (-1.0f);
    ImGui::BeginDisabled (mode != kFixedHour);
    int quarter = std::clamp (state.sunPointPlayClock / 15, 0, 95);
    if (ImGui::SliderInt ("##pointplayclock", &quarter, 0, 95, ClockText (quarter * 15).c_str ()))
        state.sunPointPlayClock = quarter * 15;
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip ("Fixed hour: the nearest measured step at this clock time on each traced day.");

    if (profile == nullptr || profile->dayLit.empty ()) {
        state.sunPointPlay.playing = false;
        state.sunPointPlayStep = -1;
        int empty = 0;
        DrawSunStudyTimeline ("pointperiodempty", empty, 0, 0, state.sunPointPlay, kStepSeconds, note, true);
        DrawSunStudyDayClock (state, nullptr, "##pointperioddayclock", -1, note, nullptr, 440.0f);
        return;
    }

    const int last = int (profile->dayLit.size ()) - 1;
    if (mode == kEveryStep) {
        const auto inSlice = [&state] (uint16_t minute) {
            return double (minute) >= state.sunPointPlayFrom && double (minute) < state.sunPointPlayTo;
        };
        std::vector<int> first (profile->dayLit.size () + 1, 0);
        for (size_t d = 0; d < profile->dayLit.size (); ++d) {
            int count = 0;
            if (d < profile->dayMinutes.size ())
                for (const uint16_t minute : profile->dayMinutes[d])
                    count += inSlice (minute) ? 1 : 0;
            first[d + 1] = first[d] + count;
        }
        const int total = first.back ();
        if (total == 0) {
            state.sunPointPlay.playing = false;
            state.sunPointPlayStep = -1;
            DrawSunStudyTimeline ("pointperiodsteps", state.sunPointPlayDay, 0, 0, state.sunPointPlay, kStepSeconds,
                                  "no measured step in this slice", true);
        }
        else {
            const int day = std::clamp (state.sunPointPlayDay, 0, last);
            int withinDay = 0;
            if (state.sunPointPlayStep >= 0 && size_t (day) < profile->dayMinutes.size ())
                for (size_t s = 0; s < profile->dayMinutes[size_t (day)].size (); ++s) {
                    if (int (s) == state.sunPointPlayStep)
                        break;
                    withinDay += inSlice (profile->dayMinutes[size_t (day)][s]) ? 1 : 0;
                }
            int index = std::clamp (first[size_t (day)] + withinDay, 0, total - 1);
            const auto locate = [&] (int at, int& locatedDay, int& step) {
                locatedDay = int (std::upper_bound (first.begin (), first.end (), at) - first.begin ()) - 1;
                int remaining = at - first[size_t (locatedDay)];
                step = -1;
                for (size_t s = 0; s < profile->dayMinutes[size_t (locatedDay)].size (); ++s)
                    if (inSlice (profile->dayMinutes[size_t (locatedDay)][s])) {
                        if (remaining == 0) {
                            step = int (s);
                            break;
                        }
                        --remaining;
                    }
            };
            int locatedDay = 0, step = 0;
            const auto label = [&] {
                int labelDay = 0, labelStep = 0;
                locate (index, labelDay, labelStep);
                const auto& minutes = profile->dayMinutes[size_t (labelDay)];
                char text[80];
                std::snprintf (text, sizeof (text), "%s %d  %s",
                               kMonthShort[std::clamp (profile->dayMonth[size_t (labelDay)], 1, 12) - 1],
                               profile->dayOfMonth[size_t (labelDay)],
                               ClockText (minutes[size_t (labelStep)]).c_str ());
                return std::string (text);
            };
            DrawSunStudyTimeline ("pointperiodsteps", index, 0, total - 1, state.sunPointPlay, kStepSeconds, label);
            locate (index, locatedDay, step);
            state.sunPointPlayDay = locatedDay;
            state.sunPointPlayStep = step;
        }
    }
    else {
        int day = std::clamp (state.sunPointPlayDay, 0, last);
        const auto label = [&] {
            char text[80];
            if (mode == kFixedHour)
                std::snprintf (text, sizeof (text), "%s %d  %s",
                               kMonthShort[std::clamp (profile->dayMonth[size_t (day)], 1, 12) - 1],
                               profile->dayOfMonth[size_t (day)], ClockText (state.sunPointPlayClock).c_str ());
            else
                std::snprintf (text, sizeof (text), "%s %d  (%d / %d)",
                               kMonthShort[std::clamp (profile->dayMonth[size_t (day)], 1, 12) - 1],
                               profile->dayOfMonth[size_t (day)], day + 1, last + 1);
            return std::string (text);
        };
        DrawSunStudyTimeline ("pointperioddays", day, 0, last, state.sunPointPlay, kDayStepSeconds, label);
        state.sunPointPlayDay = day;
        if (mode == kFixedHour && size_t (day) < profile->dayMinutes.size () &&
            size_t (day) < profile->dayHoursPerStep.size ()) {
            const int step = StepAt (profile->dayMinutes[size_t (day)], profile->dayHoursPerStep[size_t (day)],
                                     state.sunPointPlayClock);
            state.sunPointPlayStep = step < int (profile->dayMinutes[size_t (day)].size ()) ? step : -1;
        }
        else
            state.sunPointPlayStep = -1;
    }

    const size_t selected = size_t (std::clamp (state.sunPointPlayDay, 0, last));
    evp::sunstudy::PointDay day = sunstudyhud::PeriodPointDay (*profile, selected);
    const char* empty = day.minutes.empty () ? "no sun this day" : "";
    SunStudyClockSelection selection;
    const SunStudyClockShown shown { state.sunPointPlayFrom, state.sunPointPlayTo };
    DrawSunStudyDayClock (state, &day, "##pointperioddayclock", state.sunPointPlayStep, empty, &selection, 440.0f,
                          &shown, SunChart::PeriodDay);
    if (selection.step >= 0) {
        state.sunPointPlayMode = kEveryStep;
        state.sunPointPlayStep = selection.step;
        const double at = day.minutes[size_t (selection.step)]; // outside the kept slice: widen to the day
        if (at < state.sunPointPlayFrom || at >= state.sunPointPlayTo)
            state.sunPointPlayFrom = 0.0, state.sunPointPlayTo = 1440.0;
        state.sunPointPlay.playing = false;
    }
    else if (selection.slice) {
        state.sunPointPlayMode = kEveryStep;
        state.sunPointPlayFrom = selection.fromMinute;
        state.sunPointPlayTo = selection.toMinute;
        state.sunPointPlayStep = -1;
        state.sunPointPlay.playing = false;
        for (size_t s = 0; s < day.minutes.size (); ++s)
            if (double (day.minutes[s]) >= selection.fromMinute && double (day.minutes[s]) < selection.toMinute) {
                state.sunPointPlayStep = int (s);
                break;
            }
    }
    else if (selection.clear) {
        state.sunPointPlayFrom = 0.0;
        state.sunPointPlayTo = 1440.0;
        state.sunPointPlay.playing = false;
    }
}

void PointSection (HudState& state, const sunstudyhud::PeriodInfo& period, int quantity,
                   const std::vector<sunstudyhud::KeptDay>* kept)
{
    if (!ImGui::CollapsingHeader ("Study point", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const bool placed = DrawSunStudyPointHeader (state, period.studyId);
    sunstudyhud::PeriodPoint point;
    const bool read = placed && sunstudyhud::ReadPeriodPoints (state.sunPointSamples, point);
    Ramp ramp { SunStudyRampStops (state.sunGradientPreset, state.sunGradientCustom, state.sunGradientFlipped) };
    const int shownMonth = ShownMonth (period, kept);
    if (quantity == 4)
        DaysHistogram (period, read ? &point : nullptr, std::clamp (state.sunPeriodThreshold, 0, 12), ramp);
    else
        MonthBars (quantity, period, read ? &point : nullptr, shownMonth, ramp);
    BarsLegend (quantity, ramp);

    // The period in a line: totals, best and worst month, threshold days.
    char line[200] = "";
    if (read) {
        double direct = 0.0, facing = 0.0;
        int best = -1, worst = -1;
        for (int m = 0; m < 12; ++m) {
            direct += point.unoccluded[m];
            facing += point.facing[m];
            if (period.monthDays[m] <= 0.0)
                continue;
            const double perDay = point.unoccluded[m] / period.monthDays[m];
            if (best < 0 || perDay > point.unoccluded[best] / period.monthDays[best])
                best = m;
            if (worst < 0 || perDay < point.unoccluded[worst] / period.monthDays[worst])
                worst = m;
        }
        std::snprintf (line, sizeof (line), "%.0f h direct (%d%% of %.0f h in front); best %s, worst %s", direct,
                       facing > 0.0 ? int (100.0 * direct / facing + 0.5) : 0, facing,
                       best >= 0 ? kMonthShort[best] : "-", worst >= 0 ? kMonthShort[worst] : "-");
        const auto daysFrom = [&point] (int edge) {
            double days = 0.0;
            for (int bin = edge + 1; bin < 14; ++bin)
                days += point.daily[bin];
            return days;
        };
        char detail[200];
        std::snprintf (detail, sizeof (detail),
                       "Days with at least 1:30 h: %.0f, 2:00 h: %.0f, 3:00 h: %.0f, 4:00 h: %.0f -- of %.0f days.%s",
                       daysFrom (2), daysFrom (3), daysFrom (5), daysFrom (7), period.representedDays,
                       period.estimate ? " Preview: each traced day stands for its month, an estimate." : "");
        SunStudyNote (line, detail);
    }
    else
        SunStudyNote (" ");

    ImGui::Checkbox ("Detailed point playback (re-trace)", &state.sunPeriodProfile);
    ImGui::SetItemTooltip ("Re-traces only the study point's cells against every traced sun position: a month x hour "
                           "map, a 24-hour clock, and every-step / fixed-hour / day-by-day playback. Runs on the CPU "
                           "and does not keep a whole-map day archive.");
    if (!state.sunPeriodProfile) {
        state.sunPointPlay.playing = false;
        return;
    }
    const sunstudyhud::PeriodProfile* shown = nullptr;
    std::shared_ptr<const sunstudyhud::PeriodProfile> profile;
    const char* note = "no study point";
    if (read) {
        sunstudyhud::RequestPeriodProfile (state.sunPointSamples);
        profile = sunstudyhud::LatestPeriodProfile ();
        if (profile == nullptr || profile->samples != state.sunPointSamples || !profile->ready)
            note = "tracing...";
        else if (!profile->error.empty ())
            note = "not traced (hover)";
        else {
            shown = profile.get ();
            note = nullptr;
        }
    }
    PointPlayback (state, shown, note);
    int pointMonth = shownMonth;
    if (shown != nullptr && state.sunPointPlayDay >= 0 && size_t (state.sunPointPlayDay) < shown->dayMonth.size ())
        pointMonth = shown->dayMonth[size_t (state.sunPointPlayDay)];
    ProfileGrid (quantity == 4 ? 0 : quantity, shown, note, pointMonth, ramp);
    if (profile != nullptr && !profile->error.empty () && ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", profile->error.c_str ());
    const int shownDay = shown != nullptr ? state.sunPointPlayDay : -1;
    DayStrip (quantity == 4 ? 0 : quantity, shown, shownDay, ramp);
    if (shown != nullptr) {
        std::snprintf (line, sizeof (line), "%zu cell(s), %zu traced days, %zu rays, %.0f ms%s", shown->samples.size (),
                       shown->dayLit.size (), shown->rays, shown->milliseconds, period.estimate ? "; preview" : "");
        SunStudyNote (
            line, "Clock and player: the selected traced day at the study's original timestep. Grid: month by clock "
                  "hour, coloured by the share of sun-up time. Strip: one column a traced day. Preview plays "
                  "only its representative days; their weights do not create extra frames.");
    }
    else
        SunStudyNote (" ");
    DrawSunStudySeasonalClocks (state, shown, period.estimate);
}

} // namespace

void DrawSunStudyPeriodView (HudState& state)
{
    const sunstudyhud::PeriodInfo period = sunstudyhud::CurrentPeriod ();
    // A freed cache leaves the frame's quantity, month, colours and legend intact.
    if (state.sunShowsPeriod && !period.available)
        return;
    if (!state.sunShowsPeriod || period.studyId != state.sunPeriodStudyId) {
        // Back on a day study: its words and its colours.
        SunLegendUnit () = SunValueUnit::Hours;
        if (state.sunPeriodPresetsFor != 0)
            SwitchColours (state, 0);
        state.sunPeriodQuantity = state.sunPeriodMonth = state.sunPeriodDay = state.sunPeriodStep = 0;
        state.sunPeriodPlay.playing = false;
        return;
    }
    const std::shared_ptr<const std::vector<sunstudyhud::KeptDay>> keptDays = sunstudyhud::KeptPeriodDays ();
    const std::vector<sunstudyhud::KeptDay>* kept =
        period.daysKept && keptDays != nullptr && !keptDays->empty () ? keptDays.get () : nullptr;
    if (kept == nullptr)
        state.sunPeriodPlayMode = kMonthByMonth;
    const int mode = std::clamp (state.sunPeriodPlayMode, 0, 3);
    QuantityButtons (state, mode);
    const int quantity = std::clamp (state.sunPeriodQuantity, 0, kQuantityCount - 1);
    SunLegendUnit () = IsShare (quantity) && !Instant (mode) ? SunValueUnit::Share : SunValueUnit::Hours;
    PlayModeRow (state, period);
    sunstudyhud::PeriodView want;
    want.quantity = Instant (mode) ? 0 : quantity;
    want.threshold = state.sunPeriodThreshold;
    want.scale = IsShare (quantity) || Instant (mode) ? double (kSunRampTop) : 1.0;
    if (quantity == 4) {
        ThresholdChoice (state, period);
        state.sunPeriodMonth = 0;
        state.sunPeriodPlay.playing = false;
    }
    else if (mode == kMonthByMonth || kept == nullptr)
        MonthTimeline (state, period, want);
    else
        DayTimeline (state, period, *kept, want);
    char line[96];
    std::snprintf (line, sizeof (line), "%s%s",
                   Instant (mode) ? "Lit or not at this instant" : kQuantities[quantity].line,
                   period.busy ? "  (updating...)" : "");
    SunStudyNote (line, Instant (mode) ? "An instant of one kept day: a surface is drawn at the ramp's top where it "
                                         "has a clear line to the sun, at its bottom where it does not."
                                       : kQuantities[quantity].meaning);

    // Asked once per change: a refused view is reported, not retried each frame.
    static sunstudyhud::PeriodView asked;
    static std::string askedFor;
    if (askedFor != period.studyId) {
        asked = period.applied;
        askedFor = period.studyId;
    }
    if (want != asked) {
        std::string error;
        if (sunstudyhud::ShowPeriodView (want, error))
            asked = want;
        else
            hudshell::ErrorNotice (error);
    }
    if (!period.error.empty ())
        hudshell::ErrorNotice ("View not shown: " + period.error);
    PointSection (state, period, Instant (mode) ? 0 : quantity, kept);
}

} // namespace geomsrv::archviz
