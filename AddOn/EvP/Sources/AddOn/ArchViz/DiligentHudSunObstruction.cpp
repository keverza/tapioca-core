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
#include <cfloat>
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

// The lit surface's key (#168): the hours-gained ramp as it is on the model,
// the toggle to the day diagram's clock step, and the hovered place's gain.
void DrawSolarCutGain (HudState& state, const SunObstructionFrame& frame)
{
    const int most = frame.exposure.MostLevel ();
    const double band = frame.exposure.bandMinutes;
    if (!state.sunCutStepOnly && most > 0 && band > 0.0) {
        const std::string from = SunStudyCutDuration (band), to = SunStudyCutDuration (band * most);
        const float width = ImGui::GetContentRegionAvail ().x;
        const float toWidth = ImGui::CalcTextSize (to.c_str ()).x + ImGui::GetStyle ().ItemSpacing.x;
        const float fromWidth = ImGui::CalcTextSize (from.c_str ()).x + ImGui::GetStyle ().ItemSpacing.x;
        const float bar = (std::max) (24.0f, width - toWidth - fromWidth);
        ImGui::TextUnformatted (from.c_str ());
        ImGui::SameLine ();
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        const float height = ImGui::GetTextLineHeight ();
        ImDrawList* draw = ImGui::GetWindowDrawList ();
        for (int k = 1; k <= most; ++k) {
            const float x0 = at.x + bar * float (k - 1) / float (most), x1 = at.x + bar * float (k) / float (most);
            draw->AddRectFilled (ImVec2 (x0, at.y + 0.2f * height), ImVec2 (x1 + 0.5f, at.y + 0.8f * height),
                                 Opaque (SolarCutGainRgba (k, most)));
        }
        ImGui::Dummy (ImVec2 (bar, height));
        geomsrv::archviz::hudshell::ItemTip (
            "Sun the cut gains at each place over the slice, swept between the measured steps: "
            "each shade is up to %s more, the palest the least, the deepest the most any place "
            "gains. Precision is a fraction of the study timestep, not continuous.",
            SunStudyCutDuration (band).c_str () + 1);
        ImGui::SameLine ();
        ImGui::TextUnformatted (to.c_str ());
    }
    ImGui::Checkbox ("Current step only##suncutstep", &state.sunCutStepOnly);
    geomsrv::archviz::hudshell::ItemTip (
        "Show only the surface the cut lights at the day diagram's clock step, and follow the "
        "clock as it is moved or played. Off: the whole slice, shaded by hours gained.");
    if (state.sunCutStepOnly) {
        const bool clocked = state.sunStep >= 0 && size_t (state.sunStep) < state.sunStepMinutes.size ();
        const int minute = clocked ? int (state.sunStepMinutes[size_t (state.sunStep)]) : -1;
        const bool wins =
            std::any_of (frame.exposure.steps.begin (), frame.exposure.steps.end (),
                         [minute] (const evp::sunstudy::SolarExposureStep& s) { return s.minute == minute; });
        if (!clocked)
            SunStudyNote ("Pick a step on the day diagram");
        else if (minute < frame.fromMinute || minute >= frame.toMinute)
            SunStudyNote ((MinuteText (minute) + ": outside the cut slice").c_str ());
        else
            SunStudyNote (
                (MinuteText (minute) + (wins ? ": lit by the cut" : ": the cut lights nothing new")).c_str ());
    }
    const std::string gain = SunStudyCutGain (state.sunCutReadingRuns);
    if (gain.empty ())
        ImGui::TextDisabled ("Hover the lit surface for what it gains");
    else
        ImGui::Text ("%s here, %s", gain.c_str (), SunStudyCutGainRuns (state.sunCutReadingRuns).c_str ());
    if (!gain.empty () && state.sunCutShowsHours && state.sunReadingState == 1 && state.sunCutCellAfter < 0.0f)
        ImGui::TextDisabled ("%s", SunStudyCutBeforeAfter (state.sunReadingHours, state.sunCutReadingRuns).c_str ());
}

