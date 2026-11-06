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

void DrawSunStudyPointDay (HudState& state, const std::string& studyId)
{
    if (!ImGui::CollapsingHeader ("Study point", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const bool placed = DrawSunStudyPointHeader (state, studyId);
    evp::sunstudy::PointDay day;
    std::string error;
    const bool read =
        placed && evp::sunstudy::SunStudyStore::Get ().PointDayOf (studyId, state.sunPointSamples, day, error);
    // ⚠️ RESERVED: the strip and its line are drawn at their size with or
    // without data, grey when there is none.
    const float width = ImGui::GetContentRegionAvail ().x;
    const float height = ImGui::GetTextLineHeight () * 2.2f;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##sunpointday", ImVec2 (width, height + ImGui::GetTextLineHeight ()));
    const bool hovered = ImGui::IsItemHovered ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const ImU32 empty = ImGui::GetColorU32 (ImGuiCol_FrameBg);
    draw->AddRectFilled (start, ImVec2 (start.x + width, start.y + height), empty);
    const size_t steps = read ? day.minutes.size () : 0;
    if (steps == 0) {
        const char* why = !placed ? "" : !read ? "computing..." : "no sun in this day's slice";
        const ImVec2 size = ImGui::CalcTextSize (why);
        draw->AddText (ImVec2 (start.x + (width - size.x) * 0.5f, start.y + (height - size.y) * 0.5f),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), why);
        SunStudyNote (" ");
        return;
    }
    // Each step a column: lit (the shadow view's sunlit colour) up to its
    // share of the cells, shaded below it, grey where the sun is behind them.
    const auto colour = [] (uint32_t rgba) {
        return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
    };
    const ImU32 lit = colour (state.sunSingleLitRgba), shaded = colour (state.sunSingleShadowedRgba);
    const ImU32 behind = IM_COL32 (90, 94, 102, 255);
    const float column = width / float (steps);
    double litHours = 0.0, facingHours = 0.0;
    for (size_t s = 0; s < steps; ++s) {
        const float x0 = start.x + column * float (s), x1 = x0 + (std::max) (1.0f, column - 1.0f);
        const float facingTop = start.y + height * float (1.0 - day.facing[s]);
        const float litTop = start.y + height * float (1.0 - day.lit[s]);
        draw->AddRectFilled (ImVec2 (x0, start.y), ImVec2 (x1, facingTop), behind);
        draw->AddRectFilled (ImVec2 (x0, facingTop), ImVec2 (x1, litTop), shaded);
        draw->AddRectFilled (ImVec2 (x0, litTop), ImVec2 (x1, start.y + height), lit);
        litHours += day.lit[s] * day.hoursPerStep;
        facingHours += day.facing[s] * day.hoursPerStep;
    }
    // Hour ticks under the strip.
    int lastHour = -1;
    for (size_t s = 0; s < steps; ++s) {
        const int hour = day.minutes[s] / 60;
        if (hour == lastHour || day.minutes[s] % 60 != 0 || hour % 3 != 0)
            continue;
        lastHour = hour;
        char text[8];
        std::snprintf (text, sizeof (text), "%d", hour);
        draw->AddText (ImVec2 (start.x + column * float (s), start.y + height),
                       ImGui::GetColorU32 (ImGuiCol_TextDisabled), text);
    }
    if (hovered) {
        const size_t s = (std::min) (steps - 1, size_t ((std::max) (0.0f, ImGui::GetIO ().MousePos.x - start.x) /
                                                        (std::max) (column, 1.0f)));
        const int minutes = day.minutes[s];
        ImGui::SetTooltip ("%02d:%02d  lit %d%%, shaded %d%%, sun behind %d%% of the %zu cell(s)\n\n"
                           "Light: a clear line to the sun. Shaded: the sun is in front of the surface but the model "
                           "blocks it. Behind: the sun is on the other side of the surface.",
                           minutes / 60, minutes % 60, int (100.0 * day.lit[s] + 0.5),
                           int (100.0 * (day.facing[s] - day.lit[s]) + 0.5), int (100.0 * (1.0 - day.facing[s]) + 0.5),
                           day.samples);
    }
    char line[128];
    std::snprintf (line, sizeof (line), "%s direct of %s in front; sun up %s", SunStudyClock (litHours, true).c_str (),
                   SunStudyClock (facingHours, true).c_str (),
                   SunStudyClock (double (steps) * day.hoursPerStep, true).c_str ());
    SunStudyNote (line, "Direct: hours with a clear line to the sun, the mean of the point's cells. In front: hours "
                        "the sun is on the surface's side -- the most it could get. Sun up: hours the sun is above "
                        "the horizon in the study's time slice, the same everywhere.");
}

} // namespace geomsrv::archviz
