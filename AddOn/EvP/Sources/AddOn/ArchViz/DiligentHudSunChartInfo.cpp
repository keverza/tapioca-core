// What each sun study diagram shows, on its info-circle (#152 follow-up): the
// chart's name, what it shows at the analysis point, the data it reads and how
// that was calculated. One table, so the words live in one place and the
// diagram files (near their size cap) carry one call each.
//
// RENDER THREAD.

#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"

#include <imgui.h>

#include <cstring>

namespace geomsrv::archviz {

namespace {

struct ChartText {
    const char* name;
    const char* shows;
    const char* data;
    const char* method;
};

ChartText TextOf (SunChart chart)
{
    switch (chart) {
        case SunChart::LiftedDay:
            return { "Sun day diagram, lifted point",
                     "At the analysis point, raised to the chosen height above the surface it stands on, it shows "
                     "each measured time step from first to last sun: direct sun (yellow) or shadow (blue).",
                     "The day study's sun directions (Archicad's, for the project's location) and its analysis and "
                     "context geometry.",
                     "One ray per step from the raised point toward the sun. The point has no surface, so the sun is "
                     "never behind it. It is kept 10 cm below anything overhead. The centre is the steps in sun times "
                     "the step length." };
        case SunChart::PeriodDay:
            return { "Sun day diagram, period playback",
                     "At the analysis point it shows the selected traced day of the period, step by step: the share of "
                     "the point's cells in direct sun, shadowed by the model, and with the sun behind the surface.",
                     "The period's traced days and their sun directions; the point's cells re-traced on demand (no "
                     "whole-map archive).",
                     "One ray per cell and step toward the sun, culled where the surface faces away. Bars are the "
                     "cells' mean; the centre is the mean direct hours that day." };
        case SunChart::Season:
            return { "Seasonal sun diagram",
                     "At the analysis point it shows a typical day of one season (winter, spring, summer, autumn: Dec-Feb, Mar-May, Jun-Aug, Sep-Nov; swapped south of the equator): "
                     "when in the day the point's cells are lit.",
                     "The re-traced point profile of the period's traced days in that season.",
                     "Each step averages only the days that measured that sun time; the centre is the season's "
                     "weighted mean direct hours a day, including sunless days. Display only." };
        case SunChart::MonthBars:
            return { "Monthly bars",
                     "At the analysis point it shows the chosen quantity month by month (direct sun, sun in front of "
                     "the surface, sun lost to obstruction, or the access share), with what it is a part of behind it.",
                     "The period run's stored per-cell monthly totals, averaged over the point's cells. No new trace.",
                     "Every traced day adds its hours to its month; in Preview each month's middle day stands for the "
                     "whole month (an estimate). Hours are a mean day of the month." };
        case SunChart::DaysHistogram:
            return { "Days at threshold",
                     "At the analysis point it shows how many days of the period reach each daily direct-sun "
                     "threshold.",
                     "The period run's stored per-cell daily-hours bins, averaged over the point's cells.",
                     "Each traced day's direct hours, binned at the thresholds, weighted by the days it represents." };
        case SunChart::ProfileGrid:
            return { "Month x hour grid",
                     "At the analysis point it shows, for each month and clock hour, the share of the time the sun is "
                     "up that the point has the chosen quantity.",
                     "The point's cells re-traced against every traced day's sun directions.",
                     "One ray per cell and step; lit, facing and sun-up hours are summed per month and hour, weighted "
                     "by the days each traced day represents, then divided by sun-up hours." };
        case SunChart::DayStrip:
            return { "Day strip",
                     "At the analysis point it shows one column per traced day: the chosen quantity's hours that day "
                     "over the hours the sun is up (grey behind).",
                     "The point's cells re-traced against every traced day's sun directions.",
                     "Per day, the cells' mean hours of direct sun, of sun in front of the surface, and of sun up." };
        case SunChart::Day:
        default:
            return { "Sun day diagram",
                     "At the analysis point it shows each measured time step from first to last sun: the share of the "
                     "point's cells in direct sun (yellow), shadowed by the model (blue) and with the sun behind the "
                     "surface (grey). The outer rows are each step's main state and the time of day. In the "
                     "Obstruction view's Solar cut, a pink arc with two grips picks the slice to win sun for.",
                     "The single-day study's stored per-step lit bits for the point's cells (or the hovered cell) and "
                     "the sun directions Archicad gave for the project's location and date.",
                     "One ray per cell and step toward the sun, against the analysis and context geometry; a cell "
                     "facing away is 'behind'. Bars are the cells' mean; the centre is the mean direct hours (steps "
                     "lit x step length)." };
    }
}

} // namespace

SunChart SunStudyChartOf (const char* id)
{
    if (id == nullptr)
        return SunChart::Day;
    if (std::strcmp (id, "##periodbars") == 0)
        return SunChart::MonthBars;
    if (std::strcmp (id, "##perioddays") == 0)
        return SunChart::DaysHistogram;
    if (std::strcmp (id, "##periodprofile") == 0)
        return SunChart::ProfileGrid;
    if (std::strcmp (id, "##periodlines") == 0)
        return SunChart::DayStrip;
    return SunChart::Day;
}

bool SunStudyChartInfo (ImVec2 topLeft, float width, SunChart chart)
{
    const float size = ImGui::GetTextLineHeight ();
    const ImVec2 at (topLeft.x + width - size - 2.0f, topLeft.y + 2.0f);
    const bool hovered = ImGui::IsWindowHovered (ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                         ImGui::IsMouseHoveringRect (at, ImVec2 (at.x + size, at.y + size));
    ui::Icon (ui::IconId::InfoCircle, at, size, ui::IconInk (hovered));
    if (!hovered)
        return false;
    const ChartText text = TextOf (chart);
    ImGui::BeginTooltip ();
    ImGui::PushTextWrapPos (ImGui::GetFontSize () * 24.0f);
    ImGui::TextUnformatted (text.name);
    ImGui::TextWrapped ("%s", text.shows);
    ImGui::Spacing ();
    ImGui::TextDisabled ("Data");
    ImGui::TextWrapped ("%s", text.data);
    ImGui::Spacing ();
    ImGui::TextDisabled ("Calculation");
    ImGui::TextWrapped ("%s", text.method);
    ImGui::PopTextWrapPos ();
    ImGui::EndTooltip ();
    return true;
}

} // namespace geomsrv::archviz