// While a stage runs: which, how far, for how long, and whether it still
// advances. A cut that stops advancing says so rather than looking done (#168).
void DrawSolarCutProgress (const SunObstructionFrame& frame)
{
    if (frame.progress == nullptr)
        return;
    const SunSolarCutProgress& progress = *frame.progress;
    const auto phase = SunSolarCutPhase (progress.phase.load ());
    if (phase == SunSolarCutPhase::Done)
        return;
    const int64_t now = SunSolarCutProgress::NowMs ();
    const double elapsed = double (now - progress.startedMs.load ()) / 1000.0;
    const double idle = double (now - progress.advancedMs.load ()) / 1000.0;
    const uint64_t done = progress.done.load (), total = progress.total.load ();
    char text[160];
    float fraction = 0.0f;
    if (total > 0) {
        fraction = float (std::clamp (double (done) / double (total), 0.0, 1.0));
        std::snprintf (text, sizeof (text), "%s %.0f%%, %.0f s", SunSolarCutPhaseName (phase), 100.0 * fraction,
                       elapsed);
    }
    else
        std::snprintf (text, sizeof (text), "%s..., %.0f s", SunSolarCutPhaseName (phase), elapsed);
    ImGui::ProgressBar (total > 0 ? fraction : -1.0f * float (ImGui::GetTime ()), ImVec2 (-FLT_MIN, 0.0f), text);
    geomsrv::archviz::hudshell::ItemTip ("Stage %d of 5: %s. %llu of %llu done; %llu work units so far.",
                                         int (phase) + 1, SunSolarCutPhaseName (phase), (unsigned long long) done,
                                         (unsigned long long) total, (unsigned long long) progress.ticks.load ());
    constexpr double kStalled = 5.0;
    if (idle > kStalled) {
        char stalled[200];
        std::snprintf (stalled, sizeof (stalled), "No progress for %.0f s in \"%s\"", idle,
                       SunSolarCutPhaseName (phase));
        ImGui::TextColored (ImVec4 (1.0f, 0.62f, 0.25f, 1.0f), "%s", stalled);
        geomsrv::archviz::hudshell::ItemTip (
            "The worker has not reported for a while. Moving the point or the slice restarts "
            "the cut; if it stalls again at the same place, report the point and slice.");
    }
}

// What the finished cut cost, stage by stage, for a report.
std::string SolarCutTimings (const SunObstructionFrame& frame)
{
    if (frame.progress == nullptr)
        return {};
    std::string text = "Stage times:";
    char part[96];
    for (int i = 0; i < int (SunSolarCutPhase::Done); ++i) {
        std::snprintf (part, sizeof (part), "%s %s %.2f s", i == 0 ? "" : ",",
                       SunSolarCutPhaseName (SunSolarCutPhase (i)), frame.progress->phaseSeconds[i].load ());
        text += part;
    }
    std::snprintf (part, sizeof (part), ". Lit-surface work: %.1fM units.", double (frame.exposure.work) / 1.0e6);
    return text + part;
}

