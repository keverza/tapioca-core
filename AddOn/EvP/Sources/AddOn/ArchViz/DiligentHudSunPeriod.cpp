// The finished period's view in the sun study panel (#152): which quantity the
// map shows (each with its own colours), the month on screen and its player,
// the threshold of the days-at-or-above map, and the hovered point's year --
// its months from the stored totals, and on demand its month x hour profile
// re-traced against the period's own suns.
//
// ⚠️ ITS OWN TRANSLATION UNIT, for the reason DiligentHudSunRun.cpp is: the HUD
// files sit near the size cap, and this is one concern -- looking through a
// finished period, where that file asks for one.
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

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv::archviz {

namespace {

constexpr const char* kMonthNames[] = { "January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December" };
constexpr const char* kMonthLetters[] = { "J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D" };
constexpr double kMonthStepSeconds = 0.8; // the month player's 1x

struct Quantity {
    const char* button;
    const char* meaning;
};

// The order is the runner's PeriodView::quantity.
constexpr Quantity kQuantities[] = {
    { "Direct", "Direct sun: hours a day with an unobstructed view of the sun (mean of the days shown)." },
    { "Possible", "Possible sun: hours a day the sun is in front of the surface -- the most it could get with nothing "
                  "in the way." },
    { "Lost", "Lost to obstruction: hours a day the sun was in front of the surface but blocked by the model." },
    { "Access", "Sun access: the share of its possible sun a surface gets. Independent of orientation, so facades and "
                "roofs compare." },
    { "Days", "Days at or above: the share of the period's days with at least the chosen hours of direct sun." },
};
constexpr int kQuantityCount = int (std::size (kQuantities));

bool IsShare (int quantity)
{
    return quantity == 3 || quantity == 4;
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

void QuantityButtons (HudState& state)
{
    const float spacing = ImGui::GetStyle ().ItemSpacing.x * 0.5f;
    const float width =
        (ImGui::GetContentRegionAvail ().x - spacing * float (kQuantityCount - 1)) / float (kQuantityCount);
    for (int q = 0; q < kQuantityCount; ++q) {
        if (q > 0)
            ImGui::SameLine (0.0f, spacing);
        const bool active = state.sunPeriodQuantity == q;
        ImGui::PushStyleColor (ImGuiCol_Button,
                               ImGui::GetStyleColorVec4 (active ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        ImGui::PushID (q);
        if (ImGui::Button (kQuantities[q].button, ImVec2 (width, 0.0f)) && !active) {
            state.sunPeriodQuantity = q;
            SwitchColours (state, q);
        }
        ImGui::PopID ();
        ImGui::PopStyleColor ();
        ImGui::SetItemTooltip ("%s", kQuantities[q].meaning);
    }
}

std::string HoursText (double hours)
{
    return SunStudyClock (hours, true);
}

// The month timeline: "whole period" and every month the period reaches.
void MonthTimeline (HudState& state, const sunstudyhud::PeriodInfo& period)
{
    std::vector<int> months { 0 };
    for (int m = 1; m <= 12; ++m)
        if (period.monthDays[m - 1] > 0.0)
            months.push_back (m);
    int index = 0;
    for (int i = 0; i < int (months.size ()); ++i)
        if (months[i] == state.sunPeriodMonth)
            index = i;
    char label[64];
    if (months[index] == 0)
        std::snprintf (label, sizeof (label), "Whole period (%.0f days)", period.representedDays);
    else
        std::snprintf (label, sizeof (label), "%s (%.0f days)", kMonthNames[months[index] - 1],
                       period.monthDays[months[index] - 1]);
    // The player runs through the months only, not back to the whole period.
    DrawSunStudyTimeline ("periodmonths", index, 0, int (months.size ()) - 1, state.sunPeriodPlay, kMonthStepSeconds,
                          label, period.busy);
    if (state.sunPeriodPlay.playing && index == 0 && months.size () > 1)
        index = 1;
    state.sunPeriodMonth = months[index];
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

// ---- the hovered point ------------------------------------------------------

ImU32 Mix (ImU32 a, ImU32 b, float t)
{
    t = std::clamp (t, 0.0f, 1.0f);
    const auto channel = [&] (int shift) {
        const float x = float ((a >> shift) & 0xff), y = float ((b >> shift) & 0xff);
        return ImU32 (x + (y - x) * t + 0.5f) << shift;
    };
    return channel (IM_COL32_R_SHIFT) | channel (IM_COL32_G_SHIFT) | channel (IM_COL32_B_SHIFT) |
           (ImU32 (255) << IM_COL32_A_SHIFT);
}

constexpr ImU32 kSunUp = IM_COL32 (70, 74, 82, 255);
constexpr ImU32 kFacing = IM_COL32 (122, 112, 92, 255);
constexpr ImU32 kDirect = IM_COL32 (240, 178, 64, 255);
constexpr ImU32 kShaded = IM_COL32 (54, 62, 92, 255);

void Legend (ImU32 colour, const char* text)
{
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const float size = ImGui::GetTextLineHeight () * 0.7f;
    ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (at.x, at.y + size * 0.2f),
                                                ImVec2 (at.x + size, at.y + size * 1.2f), colour);
    ImGui::Dummy (ImVec2 (size, size));
    ImGui::SameLine (0.0f, 4.0f);
    ImGui::TextDisabled ("%s", text);
}

// Twelve columns, each three nested bars: sun up, sun in front, direct sun --
// hours a day. A month the period does not reach is left empty.
void MonthBars (const sunstudyhud::PeriodInfo& period, const sunstudyhud::PeriodPoint& point, int selectedMonth)
{
    double top = 1.0;
    for (int m = 0; m < 12; ++m)
        if (period.monthDays[m] > 0.0)
            top = (std::max) (top, period.siteDaylight[m] / period.monthDays[m]);
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetTextLineHeight () * 5.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##periodbars", ImVec2 (width, height + ImGui::GetTextLineHeight ()));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const float column = width / 12.0f;
    const float base = start.y + height;
    int hovered = -1;
    if (ImGui::IsItemHovered ())
        hovered = std::clamp (int ((ImGui::GetIO ().MousePos.x - start.x) / column), 0, 11);
    for (int m = 0; m < 12; ++m) {
        const float x0 = start.x + column * float (m) + 2.0f, x1 = x0 + column - 4.0f;
        if (m + 1 == selectedMonth || m == hovered)
            draw->AddRectFilled (ImVec2 (x0 - 2.0f, start.y), ImVec2 (x1 + 2.0f, base), IM_COL32 (255, 255, 255, 18));
        const double days = period.monthDays[m];
        if (days > 0.0) {
            const auto bar = [&] (double perDay, ImU32 colour, float inset) {
                const float y = base - float (perDay / top) * height;
                draw->AddRectFilled (ImVec2 (x0 + inset, y), ImVec2 (x1 - inset, base), colour);
            };
            bar (period.siteDaylight[m] / days, kSunUp, 0.0f);
            bar (point.facing[m] / days, kFacing, 1.0f);
            bar (point.unoccluded[m] / days, kDirect, 3.0f);
        }
        const ImVec2 letter = ImGui::CalcTextSize (kMonthLetters[m]);
        draw->AddText (ImVec2 (x0 + (x1 - x0 - letter.x) * 0.5f, base + 1.0f),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), kMonthLetters[m]);
    }
    if (hovered >= 0 && period.monthDays[hovered] > 0.0) {
        const double days = period.monthDays[hovered];
        const double direct = point.unoccluded[hovered] / days, facing = point.facing[hovered] / days;
        ImGui::SetTooltip (
            "%s: %s a day direct sun, %s in front, %s sun up%s", kMonthNames[hovered], HoursText (direct).c_str (),
            HoursText (facing).c_str (), HoursText (period.siteDaylight[hovered] / days).c_str (),
            facing > 0.0 ? (" -- " + std::to_string (int (100.0 * direct / facing + 0.5)) + "% access").c_str () : "");
    }
}

// Month x clock hour, coloured by the share of sun-up time it was lit: amber
// lit, blue shaded, grey behind the surface, empty when the sun is down.
void ProfileGrid (const sunstudyhud::PeriodProfile& profile)
{
    int firstHour = 24, lastHour = -1;
    for (int m = 0; m < 12; ++m)
        for (int h = 0; h < 24; ++h)
            if (profile.up[m][h] > 0.0) {
                firstHour = (std::min) (firstHour, h);
                lastHour = (std::max) (lastHour, h);
            }
    if (lastHour < firstHour) {
        ImGui::TextDisabled ("No sun in the period's time slice.");
        return;
    }
    const int hours = lastHour - firstHour + 1;
    const float label = ImGui::CalcTextSize ("M").x + 6.0f;
    const float width = ImGui::GetContentRegionAvail ().x - label;
    const float cell = width / float (hours);
    const float row = ImGui::GetTextLineHeight () * 0.8f;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##periodprofile", ImVec2 (label + width, row * 12.0f + ImGui::GetTextLineHeight ()));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    for (int m = 0; m < 12; ++m) {
        const float y = start.y + row * float (m);
        draw->AddText (ImVec2 (start.x, y - 1.0f), ImGui::GetColorU32 (ImGuiCol_TextDisabled), kMonthLetters[m]);
        for (int h = firstHour; h <= lastHour; ++h) {
            const double up = profile.up[m][h];
            if (up <= 0.0)
                continue;
            const float x = start.x + label + cell * float (h - firstHour);
            ImU32 colour = kSunUp;
            if (profile.facing[m][h] > 0.0)
                colour = Mix (kShaded, kDirect, float (profile.lit[m][h] / profile.facing[m][h]));
            draw->AddRectFilled (ImVec2 (x, y), ImVec2 (x + cell - 1.0f, y + row - 1.0f), colour);
        }
    }
    for (int h = firstHour; h <= lastHour; h += 3) {
        char text[8];
        std::snprintf (text, sizeof (text), "%d", h);
        draw->AddText (ImVec2 (start.x + label + cell * float (h - firstHour), start.y + row * 12.0f),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), text);
    }
    if (ImGui::IsItemHovered ()) {
        const ImVec2 mouse = ImGui::GetIO ().MousePos;
        const int m = int ((mouse.y - start.y) / row), h = firstHour + int ((mouse.x - start.x - label) / cell);
        if (m >= 0 && m < 12 && h >= firstHour && h <= lastHour && profile.up[m][h] > 0.0) {
            if (profile.facing[m][h] <= 0.0)
                ImGui::SetTooltip ("%s, %02d:00: the sun is behind the surface", kMonthNames[m], h);
            else
                ImGui::SetTooltip ("%s, %02d:00: lit %d%% of the time the sun is in front", kMonthNames[m], h,
                                   int (100.0 * profile.lit[m][h] / profile.facing[m][h] + 0.5));
        }
    }
}

// One line per traced day: hours of direct sun (amber) under the hours the sun
// is up (grey) -- the shape of the point's year.
void DayLines (const sunstudyhud::PeriodProfile& profile)
{
    const size_t days = profile.dayLit.size ();
    if (days < 2)
        return;
    double top = 1.0;
    for (const double up : profile.dayUp)
        top = (std::max) (top, up);
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetTextLineHeight () * 3.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##periodlines", ImVec2 (width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const auto point = [&] (size_t day, double hours) {
        return ImVec2 (start.x + width * float (day) / float (days - 1),
                       start.y + height - float (hours / top) * height);
    };
    for (size_t d = 1; d < days; ++d) {
        draw->AddLine (point (d - 1, profile.dayUp[d - 1]), point (d, profile.dayUp[d]), kSunUp, 1.5f);
        draw->AddLine (point (d - 1, profile.dayLit[d - 1]), point (d, profile.dayLit[d]), kDirect, 1.5f);
    }
    if (ImGui::IsItemHovered ()) {
        const size_t d = (std::min) (days - 1, size_t (std::max (0.0f, (ImGui::GetIO ().MousePos.x - start.x) / width) *
                                                           float (days - 1) +
                                                       0.5f));
        ImGui::SetTooltip ("%s %d: %s direct of %s sun up", kMonthNames[std::clamp (profile.dayMonth[d], 1, 12) - 1],
                           profile.dayOfMonth[d], HoursText (profile.dayLit[d]).c_str (),
                           HoursText (profile.dayUp[d]).c_str ());
    }
}

void PointSection (HudState& state, const sunstudyhud::PeriodInfo& period)
{
    // The last measured point hovered, unless pinned: the panel is out of the
    // view, so moving the mouse to it must not lose the point.
    if (!state.sunPeriodPointPinned && state.sunReadingState == 1 && state.sunReadingSample != SIZE_MAX)
        state.sunPeriodPointSample = state.sunReadingSample;
    if (!ImGui::CollapsingHeader ("Point through the period", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    sunstudyhud::PeriodPoint point;
    if (state.sunPeriodPointSample == SIZE_MAX || !sunstudyhud::ReadPeriodPoint (state.sunPeriodPointSample, point)) {
        ImGui::TextDisabled ("Inspect a measured surface to see its year here.");
        return;
    }
    ImGui::Checkbox ("Pin this point", &state.sunPeriodPointPinned);
    ImGui::SetItemTooltip ("Keep showing this point while the cursor moves elsewhere.");
    MonthBars (period, point, state.sunPeriodMonth);
    Legend (kDirect, "direct");
    ImGui::SameLine ();
    Legend (kFacing, "in front");
    ImGui::SameLine ();
    Legend (kSunUp, "sun up");
    // The year in a line: totals, the best and the worst month, threshold days.
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
    ImGui::TextWrapped ("%.0f h direct sun over the period (%d%% of %.0f h in front). Best %s, %s a day; worst %s, %s "
                        "a day.",
                        direct, facing > 0.0 ? int (100.0 * direct / facing + 0.5) : 0, facing,
                        best >= 0 ? kMonthNames[best] : "-",
                        best >= 0 ? HoursText (point.unoccluded[best] / period.monthDays[best]).c_str () : "-",
                        worst >= 0 ? kMonthNames[worst] : "-",
                        worst >= 0 ? HoursText (point.unoccluded[worst] / period.monthDays[worst]).c_str () : "-");
    const auto daysFrom = [&point] (int edge) {
        double days = 0.0;
        for (int bin = edge + 1; bin < 14; ++bin)
            days += point.daily[bin];
        return days;
    };
    ImGui::TextDisabled ("Days with at least 1:30h %.0f, 2:00h %.0f, 3:00h %.0f, 4:00h %.0f of %.0f", daysFrom (2),
                         daysFrom (3), daysFrom (5), daysFrom (7), period.representedDays);

    ImGui::Checkbox ("When is it lit? (re-trace)", &state.sunPeriodProfile);
    ImGui::SetItemTooltip ("Re-traces this one point against every traced sun position of the period: a month x hour "
                           "map of when it is lit, and its hours day by day.");
    if (!state.sunPeriodProfile)
        return;
    sunstudyhud::RequestPeriodProfile (state.sunPeriodPointSample);
    const sunstudyhud::PeriodProfile profile = sunstudyhud::LatestPeriodProfile ();
    if (profile.sample != state.sunPeriodPointSample || !profile.ready) {
        ImGui::TextDisabled ("Tracing...");
        return;
    }
    if (!profile.error.empty ()) {
        hudshell::ErrorNotice (profile.error);
        return;
    }
    ProfileGrid (profile);
    Legend (kDirect, "lit");
    ImGui::SameLine ();
    Legend (kShaded, "shaded");
    ImGui::SameLine ();
    Legend (kSunUp, "behind");
    DayLines (profile);
    ImGui::TextDisabled ("%zu traced days, %zu rays, %.0f ms%s", profile.dayLit.size (), profile.rays,
                         profile.milliseconds, period.estimate ? "; preview: one day a month" : "");
}

} // namespace

void DrawSunStudyPeriodView (HudState& state)
{
    const sunstudyhud::PeriodInfo period = sunstudyhud::CurrentPeriod ();
    if (!state.sunShowsPeriod || !period.available || period.studyId != state.sunPeriodStudyId) {
        // Back on a day study: its words and its colours.
        SunLegendUnit () = SunValueUnit::Hours;
        if (state.sunPeriodPresetsFor != 0)
            SwitchColours (state, 0);
        state.sunPeriodQuantity = 0;
        state.sunPeriodMonth = 0;
        state.sunPeriodPlay.playing = false;
        return;
    }
    QuantityButtons (state);
    const int quantity = std::clamp (state.sunPeriodQuantity, 0, kQuantityCount - 1);
    SunLegendUnit () = IsShare (quantity) ? SunValueUnit::Share : SunValueUnit::Hours;
    if (quantity == 4) {
        ThresholdChoice (state, period);
        state.sunPeriodMonth = 0;
        state.sunPeriodPlay.playing = false;
    }
    else
        MonthTimeline (state, period);
    ImGui::PushStyleColor (ImGuiCol_Text, ImGui::GetStyleColorVec4 (ImGuiCol_TextDisabled));
    ImGui::TextWrapped ("%s%s", kQuantities[quantity].meaning, period.busy ? "  (updating...)" : "");
    ImGui::PopStyleColor ();

    sunstudyhud::PeriodView want;
    want.quantity = quantity;
    want.month = state.sunPeriodMonth;
    want.threshold = state.sunPeriodThreshold;
    want.scale = IsShare (quantity) ? double (kSunRampTop) : 1.0;
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
    PointSection (state, period);
}

} // namespace geomsrv::archviz
