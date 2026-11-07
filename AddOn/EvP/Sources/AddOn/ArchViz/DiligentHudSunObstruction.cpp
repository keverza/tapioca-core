// The sun study's Obstruction view (#167, #168 and the solar cut): what blocks
// the study point at a step, over the day, and what of it must go for sun
// through a chosen slice. Split from DiligentHudSunPoint.cpp for the size cap;
// the work is SunStudyObstructionController's, this file only asks and reads.
//
// RENDER THREAD.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/SunStudyObstructionController.hpp"
#include "ArchViz/SunStudyPreview.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace geomsrv::archviz {

namespace {

std::string MinuteText (double minute)
{
    const int at = std::clamp (int (std::lround (minute)), 0, 1440);
    char text[8];
    std::snprintf (text, sizeof (text), "%02d:%02d", at / 60, at % 60);
    return text;
}

ImU32 Opaque (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
}

} // namespace

namespace {

// The solar cut in numbers: the hours it buys at the point (re-traced after
// the cut), the volume that goes, each element's share, and the cells whose
// shadow it removes.
void DrawSolarCutSummary (const HudState& state, const SunObstructionFrame& frame)
{
    char line[200];
    const bool retraced = frame.cutStage == SunSolarCutStage::Done;
    const std::string after = retraced ? SunStudyClock (frame.litAfterHours, true) : std::string ("...");
    std::snprintf (line, sizeof (line), "%s-%s: %s direct now, %s after the cut (of %s in front)",
                   MinuteText (state.sunCutFrom).c_str (), MinuteText (state.sunCutTo).c_str (),
                   SunStudyClock (frame.litHours, true).c_str (), after.c_str (),
                   SunStudyClock (frame.facingHours, true).c_str ());
    SunStudyNote (line, "Hours of the slice with the sun in front of the point; lit now; and lit with the cut "
                        "elements as they would remain -- traced again, not assumed. Steps with the sun behind the "
                        "surface cannot be won by removing anything.");
    if (frame.facingHours <= 0.0) {
        SunStudyNote ("The sun is never in front of the point in this slice");
        return;
    }
    if (frame.sectors == 0) {
        SunStudyNote ("Widen the slice to two measured steps or more",
                      "The cutting surface is spanned by the rays to consecutive suns; one sun is a line.");
        return;
    }
    if (frame.cutStage == SunSolarCutStage::Surface) {
        SunStudyNote ("Cutting the blockers...", "The cutting surface is shown first; what goes follows, then the "
                                                 "shadows re-traced without it.");
        return;
    }
    if (frame.cutObjects.empty ()) {
        SunStudyNote (frame.litHours < frame.facingHours ? "Nothing analysed or context blocks it above the surface"
                                                         : "Nothing to remove: the slice is already lit");
        return;
    }
    if (retraced)
        std::snprintf (line, sizeof (line), "Remove %.1f m3 from %zu element%s; %zu cell%s gain sun",
                       frame.removedVolume, frame.cutObjects.size (), frame.cutObjects.size () == 1 ? "" : "s",
                       frame.wonCells, frame.wonCells == 1 ? "" : "s");
    else
        std::snprintf (line, sizeof (line), "Remove %.1f m3 from %zu element%s; re-tracing the shadows...",
                       frame.removedVolume, frame.cutObjects.size (), frame.cutObjects.size () == 1 ? "" : "s");
    SunStudyNote (
        line,
        "The rays from the point to the slice's suns are merged into one cutting surface (pink sheet); "
        "everything of the blockers above it goes, extruded up (a see-through pink cut-out, the cut faces darker). The "
        "existing elements keep their outline and edges. Yellow area: study cells whose shadow in the "
        "slice was cast by a cut element and is gone after the cut, merged into one shape.");
    constexpr size_t kRows = 8;
    if (ImGui::BeginTable ("##suncutobjects", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn ("Element", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn ("Removed", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn ("Share", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableHeadersRow ();
        for (size_t row = 0; row < frame.cutObjects.size () && row < kRows; ++row) {
            const SunSolarCutObject& object = frame.cutObjects[row];
            ImGui::TableNextRow ();
            ImGui::TableNextColumn ();
            ImGui::TextUnformatted (object.guid.substr (0, 9).c_str ());
            ImGui::SetItemTooltip ("%s\ntype %d", object.guid.c_str (), int (object.elemType));
            ImGui::TableNextColumn ();
            if (object.closed)
                ImGui::Text ("%.1f m3", object.removed);
            else
                ImGui::TextDisabled ("open mesh");
            ImGui::SetItemTooltip ("%s", object.closed ? "Volume above the cutting surface, from its closed boundary."
                                                       : "The element is not a closed solid: its surfaces above the "
                                                         "cut are shown, but no cut face or volume is invented.");
            ImGui::TableNextColumn ();
            ImGui::Text ("%d%%",
                         object.closed && object.total > 0.0 ? int (100.0 * object.removed / object.total + 0.5) : 0);
        }
        ImGui::EndTable ();
    }
    if (frame.cutObjects.size () > kRows)
        ImGui::TextDisabled ("+%zu more elements", frame.cutObjects.size () - kRows);
}

} // namespace

void DrawSunStudyObstructionControls (HudState& state, const std::string& studyId)
{
    ImGui::RadioButton ("Time step##sunobstruction", &state.sunObstructionMode, 1);
    ImGui::SameLine ();
    ImGui::RadioButton ("Whole day##sunobstruction", &state.sunObstructionMode, 2);
    ImGui::SameLine ();
    ImGui::RadioButton ("Solar cut##sunobstruction", &state.sunObstructionMode, 3);
    ImGui::SetItemTooltip ("Time step: what blocks the study point's sun at the diagram's selected step.\n"
                           "Whole day: every measured step in the selected slice (all of it when none is "
                           "selected).\nSolar cut: what of the blockers must go for direct sun through the pink slice "
                           "on the day diagram.");
    // The prerequisite, made actionable (#167): a hovered cell is one click
    // from being the pinned point the attribution needs.
    if (!state.sunPointPlaced) {
        const bool hovered =
            state.sunHoverDayStudy == studyId && state.sunHoverDaySample != SIZE_MAX && state.sunHoverDayPointValid;
        ImGui::BeginDisabled (!hovered);
        if (ImGui::SmallButton ("Pin hovered cell##sunobstruction"))
            PlaceSunStudyPoint (state, state.sunHoverDayPoint);
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        SunStudyNote (hovered ? "Pin the hovered cell to find what blocks it"
                              : "Hover a measured surface (Inspect on), then pin it",
                      "Obstruction is attributed for a placed study point: its snapped cell, or every cell in its "
                      "radius. Pin the hovered cell here, or right-click a measured surface and place the point.");
        return;
    }
    if (state.sunPointHeight > 0.0f)
        SunStudyNote ("Attributed for the surface cells under the lifted point",
                      "Obstruction footprints belong to measured surface cells; the lifted receiver's diagram is "
                      "traced separately in the Study point section.");
    if (state.sunObstructionMode == 3) {
        if (!(state.sunCutTo > state.sunCutFrom))
            SunStudyNote ("Set the cut slice with the pink grips on the day diagram");
    }
    auto& tasks = SunObstructionTasks ();
    const auto frame = tasks.Latest ();
    if (state.sunObstructionMode == 3 && frame != nullptr && frame->studyId == studyId && frame->error.empty () &&
        SunObstructionFrameCurrent (frame, studyId) && frame->mode == SunObstructionMode::SolarCut) {
        DrawSolarCutSummary (state, *frame);
        return;
    }
    if (state.sunObstructionMode == 3 && (frame == nullptr || frame->mode != SunObstructionMode::SolarCut) &&
        (frame == nullptr || frame->error.empty ())) {
        SunStudyNote (tasks.Busy () ? "Cutting the blockers..." : "Waiting for the cut slice...");
        return;
    }
    if (frame == nullptr || frame->studyId != studyId) {
        SunStudyNote (tasks.Busy () ? "Finding what blocks the point..." : "Waiting for the current study...");
        return;
    }
    if (!frame->error.empty ()) {
        hudshell::ErrorNotice ("Obstruction not shown: " + frame->error);
        return;
    }
    if (!SunObstructionFrameCurrent (frame, studyId)) {
        SunStudyNote ("Obstruction hidden: the scene changed; run the day again.");
        return;
    }
    // What was covered, said rather than implied.
    const bool window = frame->windowFrom > 0 || frame->windowTo < 1440;
    const bool sliced =
        frame->mode == SunObstructionMode::WholeDay && (frame->fromMinute > 0 || frame->toMinute < 1440);
    std::string covered;
    if (frame->mode == SunObstructionMode::TimeStep)
        covered = MinuteText (frame->minute) + " step";
    else if (frame->attributedSteps == 0)
        covered = "no measured step";
    else
        covered = MinuteText (frame->firstMinute) + "-" + MinuteText (frame->lastMinute + frame->timestepMinutes) +
                  (sliced ? " slice" : "") + (window ? " (window)" : "");
    char coverage[200];
    std::snprintf (coverage, sizeof (coverage), "%04d-%02d-%02d, %zu cell%s, r %.1f m; %s", frame->year, frame->month,
                   frame->day, frame->cells, frame->cells == 1 ? "" : "s", double (state.sunPointRadius),
                   covered.c_str ());
    char detail[420];
    std::snprintf (detail, sizeof (detail),
                   "Cell-steps: %zu blocked, %zu lit, %zu sun behind the surface, %zu unavailable. Footprints: each "
                   "cell split %d x %d, every sub-cell cast along the sun onto the face it hits.%s%s",
                   frame->blocked, frame->lit, frame->backFacing, frame->unavailable, frame->footprintDivisions,
                   frame->footprintDivisions,
                   window ? " The run measured only its hour window; hours outside it were not checked." : "",
                   frame->cells >= 256 ? " The point's cells were capped at 256, nearest first." : "");
    SunStudyNote (coverage, detail);
    if (frame->blockers.empty ()) {
        SunStudyNote (frame->mode == SunObstructionMode::WholeDay ? "Nothing blocks the point on these steps."
                                                                  : "Nothing blocks the point at this step.");
        return;
    }
    // One row per blocking element: its intervals, elapsed time, cell share.
    constexpr size_t kRows = 8;
    if (ImGui::BeginTable ("##sunblockers", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn ("Blocker", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn ("When", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn ("h, cells", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow ();
        for (size_t row = 0; row < frame->blockers.size () && row < kRows; ++row) {
            const SunObstructionBlocker& blocker = frame->blockers[row];
            ImGui::TableNextRow ();
            ImGui::TableNextColumn ();
            const std::string shortGuid = blocker.guid.size () > 9 ? blocker.guid.substr (0, 9) : blocker.guid;
            ImGui::TextUnformatted (shortGuid.c_str ());
            ImGui::SetItemTooltip ("%s\n%s element, type %d", blocker.guid.c_str (),
                                   blocker.context ? "Context" : "Analysis", int (blocker.elemType));
            ImGui::TableNextColumn ();
            // The first two intervals, each with a swatch in its time colour.
            std::string when;
            for (size_t i = 0; i < blocker.intervals.size (); ++i) {
                const std::string text =
                    MinuteText (blocker.intervals[i].first) + "-" + MinuteText (blocker.intervals[i].second);
                when += (when.empty () ? "" : "\n") + text;
                if (i >= 2)
                    continue;
                if (i > 0)
                    ImGui::SameLine (0.0f, 4.0f);
                const float side = ImGui::GetTextLineHeight () * 0.6f;
                const ImVec2 at = ImGui::GetCursorScreenPos ();
                const float y = at.y + (ImGui::GetTextLineHeight () - side) * 0.5f;
                const double middle = 0.5 * double (blocker.intervals[i].first + blocker.intervals[i].second);
                ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (at.x, y), ImVec2 (at.x + side, y + side),
                                                            Opaque (SunStudyTimeOfDayRgba (middle)), 2.0f);
                ImGui::Dummy (ImVec2 (side, ImGui::GetTextLineHeight ()));
                ImGui::SameLine (0.0f, 2.0f);
                ImGui::TextUnformatted (text.c_str ());
            }
            if (blocker.intervals.size () > 2) {
                ImGui::SameLine (0.0f, 4.0f);
                ImGui::TextDisabled ("+%zu", blocker.intervals.size () - 2);
            }
            ImGui::SetItemTooltip ("%s", when.c_str ());
            ImGui::TableNextColumn ();
            ImGui::Text ("%s %d%%", SunStudyClock (blocker.hours, true).c_str (),
                         int (100.0 * blocker.meanCellShare + 0.5));
        }
        ImGui::EndTable ();
    }
    if (frame->blockers.size () > kRows)
        ImGui::TextDisabled ("+%zu more blocking elements", frame->blockers.size () - kRows);
    // The time legend: the fills and rays use the diagram's time row.
    const float width = ImGui::GetContentRegionAvail ().x;
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const float barHeight = ImGui::GetTextLineHeight () * 0.5f;
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    constexpr int kSlices = 48;
    for (int i = 0; i < kSlices; ++i) {
        const float x0 = at.x + width * float (i) / kSlices, x1 = at.x + width * float (i + 1) / kSlices;
        const double minute = 240.0 + (1200.0 - 240.0) * (double (i) + 0.5) / kSlices;
        draw->AddRectFilled (ImVec2 (x0, at.y), ImVec2 (x1 + 0.5f, at.y + barHeight),
                             Opaque (SunStudyTimeOfDayRgba (minute)));
    }
    ImGui::Dummy (ImVec2 (width, barHeight));
    SunStudyNote ("04:00 morning - day - evening 20:00: fill and ray colour = time",
                  "Each blocked cell's shadow footprint is filled on its blocker in the colour of its step's time of "
                  "day, with the ray from the cell to where it is blocked. The blocking elements are outlined like a "
                  "selection, in pink. Elapsed hours count a step once however many cells it blocks; the share is "
                  "the mean share of the point's cells it blocks on those steps.");
}

} // namespace geomsrv::archviz