// The solar cut in numbers: the hours it buys at the point (re-traced after
// the cut), the volume that goes, each element's share, and the cells whose
// shadow it removes.
void DrawSolarCutSummary (HudState& state, const SunObstructionFrame& frame)
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
        DrawSolarCutProgress (frame);
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
        "existing elements keep their outline and edges. Yellow area: projected removed shadows minus all "
        "remaining shadows, clipped to surviving measured surfaces and swept between consecutive measured suns, "
        "shaded by the minutes it gains (pale: the least, deep amber: the most). "
        "It is independent of cell spacing; real openings and remaining shadows stay unlit. "
        "Time precision is still the study timestep, not continuous sun motion.");
    if (!retraced)
        DrawSolarCutProgress (frame);
    // Every way the cut can end is SAID: an empty result is not a stall (#168).
    const ImVec4 warning (1.0f, 0.62f, 0.25f, 1.0f);
    const std::string timings = retraced ? SolarCutTimings (frame) : std::string ();
    if (retraced && !frame.cutCells.error.empty ()) {
        ImGui::TextColored (warning, "Cells not re-counted");
        geomsrv::archviz::hudshell::ItemTip ("%s", frame.cutCells.error.c_str ());
    }
    else if (retraced && frame.cutCells.cells > 0) {
        std::snprintf (line, sizeof (line), "%zu cell%s gain sun over the day, %.1f m2, up to %s a day",
                       frame.cutCells.cells, frame.cutCells.cells == 1 ? "" : "s", frame.cutCells.area,
                       SunStudyCutDuration (double (frame.cutCells.mostGained) * 60.0).c_str ());
        const std::string detail =
            std::string ("Each study cell the cut changes, its day re-counted from the study's own result: steps "
                         "already lit stay lit; a step whose ray ran through the removed part and now runs clear "
                         "of what remains is won. ") +
            (frame.cutCells.unplaced > 0 ? std::to_string (frame.cutCells.unplaced) +
                                               " changed cells have no lattice to draw on and are counted only. "
                                         : std::string ()) +
            timings;
        SunStudyNote (line, detail.c_str ());
    }
    // The reason is in the panel, not only behind a hover: a report needs it.
    if (!frame.exposure.error.empty ()) {
        ImGui::PushStyleColor (ImGuiCol_Text, warning);
        ImGui::TextWrapped ("Won-lit surface failed: %s", frame.exposure.error.c_str ());
        ImGui::PopStyleColor ();
        geomsrv::archviz::hudshell::ItemTip ("%s", timings.c_str ());
    }
    else if (retraced && !frame.exposure.note.empty ()) {
        ImGui::PushStyleColor (ImGuiCol_Text, warning);
        ImGui::TextWrapped ("Won-lit surface incomplete: %s", frame.exposure.note.c_str ());
        ImGui::PopStyleColor ();
        geomsrv::archviz::hudshell::ItemTip ("%s", timings.c_str ());
    }
    const bool shown = retraced && frame.exposure.error.empty ();
    if (shown && !frame.exposure.steps.empty ())
        DrawSolarCutGain (state, frame);
    else if (shown && frame.exposure.note.empty ())
        SunStudyNote (frame.wonCells > 0 ? "No won-lit surface on the measured surfaces in this slice"
                                         : "The cut brings no new sun to the measured surfaces in this slice",
                      (std::string ("The removed part's shadow in the slice lies on nothing the study measured, or "
                                    "what remains still shades it. ") +
                       timings)
                          .c_str ());
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
            geomsrv::archviz::hudshell::ItemTip ("%s\ntype %d", object.guid.c_str (), int (object.elemType));
            ImGui::TableNextColumn ();
            if (object.closed)
                ImGui::Text ("%.1f m3", object.removed);
            else
                ImGui::TextDisabled ("open mesh");
            geomsrv::archviz::hudshell::ItemTip (
                "%s", object.closed ? "Volume above the cutting surface, from its closed boundary."
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
    // Segmented like the view row above: the active mode is the lit button.
    const char* labels[] = { "Time step##sunobstruction", "Whole day##sunobstruction", "Solar cut##sunobstruction" };
    const char* tips[] = { "What blocks the study point's sun at the diagram's selected step.",
                           "Every measured step in the selected slice (all of it when none is selected).",
                           "What of the blockers must go for direct sun through the pink slice on the day diagram." };
    const float modeWidth = (ImGui::GetContentRegionAvail ().x - 2.0f * ImGui::GetStyle ().ItemSpacing.x) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        if (i != 0)
            ImGui::SameLine ();
        const bool active = state.sunObstructionMode == i + 1;
        ImGui::PushStyleColor (ImGuiCol_Button,
                               ImGui::GetStyleColorVec4 (active ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        if (ImGui::Button (labels[i], ImVec2 (modeWidth, 0)))
            state.sunObstructionMode = i + 1;
        ImGui::PopStyleColor ();
        geomsrv::archviz::hudshell::ItemTip ("%s", tips[i]);
    }
    // What lies under the cut (#168): the day's sun hours, to read the gain
    // against the whole study, or the single shadow at the diagram's clock.
    if (state.sunObstructionMode == 3) {
        const char* bases[] = { "Sun hours##suncutbase", "Shadow at clock##suncutbase" };
        const char* baseTips[] = { "The study's hours of direct sun under the cut: hover a lit place for the day "
                                   "before and after.",
                                   "The single shadow at the day diagram's clock step under the cut." };
        for (int i = 0; i < 2; ++i) {
            if (i != 0)
                ImGui::SameLine ();
            const bool active = state.sunCutShowsHours == (i == 0);
            ImGui::PushStyleColor (ImGuiCol_Button,
                                   ImGui::GetStyleColorVec4 (active ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
            if (ImGui::SmallButton (bases[i]))
                state.sunCutShowsHours = i == 0;
            ImGui::PopStyleColor ();
            geomsrv::archviz::hudshell::ItemTip ("%s", baseTips[i]);
        }
        if (state.sunCutShowsHours) {
            ImGui::Checkbox ("Cells##suncutcells", &state.sunCutShowsCells);
            geomsrv::archviz::hudshell::ItemTip (
                "The study cells the cut changes, re-counted over the whole day (the study's own "
                "lit steps plus the rays that now run clear past what remains) and drawn in the "
                "hours they would read, in the map's colours.");
            ImGui::SameLine ();
            ImGui::Checkbox ("Outline##suncutoutline", &state.sunCutShowsOutline);
            geomsrv::archviz::hudshell::ItemTip (
                "One pink outline around the changed cells on each surface: where the cut's new "
                "sun comes from.");
            ImGui::SameLine ();
            ImGui::Checkbox ("Lit surface##suncutgain", &state.sunCutShowsGain);
            geomsrv::archviz::hudshell::ItemTip (
                "The exact surface the cut lights in its slice, independent of the cells, shaded "
                "yellow to amber by the minutes it gains, with its own yellow edge.");
        }
    }
    // The prerequisite, made actionable (#167, #168): Place point arms a click
    // on the scene, so nobody has to know about the right-click menu. A cell
    // hovered just before entering the panel can still be pinned directly.
    const auto placeButton = [&state, &studyId] (const char* label) {
        if (ImGui::SmallButton (state.sunPointPlacing ? "Click the scene...##sunobstructionplace" : label)) {
            state.sunPointPlacing = !state.sunPointPlacing;
            state.sunPointStudy = studyId;
            state.sunPointDragging = false;
        }
        geomsrv::archviz::hudshell::ItemTip (
            "Then click a measured surface in the view; keep the button down to drag the point "
            "into place. Right-click cancels. The view's right-click menu places it too.");
    };
    if (!state.sunPointPlaced) {
        placeButton ("Place point##sunobstructionplace");
        const bool hovered =
            state.sunHoverDayStudy == studyId && state.sunHoverDaySample != SIZE_MAX && state.sunHoverDayPointValid;
        if (hovered && !state.sunPointPlacing) {
            ImGui::SameLine ();
            if (ImGui::SmallButton ("Pin hovered cell##sunobstruction"))
                PlaceSunStudyPoint (state, state.sunHoverDayPoint);
        }
        SunStudyNote (state.sunPointPlacing ? "Click a measured surface in the view"
                                            : "Place a study point to find what blocks it",
                      "Obstruction is attributed for a placed study point: its snapped cell, or every cell in its "
                      "radius. Place it with the button, pin the cell hovered last, or right-click a measured "
                      "surface and place the point there.");
        return;
    }
    placeButton ("Move point##sunobstructionplace");
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
            geomsrv::archviz::hudshell::ItemTip ("%s\n%s element, type %d", blocker.guid.c_str (),
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
            geomsrv::archviz::hudshell::ItemTip ("%s", when.c_str ());
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
