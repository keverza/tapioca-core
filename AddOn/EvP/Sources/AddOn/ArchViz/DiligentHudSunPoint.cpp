// The sun study's STUDY POINT (#152): placed from the view's right-click menu
// on a measured surface, snapped to the nearest sample, drawn as a sphere whose
// radius takes in the cells around it. The panel's diagrams show the mean of
// those cells: for a period through the period view (DiligentHudSunPeriod),
// for a single day here -- which steps of the day it was lit.
//
// A placed point pins the diagram. Without one, the exact measured hover sample
// is previewed and latched while the pointer crosses into the panel; no new
// trace is launched. Both paths reuse the cached stored day.
//
// RENDER THREAD. Store reads are the inspector's kind: short, under the
// store's lock, refused while a study advances.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunStudyObstructionController.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace geomsrv::archviz {

namespace {

constexpr char kMenu[] = "##sunpoint.menu";
constexpr size_t kPointSampleCap = 256; // cells averaged; nearest first
constexpr float kRadii[] = { 0.0f, 0.5f, 1.0f, 2.0f, 5.0f };
constexpr ImU32 kPointColour = IM_COL32 (64, 200, 255, 230);

bool Project (const double point[3], const float viewProj[16], uint32_t width, uint32_t height, ImVec2& screen)
{
    const float input[4] = { float (point[0]), float (point[1]), float (point[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, input, viewProj);
    if (!std::isfinite (clip[0]) || !std::isfinite (clip[1]) || !std::isfinite (clip[2]) || !std::isfinite (clip[3]) ||
        clip[3] <= 1.0e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    screen.x = (clip[0] / clip[3] * 0.5f + 0.5f) * float (width);
    screen.y = (0.5f - clip[1] / clip[3] * 0.5f) * float (height);
    return std::isfinite (screen.x) && std::isfinite (screen.y);
}

void Place (HudState& state, const double at[3])
{
    state.sunPointPlaced = true;
    std::copy (at, at + 3, state.sunPointAt);
    std::copy (at, at + 3, state.sunPointCentre);
    state.sunPointSamples.clear ();
    state.sunPointSamplesFor.clear ();
}

std::string RadiusText (float radius)
{
    if (radius <= 0.0f)
        return "one cell";
    char text[24];
    std::snprintf (text, sizeof (text), "%.1f m radius", double (radius));
    return text;
}

} // namespace

void DrawSunStudyPointMenu (HudState& state)
{
    // On the view's background only: a panel, a gumball or a popup that took
    // the press keeps it (hudshell::ClaimRightClick).
    const bool background = !ImGui::IsWindowHovered (ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemHovered ();
    const bool measured = state.sunReadingState == 1 && state.sunReadingPointValid;
    if (background && ImGui::IsMouseReleased (ImGuiMouseButton_Right) && !hudshell::RightClickClaimed () &&
        (measured || state.sunPointPlaced) && !state.readOnly) {
        hudshell::ClaimRightClick ();
        state.sunPointMenuValid = measured;
        if (measured)
            std::copy (state.sunReadingPoint, state.sunReadingPoint + 3, state.sunPointMenuAt);
        ImGui::OpenPopup (kMenu);
    }
    if (!ImGui::BeginPopup (kMenu))
        return;
    ImGui::TextDisabled ("Sun study point");
    if (ImGui::MenuItem (state.sunPointPlaced ? "Move study point here" : "Place study point here", nullptr, false,
                         state.sunPointMenuValid))
        Place (state, state.sunPointMenuAt);
    if (!state.sunPointMenuValid)
        ImGui::SetItemTooltip ("Right-click a measured surface (with Inspect on) to place the point there.");
    if (ImGui::BeginMenu ("Radius")) {
        for (const float radius : kRadii)
            if (ImGui::MenuItem (RadiusText (radius).c_str (), nullptr, state.sunPointRadius == radius))
                state.sunPointRadius = radius;
        ImGui::EndMenu ();
    }
    if (ImGui::MenuItem ("Remove study point", nullptr, false, state.sunPointPlaced)) {
        state.sunPointPlaced = false;
        state.sunPointSamples.clear ();
        state.sunPointSamplesFor.clear ();
    }
    ImGui::EndPopup ();
}

void DrawSunStudyPointOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    if (!state.sunPointPlaced || viewProj == nullptr || width == 0 || height == 0)
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList ();
    const double* c = state.sunPointCentre;
    ImVec2 centre;
    if (!Project (c, viewProj, width, height, centre))
        return;
    // Three great circles: a sphere that reads as one from any side. A zero
    // radius still gets a small one, so the point can be found.
    const double r = (std::max) (double (state.sunPointRadius), 0.15);
    constexpr int kSegments = 48;
    for (int plane = 0; plane < 3; ++plane) {
        ImVec2 previous;
        bool had = false;
        for (int i = 0; i <= kSegments; ++i) {
            const double a = 2.0 * 3.14159265358979323846 * double (i) / double (kSegments);
            double p[3] = { c[0], c[1], c[2] };
            const int u = plane == 2 ? 1 : 0, v = plane == 0 ? 1 : 2;
            p[u] += r * std::cos (a);
            p[v] += r * std::sin (a);
            ImVec2 at;
            const bool ok = Project (p, viewProj, width, height, at);
            if (ok && had)
                draw->AddLine (previous, at, kPointColour, 1.5f);
            previous = at;
            had = ok;
        }
    }
    draw->AddCircleFilled (centre, 4.0f, kPointColour);
    draw->AddCircle (centre, 4.0f, IM_COL32 (20, 30, 40, 220), 0, 1.5f);
}

bool RefreshSunStudyPointSamples (HudState& state, const std::string& studyId)
{
    if (!state.sunPointPlaced || studyId.empty ())
        return false;
    char key[160];
    std::snprintf (key, sizeof (key), "%s|%.3f", studyId.c_str (), double (state.sunPointRadius));
    if (state.sunPointSamplesFor != key) {
        std::string error;
        double snapped[3] = {};
        state.sunPointSamples.clear ();
        if (evp::sunstudy::SunStudyStore::Get ().PointSamples (studyId, state.sunPointAt, double (state.sunPointRadius),
                                                               kPointSampleCap, state.sunPointSamples, snapped, error))
            std::copy (snapped, snapped + 3, state.sunPointCentre);
        state.sunPointSamplesFor = key;
    }
    return !state.sunPointSamples.empty ();
}

bool DrawSunStudyPointHeader (HudState& state, const std::string& studyId)
{
    // One fixed row either way, so the diagrams below never move.
    if (!RefreshSunStudyPointSamples (state, studyId)) {
        const bool hover =
            !state.sunPointPlaced && state.sunHoverDayStudy == studyId && state.sunHoverDaySample != SIZE_MAX;
        SunStudyNote (hover ? "Hover: one cell; right-click to place and pin a study point"
                            : "Hover a measured surface, or right-click to place a study point",
                      "Hover previews that exact measured cell and remains available when the pointer enters this "
                      "panel. A placed point pins the diagram and can average the cells within its radius.");
        return false;
    }
    const float spacing = ImGui::GetStyle ().ItemSpacing.x;
    const float remove = ImGui::CalcTextSize ("Remove").x + ImGui::GetStyle ().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth ((std::max) (60.0f, ImGui::GetContentRegionAvail ().x - remove - spacing));
    char format[48];
    std::snprintf (format, sizeof (format), "r %%.1f m, %zu cell%s", state.sunPointSamples.size (),
                   state.sunPointSamples.size () == 1 ? "" : "s");
    ImGui::SliderFloat ("##sunpointradius", &state.sunPointRadius, 0.0f, 10.0f, format);
    ImGui::SetItemTooltip ("The study point's sphere: every measured cell within this distance of the snapped "
                           "point is averaged (at most %zu, nearest first). 0 is the one cell.",
                           kPointSampleCap);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove##sunpoint")) {
        state.sunPointPlaced = false;
        state.sunPointSamples.clear ();
        state.sunPointSamplesFor.clear ();
        return false;
    }
    return true;
}

namespace {

void ClockSector (ImDrawList* draw, ImVec2 centre, float inner, float outer, double from, double to, ImU32 colour)
{
    if (outer <= inner || to <= from)
        return;
    draw->PathClear ();
    draw->PathArcTo (centre, outer, float (from), float (to));
    draw->PathArcTo (centre, inner, float (to), float (from));
    draw->PathFillConvex (colour);
}

ImVec2 ClockPoint (ImVec2 centre, float radius, double angle)
{
    return ImVec2 (centre.x + radius * float (std::cos (angle)), centre.y + radius * float (std::sin (angle)));
}

void ClockLabel (ImDrawList* draw, ImVec2 centre, float radius, double angle, const char* text)
{
    const ImVec2 at = ClockPoint (centre, radius, angle);
    const ImVec2 size = ImGui::CalcTextSize (text);
    draw->AddText (ImVec2 (at.x - size.x * 0.5f, at.y - size.y * 0.5f), ImGui::GetColorU32 (ImGuiCol_TextDisabled),
                   text);
}

size_t NearestClockStep (const evp::sunstudy::PointDay& day, size_t steps, double minute)
{
    // A bar's start belongs to that bar, not the preceding step's centre.
    for (size_t s = 0; s < steps; ++s)
        if (minute >= double (day.minutes[s]) && minute < double (day.minutes[s]) + day.hoursPerStep * 60.0)
            return s;
    size_t nearest = 0;
    double distance = 1440.0;
    for (size_t s = 0; s < steps; ++s) {
        const double middle = double (day.minutes[s]) + day.hoursPerStep * 30.0;
        double d = std::abs (minute - middle);
        d = (std::min) (d, 1440.0 - d);
        if (d < distance) {
            distance = d;
            nearest = s;
        }
    }
    return nearest;
}

} // namespace

void DrawSunStudyDayClock (HudState& state, const evp::sunstudy::PointDay* day, const char* id, int highlightedStep,
                           const char* emptyNote, SunStudyClockSelection* selection, float sizeCap)
{
    // One measured step is one radial bar. Within it, radius shows the share
    // of the point's cells: lit inside, obstruction next, back-facing outside.
    // Angle remains clock time, never a re-spacing of only the daylight steps.
    const float width = ImGui::GetContentRegionAvail ().x;
    const float size = (std::min) (sizeCap, width);
    if (size <= 0.0f)
        return;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton (id, ImVec2 (width, size));
    const bool hovered = ImGui::IsItemHovered ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const ImVec2 centre (start.x + width * 0.5f, start.y + size * 0.5f);
    const float inner = size * 0.20f, outer = size * 0.34f;
    draw->AddCircle (centre, (inner + outer) * 0.5f, IM_COL32 (100, 106, 116, 70), 96, outer - inner);
    for (int hour = 0; hour < 24; ++hour) {
        const double angle = SunStudyClockAngle (hour * 60.0);
        const float tick = hour % 3 == 0 ? size * 0.025f : size * 0.012f;
        draw->AddLine (ClockPoint (centre, outer + 1.0f, angle), ClockPoint (centre, outer + tick, angle),
                       IM_COL32 (130, 136, 146, 130), 1.0f);
    }
    const float labels = size * 0.44f;
    ClockLabel (draw, centre, labels, SunStudyClockAngle (0), "0 / 24");
    ClockLabel (draw, centre, labels, SunStudyClockAngle (6 * 60), "6");
    ClockLabel (draw, centre, labels, SunStudyClockAngle (12 * 60), "12");
    ClockLabel (draw, centre, labels, SunStudyClockAngle (18 * 60), "18");

    const size_t steps =
        day != nullptr ? (std::min) ({ day->minutes.size (), day->lit.size (), day->facing.size () }) : 0;
    if (selection != nullptr)
        *selection = SunStudyClockSelection ();
    const ImVec2 pointer (ImGui::GetIO ().MousePos.x - centre.x, ImGui::GetIO ().MousePos.y - centre.y);
    const float pointerRadius = std::hypot (pointer.x, pointer.y);
    const bool inRing = pointerRadius >= inner - 2.0f && pointerRadius <= outer + 3.0f;
    const uint32_t clockId = ImGui::GetID (id);
    const auto cancelBrush = [&state] {
        state.sunClockBrushActive = state.sunClockBrushDragged = false;
        state.sunClockBrushId = 0;
        state.sunClockBrushFrame = -1;
    };
    // A brush belongs to one continuously visible clock. A hidden/replaced
    // clock must never consume a later, unrelated mouse release.
    if (selection != nullptr && state.sunClockBrushActive &&
        (state.sunClockBrushId != clockId || steps == 0 || ImGui::GetFrameCount () != state.sunClockBrushFrame + 1))
        cancelBrush ();
    if (selection != nullptr && state.sunClockBrushActive && !ImGui::IsMouseDown (ImGuiMouseButton_Left) &&
        !ImGui::IsMouseReleased (ImGuiMouseButton_Left))
        cancelBrush ();
    if (selection != nullptr && steps > 0) {
        if (hovered && inRing && ImGui::IsItemActivated ()) {
            state.sunClockBrushActive = true;
            state.sunClockBrushDragged = false;
            state.sunClockBrushId = clockId;
            state.sunClockBrushFrom = state.sunClockBrushTo = SunStudyClockMinute (pointer.x, pointer.y);
        }
        if (state.sunClockBrushActive) {
            state.sunClockBrushFrame = ImGui::GetFrameCount ();
            state.sunClockBrushTo = SunStudyClockMinute (pointer.x, pointer.y);
            state.sunClockBrushDragged =
                state.sunClockBrushDragged || ImGui::IsMouseDragging (ImGuiMouseButton_Left, 2.0f);
            if (ImGui::IsMouseReleased (ImGuiMouseButton_Left)) {
                if (state.sunClockBrushDragged) {
                    selection->slice = true;
                    selection->fromMinute = state.sunClockBrushFrom;
                    // The shared shadow range is non-wrapping: constrain the
                    // gesture at midnight instead of silently losing its tail.
                    selection->toMinute =
                        state.sunClockBrushTo > state.sunClockBrushFrom ? state.sunClockBrushTo : 1440.0;
                }
                else
                    selection->step = int (NearestClockStep (*day, steps, state.sunClockBrushFrom));
                cancelBrush ();
            }
        }
        if (selection->step >= 0)
            highlightedStep = selection->step;
    }
    // Hover is an immediate preview of the exact slice; release still commits
    // the click/drag so a brush cannot be mistaken for a single-step click.
    if (hovered && inRing && steps > 0)
        highlightedStep = int (NearestClockStep (*day, steps, SunStudyClockMinute (pointer.x, pointer.y)));
    const auto colour = [] (uint32_t rgba) {
        return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
    };
    const ImU32 lit = colour (state.sunSingleLitRgba), shaded = colour (state.sunSingleShadowedRgba);
    const ImU32 behind = IM_COL32 (90, 94, 102, 255);
    double litHours = 0.0, facingHours = 0.0;
    for (size_t s = 0; s < steps; ++s) {
        const double quantum = (std::max) (0.0, day->hoursPerStep * 60.0);
        const double begin = SunStudyClockAngle (day->minutes[s]);
        const double end = SunStudyClockAngle (double (day->minutes[s]) + quantum);
        const double gap = (std::min) (0.006, (end - begin) * 0.12);
        const double from = begin + gap * 0.5, to = end - gap * 0.5;
        const double litShare = std::clamp (day->lit[s], 0.0, 1.0);
        const double facingShare = std::clamp ((std::max) (litShare, day->facing[s]), 0.0, 1.0);
        const float litEdge = inner + (outer - inner) * float (litShare);
        const float facingEdge = inner + (outer - inner) * float (facingShare);
        ClockSector (draw, centre, facingEdge, outer, from, to, behind);
        ClockSector (draw, centre, litEdge, facingEdge, from, to, shaded);
        ClockSector (draw, centre, inner, litEdge, from, to, lit);
        litHours += day->lit[s] * day->hoursPerStep;
        facingHours += day->facing[s] * day->hoursPerStep;
        if (int (s) == highlightedStep) {
            const ImU32 ink = ImGui::GetColorU32 (ImGuiCol_Text);
            for (const double angle : { begin, end })
                draw->AddLine (ClockPoint (centre, inner - 2.0f, angle), ClockPoint (centre, outer + 2.0f, angle), ink,
                               1.5f);
            draw->PathClear ();
            draw->PathArcTo (centre, outer + 2.0f, float (begin), float (end));
            draw->PathStroke (ink, 0, 1.5f);
        }
    }
    if (day != nullptr && day->hasTotalHours) {
        litHours = day->directHours;
        facingHours = day->facingHours;
    }
    if (selection != nullptr && state.sunClockBrushActive && state.sunClockBrushDragged) {
        const double to = state.sunClockBrushTo > state.sunClockBrushFrom ? state.sunClockBrushTo : 1440.0;
        for (double minute = state.sunClockBrushFrom; minute < to; minute += 30.0)
            ClockSector (draw, centre, inner - 2.0f, outer + 2.0f, SunStudyClockAngle (minute),
                         SunStudyClockAngle ((std::min) (minute + 30.0, to)), IM_COL32 (255, 255, 255, 54));
    }

    const auto centreText = [&] (const char* text, float y, ImU32 ink) {
        const ImVec2 extent = ImGui::CalcTextSize (text);
        draw->AddText (ImVec2 (centre.x - extent.x * 0.5f, centre.y + y - extent.y * 0.5f), ink, text);
    };
    if (steps == 0) {
        centreText (emptyNote != nullptr ? emptyNote : "", 0.0f, ImGui::GetColorU32 (ImGuiCol_TextDisabled));
        SunStudyNote (" ");
        return;
    }
    const std::string total = SunStudyClock (litHours, true);
    centreText (total.c_str (), -ImGui::GetTextLineHeight () * 0.45f, ImGui::GetColorU32 (ImGuiCol_Text));
    centreText ("direct", ImGui::GetTextLineHeight () * 0.65f, ImGui::GetColorU32 (ImGuiCol_TextDisabled));

    if (hovered) {
        if (inRing) {
            const double clock = SunStudyClockMinute (pointer.x, pointer.y);
            const size_t nearest = NearestClockStep (*day, steps, clock);
            const int minutes = day->minutes[nearest];
            ImGui::SetTooltip ("%02d:%02d  lit %d%%, shaded %d%%, sun behind %d%% of the %zu cell(s)\n\n%s\n\n"
                               "Light: a clear line to the sun. Shaded: the sun is in front of the surface but the "
                               "model blocks it. Behind: the sun is on the other side of the surface.",
                               minutes / 60, minutes % 60, int (100.0 * day->lit[nearest] + 0.5),
                               int (100.0 * (day->facing[nearest] - day->lit[nearest]) + 0.5),
                               int (100.0 * (1.0 - day->facing[nearest]) + 0.5), day->samples,
                               selection != nullptr ? "Click: select this step. Drag clockwise (up to 24:00): select a "
                                                      "time slice."
                                                    : "Display-only seasonal aggregate; no independent selection.");
        }
    }
    char line[192];
    std::string range;
    if (day->samples > 1 && day->hasCellRange)
        range =
            "; cells " + SunStudyClock (day->minDirectHours, true) + "-" + SunStudyClock (day->maxDirectHours, true);
    std::snprintf (
        line, sizeof (line), "%s direct of %s in front; sun up %s%s", SunStudyClock (litHours, true).c_str (),
        SunStudyClock (facingHours, true).c_str (),
        SunStudyClock (day->hasTotalHours ? day->sunUpHours : double (steps) * day->hoursPerStep, true).c_str (),
        range.c_str ());
    if (selection != nullptr && state.sunClockBrushActive && state.sunClockBrushDragged &&
        state.sunClockBrushTo <= state.sunClockBrushFrom)
        SunStudyNote ("Clock slice stops at 24:00", "The shadow-hours range cannot wrap through midnight. Drag "
                                                    "from midnight into the morning to select that part instead.");
    else
        SunStudyNote (line,
                      "24-hour clock: 0/24 is at the top and time runs clockwise. Each measured step is a radial "
                      "bar: lit inside, shaded in the middle, sun behind outside. The centre is total direct sun. "
                      "Direct and in front are means of the point's cells; sun up is the site's total in the slice. "
                      "For multiple cells, the summary's cell range is their exact minimum-maximum direct hours; "
                      "the centre remains the mean.");
}

void ApplySunStudyDaySelection (HudState& state, const SunStudyClockSelection& selection)
{
    if (selection.step >= 0) {
        state.sunPlaying = false;
        state.sunStep = selection.step;
        state.sunTimeFrom = 0.0f;
        state.sunTimeTo = 24.0f;
    }
    else if (selection.slice) {
        state.sunPlaying = false;
        state.sunTimeFrom = float (selection.fromMinute / 60.0);
        state.sunTimeTo = float (selection.toMinute / 60.0);
    }
}

void DrawSunStudyPointDay (HudState& state, const std::string& studyId, uint64_t resultVersion)
{
    if (state.sunReadingState == 1 && state.sunReadingSample != SIZE_MAX && state.sunReadingStudy == studyId) {
        state.sunHoverDayStudy = studyId;
        state.sunHoverDaySample = state.sunReadingSample;
    }
    if (!ImGui::CollapsingHeader ("Study point", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const bool pointReady = DrawSunStudyPointHeader (state, studyId);
    const bool pinned = state.sunPointPlaced;
    std::vector<size_t> hoverSample;
    const std::vector<size_t>* samples = nullptr;
    if (pinned) {
        if (pointReady)
            samples = &state.sunPointSamples;
    }
    else if (state.sunHoverDayStudy == studyId && state.sunHoverDaySample != SIZE_MAX) {
        hoverSample = { state.sunHoverDaySample };
        samples = &hoverSample;
    }

    bool read = false;
    if (samples != nullptr) {
        if (state.sunPointDayStudy != studyId || state.sunPointDayVersion != resultVersion ||
            state.sunPointDaySamples != *samples) {
            evp::sunstudy::PointDay fresh;
            std::string error;
            ++state.sunPointDayReads;
            state.sunPointDayReady = evp::sunstudy::SunStudyStore::Get ().PointDayOf (studyId, *samples, fresh, error);
            state.sunPointDay = std::move (fresh);
            if (state.sunPointDayReady) {
                state.sunPointDayStudy = studyId;
                state.sunPointDayVersion = resultVersion;
                state.sunPointDaySamples = *samples;
            }
            else
                state.sunPointDayStudy.clear (); // "computing" is transient: retry the same selection next frame
        }
        read = state.sunPointDayReady;
    }
    const evp::sunstudy::PointDay* day = read ? &state.sunPointDay : nullptr;
    const size_t steps = day != nullptr ? day->minutes.size () : 0;
    const char* why = samples == nullptr ? "hover a measured cell"
                      : !read            ? "computing..."
                                         : "no sun in this day's slice";
    const int current = state.sunStep >= 0 && size_t (state.sunStep) < steps ? state.sunStep : -1;
    SunStudyClockSelection selection;
    DrawSunStudyDayClock (state, day, "##sunpointday", current, why, &selection, 352.0f);
    ApplySunStudyDaySelection (state, selection); // selection never changes the current view tab
}

void DrawSunStudyObstructionControls (HudState& state, const std::string& studyId)
{
    ImGui::RadioButton ("Time step##sunobstruction", &state.sunObstructionMode, 1);
    ImGui::SameLine ();
    ImGui::RadioButton ("Whole day##sunobstruction", &state.sunObstructionMode, 2);
    ImGui::SetItemTooltip (
        "Time step: exact nearest blockers at the clock's selected step, filled pink. Whole day: "
        "every measured step, with exact parts coloured by shadow time. Both wire the whole object.");
    if (!state.sunPointPlaced) {
        SunStudyNote ("Place a study point to attribute its obstructions.");
        return;
    }
    auto& tasks = SunObstructionTasks ();
    const auto frame = tasks.Latest ();
    if (frame == nullptr || frame->studyId != studyId)
        SunStudyNote (tasks.Busy () ? "Finding exact obstructing parts..." : "Waiting for the current study...");
    else if (!frame->error.empty ())
        hudshell::ErrorNotice ("Obstruction not shown: " + frame->error);
    else if (!SunObstructionFrameCurrent (frame, studyId))
        SunStudyNote ("Obstruction hidden: the scene changed; run the day again.");
    else if (frame->timedParts == 0)
        SunStudyNote (frame->mode == SunObstructionMode::WholeDay ? "No obstruction found on any measured step."
                                                                  : "No obstruction at this time step.");
    else {
        char line[160];
        if (frame->mode == SunObstructionMode::WholeDay)
            std::snprintf (line, sizeof (line), "%zu timed part(s) on %zu blocking object(s); colour = shadow time",
                           frame->timedParts, frame->blockerObjects);
        else
            std::snprintf (line, sizeof (line), "%02d:%02d: %zu exact part(s) on %zu blocking object(s), pink",
                           frame->minute / 60, frame->minute % 60, frame->timedParts, frame->blockerObjects);
        SunStudyNote (line, "Whole-day gradient: midnight blue, dawn orange, noon yellow, dusk red, midnight blue. "
                            "A part that blocks at several steps has one timed fill per step, drawn chronologically "
                            "with the latest foremost. Wireframes use source/CAD boundaries where available.");
    }
}

} // namespace geomsrv::archviz
