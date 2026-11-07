// The sun study's STUDY POINT (#152): placed from the view's right-click menu
// on a measured surface, snapped to the nearest sample, drawn as a sphere whose
// radius takes in the cells around it. The panel's diagrams show the mean of
// those cells: for a period through the period view (DiligentHudSunPeriod),
// for a single day here -- which steps of the day it was lit.
//
// ⚠️ A PLACED POINT, NOT THE HOVERED ONE: diagrams that followed the cursor
// changed under it on the way to the panel, and the re-traced profile was
// thrown away and restarted on every move.
//
// RENDER THREAD. Store reads are the inspector's kind: short, under the
// store's lock, refused while a study advances.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
        SunStudyNote ("No study point: right-click a measured surface, Place study point",
                      "The diagrams describe one placed point -- or the mean of the cells within its radius -- so "
                      "they stay put while the cursor moves. Right-click a measured surface in the view and choose "
                      "Place study point; the radius is in the same menu and here.");
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

} // namespace

void DrawSunStudyDayClock (HudState& state, const evp::sunstudy::PointDay* day, const char* id, int highlightedStep,
                           const char* emptyNote)
{
    // One measured step is one radial bar. Within it, radius shows the share
    // of the point's cells: lit inside, obstruction next, back-facing outside.
    // Angle remains clock time, never a re-spacing of only the daylight steps.
    const float width = ImGui::GetContentRegionAvail ().x;
    const float size = (std::min) (220.0f, width);
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
        const ImVec2 delta (ImGui::GetIO ().MousePos.x - centre.x, ImGui::GetIO ().MousePos.y - centre.y);
        const float radius = std::hypot (delta.x, delta.y);
        if (radius >= inner - 2.0f && radius <= outer + 3.0f) {
            double angle = std::atan2 (double (delta.y), double (delta.x)) + 1.57079632679489661923;
            if (angle < 0.0)
                angle += 6.28318530717958647692;
            const double clock = angle * (1440.0 / 6.28318530717958647692);
            size_t nearest = 0;
            double distance = 1440.0;
            for (size_t s = 0; s < steps; ++s) {
                const double middle = double (day->minutes[s]) + day->hoursPerStep * 30.0;
                double d = std::abs (clock - middle);
                d = (std::min) (d, 1440.0 - d);
                if (d < distance) {
                    distance = d;
                    nearest = s;
                }
            }
            const int minutes = day->minutes[nearest];
            ImGui::SetTooltip ("%02d:%02d  lit %d%%, shaded %d%%, sun behind %d%% of the %zu cell(s)\n\n"
                               "Light: a clear line to the sun. Shaded: the sun is in front of the surface but the "
                               "model blocks it. Behind: the sun is on the other side of the surface.",
                               minutes / 60, minutes % 60, int (100.0 * day->lit[nearest] + 0.5),
                               int (100.0 * (day->facing[nearest] - day->lit[nearest]) + 0.5),
                               int (100.0 * (1.0 - day->facing[nearest]) + 0.5), day->samples);
        }
    }
    char line[128];
    std::snprintf (line, sizeof (line), "%s direct of %s in front; sun up %s", SunStudyClock (litHours, true).c_str (),
                   SunStudyClock (facingHours, true).c_str (),
                   SunStudyClock (double (steps) * day->hoursPerStep, true).c_str ());
    SunStudyNote (line, "24-hour clock: 0/24 is at the top and time runs clockwise. Each measured step is a radial "
                        "bar: lit inside, shaded in the middle, sun behind outside. The centre is total direct sun. "
                        "Direct and in front are means of the point's cells; sun up is the site's total in the slice.");
}

void DrawSunStudyPointDay (HudState& state, const std::string& studyId)
{
    if (!ImGui::CollapsingHeader ("Study point", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const bool placed = DrawSunStudyPointHeader (state, studyId);
    evp::sunstudy::PointDay day;
    std::string error;
    const bool read =
        placed && evp::sunstudy::SunStudyStore::Get ().PointDayOf (studyId, state.sunPointSamples, day, error);
    const char* why = !placed ? "" : !read ? "computing..." : "no sun in this day's slice";
    const int current = state.sunStep >= 0 && size_t (state.sunStep) < day.minutes.size () ? state.sunStep : -1;
    DrawSunStudyDayClock (state, read ? &day : nullptr, "##sunpointday", current, why);
}

} // namespace geomsrv::archviz
