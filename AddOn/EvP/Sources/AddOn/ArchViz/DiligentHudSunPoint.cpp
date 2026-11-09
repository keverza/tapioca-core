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
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunStudyObstructionController.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "SunStudy/SunStudyLiftedPoint.hpp"
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
constexpr float kGripRadius = 10.0f; // pixels around the 2D grip that take a press

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
    state.sunPointLiftedFor.clear ();
}

} // namespace

void PlaceSunStudyPoint (HudState& state, const double at[3])
{
    Place (state, at);
}

namespace {

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
    if (ImGui::BeginMenu ("Height")) {
        for (const float height : { 0.0f, 1.2f, 1.6f, 1.8f })
            if (ImGui::MenuItem (height <= 0.0f ? "on the surface"
                                                : (std::to_string (height).substr (0, 3) + " m").c_str (),
                                 nullptr, state.sunPointHeight == height))
                state.sunPointHeight = height;
        ImGui::EndMenu ();
    }
    if (ImGui::MenuItem ("Remove study point", nullptr, false, state.sunPointPlaced)) {
        state.sunPointPlaced = false;
        state.sunPointDragging = false;
        state.sunPointSamples.clear ();
        state.sunPointSamplesFor.clear ();
    }
    ImGui::EndPopup ();
}

void DrawSunStudyPointOverlay (HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    state.sunPointScreenValid = false;
    if (!state.sunPointPlaced || viewProj == nullptr || width == 0 || height == 0)
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList ();
    // A lifted point is its own receiver in the air; on the surface, the
    // snapped cell's sphere.
    const bool lifted = state.sunPointHeight > 0.0f && state.sunPointLiftedReady;
    const double* c = lifted ? state.sunPointLifted : state.sunPointCentre;
    ImVec2 centre;
    const bool centreShown = Project (c, viewProj, width, height, centre);
    if (centreShown) {
        // Three great circles: a sphere that reads as one from any side. A zero
        // radius still gets a small one, so the point can be found.
        const double r = lifted ? 0.15 : (std::max) (double (state.sunPointRadius), 0.15);
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
    // The 2D grip sits where the point stands on the surface: drag it along
    // the scene. A lifted point hangs above it on a dashed stem.
    ImVec2 grip;
    if (!Project (state.sunPointAt, viewProj, width, height, grip))
        return;
    if (lifted && centreShown) {
        constexpr int kDashes = 12;
        for (int i = 0; i < kDashes; i += 2) {
            const float a = float (i) / kDashes, b = float (i + 1) / kDashes;
            draw->AddLine (ImVec2 (grip.x + (centre.x - grip.x) * a, grip.y + (centre.y - grip.y) * a),
                           ImVec2 (grip.x + (centre.x - grip.x) * b, grip.y + (centre.y - grip.y) * b), kPointColour,
                           1.5f);
        }
    }
    state.sunPointScreenValid = true;
    state.sunPointScreen[0] = grip.x;
    state.sunPointScreen[1] = grip.y;
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool hot = state.sunPointDragging || std::hypot (mouse.x - grip.x, mouse.y - grip.y) <= kGripRadius;
    const ImU32 fill = hot ? IM_COL32 (255, 255, 255, 240) : IM_COL32 (64, 200, 255, 200);
    const float side = 6.0f;
    draw->AddRectFilled (ImVec2 (grip.x - side, grip.y - side), ImVec2 (grip.x + side, grip.y + side), fill, 2.0f);
    draw->AddRect (ImVec2 (grip.x - side, grip.y - side), ImVec2 (grip.x + side, grip.y + side),
                   IM_COL32 (20, 30, 40, 230), 2.0f, 0, 1.5f);
    // Four arrow ticks: "move in the plane".
    const ImU32 ink = IM_COL32 (20, 30, 40, 230);
    for (int k = 0; k < 4; ++k) {
        const float dx = k == 0 ? 1.0f : k == 1 ? -1.0f : 0.0f, dy = k == 2 ? 1.0f : k == 3 ? -1.0f : 0.0f;
        const ImVec2 tip (grip.x + dx * (side + 6.0f), grip.y + dy * (side + 6.0f));
        const ImVec2 base (grip.x + dx * (side + 2.0f), grip.y + dy * (side + 2.0f));
        draw->AddTriangleFilled (tip, ImVec2 (base.x + dy * 3.0f, base.y + dx * 3.0f),
                                 ImVec2 (base.x - dy * 3.0f, base.y - dx * 3.0f), hot ? fill : ink);
    }
    if (hot && !state.sunPointDragging && !ImGui::IsWindowHovered (ImGuiHoveredFlags_AnyWindow))
        ImGui::SetTooltip ("Drag: move the study point over the scene");
}

void ServiceSunStudyPointGrip (HudState& state, const InputSnapshot& input, uint32_t width, uint32_t height,
                               const float rayOrigin[3], const float rayDirection[3])
{
    if (!state.sunPointPlaced || state.readOnly || state.sunPointStudy.empty () || !state.sunPointScreenValid ||
        width == 0 || height == 0) {
        state.sunPointDragging = false;
        return;
    }
    bool pressed = false, released = false;
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft) {
            pressed = pressed || input.transitions[i].down;
            released = released || !input.transitions[i].down;
        }
    const float x = float (CursorTargetX (input, width)), y = float (CursorTargetY (input, height));
    const bool overGrip =
        input.inside && std::hypot (x - state.sunPointScreen[0], y - state.sunPointScreen[1]) <= kGripRadius;
    if (!state.sunPointDragging && !state.wantsMouse && overGrip && pressed)
        state.sunPointDragging = true;
    // Navigation must not take the same press (the viewpoint gumball's flag).
    if (state.sunPointDragging || (!state.wantsMouse && overGrip && (input.buttons & kMouseLeft)))
        state.viewpointOwnsMouse = true;
    if (!state.sunPointDragging)
        return;
    // Follow the cursor onto the nearest surface the study can see.
    const auto record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (state.sunPointStudy);
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    double hit[3];
    if (record != nullptr && input.inside && evp::sunstudy::SunStudySceneHit (*record, origin, direction, hit) &&
        (hit[0] != state.sunPointAt[0] || hit[1] != state.sunPointAt[1] || hit[2] != state.sunPointAt[2])) {
        std::copy (hit, hit + 3, state.sunPointAt);
        state.sunPointSamplesFor.clear (); // re-snap the cells; the lifted key holds the position
    }
    if (released || !(input.buttons & kMouseLeft))
        state.sunPointDragging = false;
}

bool RefreshSunStudyPointSamples (HudState& state, const std::string& studyId)
{
    if (!state.sunPointPlaced || studyId.empty ())
        return false;
    state.sunPointStudy = studyId;
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

bool RefreshSunStudyLiftedPoint (HudState& state, const std::string& studyId, uint64_t resultVersion)
{
    if (!state.sunPointPlaced || state.sunPointHeight <= 0.0f || studyId.empty ()) {
        state.sunPointLiftedReady = false;
        state.sunPointLiftedFor.clear ();
        return false;
    }
    char key[224];
    std::snprintf (key, sizeof (key), "%s|%llu|%.4f|%.4f|%.4f|%.3f", studyId.c_str (),
                   static_cast<unsigned long long> (resultVersion), state.sunPointAt[0], state.sunPointAt[1],
                   state.sunPointAt[2], double (state.sunPointHeight));
    if (state.sunPointLiftedFor == key)
        return state.sunPointLiftedReady;
    state.sunPointLiftedReady = false;
    const auto record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (studyId);
    evp::sunstudy::LiftedPoint lifted;
    if (record == nullptr)
        return false; // still computing: retried next frame, the key is not latched
    state.sunPointLiftedFor = key;
    if (!evp::sunstudy::LiftSunStudyPoint (*record, state.sunPointAt, double (state.sunPointHeight), lifted) ||
        !evp::sunstudy::LiftedPointDay (*record, lifted, state.sunPointLiftedDay))
        return false;
    std::copy (lifted.position, lifted.position + 3, state.sunPointLifted);
    state.sunPointLiftedHeight = lifted.height;
    state.sunPointLiftedClamped = lifted.clamped;
    state.sunPointLiftedReady = true;
    return true;
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
                      "panel. A placed point pins the diagram, can average the cells within its radius, can be "
                      "dragged by its grip over the scene, and can be lifted to a height (a person's eye level).");
        return false;
    }
    const float spacing = ImGui::GetStyle ().ItemSpacing.x;
    const float remove = ImGui::CalcTextSize ("Remove").x + ImGui::GetStyle ().FramePadding.x * 2.0f;
    const float slider = (std::max) (60.0f, (ImGui::GetContentRegionAvail ().x - remove - 2.0f * spacing) * 0.5f);
    ImGui::SetNextItemWidth (slider);
    char format[48];
    if (state.sunPointHeight > 0.0f)
        std::snprintf (format, sizeof (format), "r %%.1f m, Suncut");
    else
        std::snprintf (format, sizeof (format), "r %%.1f m, %zu cell%s", state.sunPointSamples.size (),
                       state.sunPointSamples.size () == 1 ? "" : "s");
    // A lifted point has no cell sphere, but Suncut still sweeps this radius.
    // Disabling the control here made that cut width impossible to change or reset.
    ImGui::SliderFloat ("##sunpointradius", &state.sunPointRadius, 0.0f, 10.0f, format);
    ImGui::SetItemTooltip ("On a surface, every measured cell within this radius of the snapped point is averaged "
                           "(at most %zu, nearest first); 0 is one cell. For a lifted point, this instead sets "
                           "Suncut's half-width across the slice; 0 resets it to the point alone.",
                           kPointSampleCap);
    ImGui::SameLine ();
    ImGui::SetNextItemWidth (slider);
    ImGui::SliderFloat ("##sunpointheight", &state.sunPointHeight, 0.0f, 5.0f,
                        state.sunPointHeight > 0.0f ? "h %.2f m" : "on surface");
    ImGui::SetItemTooltip ("Height above the surface the point stands on (1.8 m: eye level). Above 0 the point is "
                           "traced as one receiver in the air, never as the surface cells, and it is kept %.0f cm "
                           "below anything overhead so its rays never start inside geometry.",
                           evp::sunstudy::kLiftedPointClearance * 100.0);
    if (ImGui::IsItemDeactivatedAfterEdit () && state.sunPointHeight < 0.05f)
        state.sunPointHeight = 0.0f;
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove##sunpoint")) {
        state.sunPointPlaced = false;
        state.sunPointDragging = false;
        state.sunPointSamples.clear ();
        state.sunPointSamplesFor.clear ();
        state.sunPointLiftedFor.clear ();
        return false;
    }
    return true;
}

namespace {

// A ring section, filled as small quads. ⚠️ NEVER ONE CONVEX FILL: a ring
// section is not convex, and ImGui's convex fill fans it from its first
// vertex -- the stray wedges and triangles across the diagram's centre.
void ArcSector (ImDrawList* draw, ImVec2 centre, float inner, float outer, double from, double to, ImU32 colour)
{
    if (outer <= inner || to <= from)
        return;
    constexpr double kPiece = 0.035; // radians: about two degrees a quad
    const int pieces = (std::max) (1, int (std::ceil ((to - from) / kPiece)));
    const auto at = [&] (float radius, double angle) {
        return ImVec2 (centre.x + radius * float (std::cos (angle)), centre.y + radius * float (std::sin (angle)));
    };
    for (int i = 0; i < pieces; ++i) {
        const double a = from + (to - from) * double (i) / pieces, b = from + (to - from) * double (i + 1) / pieces;
        draw->AddQuadFilled (at (inner, a), at (outer, a), at (outer, b), at (inner, b), colour);
    }
}

ImVec2 ArcPoint (ImVec2 centre, float radius, double angle)
{
    return ImVec2 (centre.x + radius * float (std::cos (angle)), centre.y + radius * float (std::sin (angle)));
}

ImU32 Opaque (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
}

std::string MinuteText (double minute)
{
    const int at = std::clamp (int (std::lround (minute)), 0, 1440);
    char text[8];
    std::snprintf (text, sizeof (text), "%02d:%02d", at / 60, at % 60);
    return text;
}

size_t NearestDayStep (const evp::sunstudy::PointDay& day, size_t steps, double minute)
{
    // A bar's start belongs to that bar, not the preceding step's centre.
    for (size_t s = 0; s < steps; ++s)
        if (minute >= double (day.minutes[s]) && minute < double (day.minutes[s]) + day.hoursPerStep * 60.0)
            return s;
    size_t nearest = 0;
    double distance = 1.0e9;
    for (size_t s = 0; s < steps; ++s) {
        const double d = std::abs (minute - (double (day.minutes[s]) + day.hoursPerStep * 30.0));
        if (d < distance) {
            distance = d;
            nearest = s;
        }
    }
    return nearest;
}

// The swatch-and-word legend under the diagram, one line.
void LegendSwatch (ImU32 colour, const char* text)
{
    const float side = ImGui::GetTextLineHeight () * 0.7f;
    const ImVec2 at = ImGui::GetCursorScreenPos ();
    const float y = at.y + (ImGui::GetTextLineHeight () - side) * 0.5f;
    ImGui::GetWindowDrawList ()->AddRectFilled (ImVec2 (at.x, y), ImVec2 (at.x + side, y + side), colour, 2.0f);
    ImGui::Dummy (ImVec2 (side, ImGui::GetTextLineHeight ()));
    ImGui::SameLine (0.0f, 3.0f);
    ImGui::TextDisabled ("%s", text);
}

} // namespace

void DrawSunStudyDayClock (HudState& state, const evp::sunstudy::PointDay* day, const char* id, int highlightedStep,
                           const char* emptyNote, SunStudyClockSelection* selection, float sizeCap,
                           const SunStudyClockShown* shown, SunChart chart)
{
    // #169: a semicircle from the first to the last measured sun, time running
    // left to right over the top. One measured step is one radial bar: lit
    // inside, shadowed next, sun behind outside. Outside the bars, two rows:
    // the step's dominant state (lit / shadowed / behind), then the time of day
    // (morning / day / evening) in the gradient the obstruction fills share.
    const float width = ImGui::GetContentRegionAvail ().x;
    const float size = (std::min) (sizeCap, width);
    if (size <= 0.0f)
        return;
    const float line = ImGui::GetTextLineHeight ();
    // The solar cut's pink arc, outside the time row, on the point's own day.
    const bool cutArc = selection != nullptr && state.sunObstructionMode == 3 && state.sunView == kSunObstructionView &&
                        (chart == SunChart::Day || chart == SunChart::LiftedDay);
    const float outerMost = size * 0.46f + (cutArc ? 12.0f : 0.0f);
    const float height = outerMost + line * 2.6f;
    const ImVec2 start = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton (id, ImVec2 (width, height));
    const bool info = SunStudyChartInfo (start, width, chart);
    const bool hovered = ImGui::IsItemHovered () && !info;
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    const ImVec2 centre (start.x + width * 0.5f, start.y + outerMost + line * 0.9f);
    const float inner = size * 0.17f, outer = size * 0.36f;
    const float stateFrom = outer + 3.0f, stateTo = outer + size * 0.035f + 3.0f;
    const float timeFrom = stateTo + 2.0f, timeTo = stateTo + size * 0.035f + 2.0f;
    constexpr double kLeft = 3.14159265358979323846, kRight = 2.0 * 3.14159265358979323846;

    const size_t steps =
        day != nullptr ? (std::min) ({ day->minutes.size (), day->lit.size (), day->facing.size () }) : 0;
    double from = 0.0, to = 1440.0;
    const bool domain = steps > 0 && SunStudyDayDomain (*day, from, to);
    const auto angleOf = [&] (double minute) { return SunStudyArcAngle (minute, from, to); };

    // The unmeasured base: what no step covers stays this colour, never shadow.
    const ImU32 unmeasured = IM_COL32 (100, 106, 116, 60);
    ArcSector (draw, centre, inner, outer, kLeft, kRight, unmeasured);
    ArcSector (draw, centre, stateFrom, stateTo, kLeft, kRight, unmeasured);
    if (selection != nullptr)
        *selection = SunStudyClockSelection ();
    const ImVec2 pointer (ImGui::GetIO ().MousePos.x - centre.x, ImGui::GetIO ().MousePos.y - centre.y);
    const float pointerRadius = std::hypot (pointer.x, pointer.y);
    const bool onBars = domain && pointer.y <= 4.0f && pointerRadius >= inner - 2.0f && pointerRadius <= timeTo + 3.0f;
    const float cutR = timeTo + 7.0f;
    const bool onCutBand = cutArc && pointer.y <= 4.0f && pointerRadius > timeTo + 3.0f && pointerRadius <= cutR + 7.0f;
    bool onGrip = false;
    if (cutArc && domain) {
        if (!(state.sunCutTo > state.sunCutFrom)) { // first shown: two hours about the middle of the day
            const double middle = 0.5 * (from + to);
            state.sunCutFrom = (std::max) (from, middle - 60.0);
            state.sunCutTo = (std::min) (to, middle + 60.0);
        }
        state.sunCutFrom = std::clamp (state.sunCutFrom, from, to);
        state.sunCutTo = std::clamp (state.sunCutTo, from, to);
        const ImVec2 gripFrom = ArcPoint (centre, cutR, angleOf (state.sunCutFrom));
        const ImVec2 gripTo = ArcPoint (centre, cutR, angleOf (state.sunCutTo));
        const ImVec2 mouse = ImGui::GetIO ().MousePos;
        const float toFrom = std::hypot (mouse.x - gripFrom.x, mouse.y - gripFrom.y);
        const float toTo = std::hypot (mouse.x - gripTo.x, mouse.y - gripTo.y);
        int near = (std::min) (toFrom, toTo) > 9.0f ? 0 : (toFrom <= toTo ? 1 : 2);
        // The arc's body between the grips: grabbed there, it moves whole.
        const double pointerMinute = SunStudyArcMinute (pointer.x, pointer.y, from, to);
        if (near == 0 && onCutBand && pointerMinute > state.sunCutFrom && pointerMinute < state.sunCutTo)
            near = 3;
        onGrip = hovered && near != 0;
        if (onGrip && ImGui::IsItemActivated ()) {
            state.sunCutGrip = near;
            state.sunCutGrabOffset = pointerMinute - state.sunCutFrom;
        }
        if (state.sunCutGrip != 0 && !ImGui::IsMouseDown (ImGuiMouseButton_Left))
            state.sunCutGrip = 0;
        // Snap to the measured steps' edges: a slice is whole steps.
        const auto snap = [&] (double raw) {
            double snapped = raw, gap = 1.0e9;
            for (size_t s = 0; s < steps; ++s)
                for (const double edge :
                     { double (day->minutes[s]), double (day->minutes[s]) + day->hoursPerStep * 60.0 })
                    if (std::abs (edge - raw) < gap) {
                        gap = std::abs (edge - raw);
                        snapped = edge;
                    }
            return snapped;
        };
        if (state.sunCutGrip == 3 && steps > 0) {
            // Moved whole: the same length, its start snapped, kept on the day.
            const double length = state.sunCutTo - state.sunCutFrom;
            const double start = std::clamp (snap (pointerMinute - state.sunCutGrabOffset), from, to - length);
            state.sunCutFrom = start;
            state.sunCutTo = start + length;
        }
        else if (state.sunCutGrip != 0 && steps > 0) {
            (state.sunCutGrip == 1 ? state.sunCutFrom : state.sunCutTo) = snap (pointerMinute);
            if (state.sunCutFrom > state.sunCutTo) { // crossing over: the grips trade places
                std::swap (state.sunCutFrom, state.sunCutTo);
                state.sunCutGrip = 3 - state.sunCutGrip;
            }
        }
    }
    const uint32_t clockId = ImGui::GetID (id);
    const auto cancelBrush = [&state] {
        state.sunClockBrushActive = state.sunClockBrushDragged = false;
        state.sunClockBrushId = 0;
        state.sunClockBrushFrame = -1;
    };
    // A brush belongs to one continuously visible diagram. A hidden/replaced
    // one must never consume a later, unrelated mouse release.
    if (selection != nullptr && state.sunClockBrushActive &&
        (state.sunClockBrushId != clockId || steps == 0 || ImGui::GetFrameCount () != state.sunClockBrushFrame + 1))
        cancelBrush ();
    if (selection != nullptr && state.sunClockBrushActive && !ImGui::IsMouseDown (ImGuiMouseButton_Left) &&
        !ImGui::IsMouseReleased (ImGuiMouseButton_Left))
        cancelBrush ();
    if (selection != nullptr && domain) {
        if (hovered && ImGui::IsItemActivated () && !onGrip && state.sunCutGrip == 0 && !onCutBand) {
            if (onBars) {
                state.sunClockBrushActive = true;
                state.sunClockBrushDragged = false;
                state.sunClockBrushId = clockId;
                state.sunClockBrushFrom = state.sunClockBrushTo = SunStudyArcMinute (pointer.x, pointer.y, from, to);
            }
            else
                selection->clear = true; // inside the diagram, off the bars: deselect
        }
        if (state.sunClockBrushActive) {
            state.sunClockBrushFrame = ImGui::GetFrameCount ();
            state.sunClockBrushTo = SunStudyArcMinute (pointer.x, pointer.y, from, to);
            state.sunClockBrushDragged =
                state.sunClockBrushDragged || ImGui::IsMouseDragging (ImGuiMouseButton_Left, 2.0f);
            if (ImGui::IsMouseReleased (ImGuiMouseButton_Left)) {
                if (state.sunClockBrushDragged) {
                    selection->slice = true;
                    selection->fromMinute = (std::min) (state.sunClockBrushFrom, state.sunClockBrushTo);
                    selection->toMinute = (std::max) (state.sunClockBrushFrom, state.sunClockBrushTo);
                    if (selection->toMinute - selection->fromMinute < 1.0)
                        selection->slice = false;
                }
                if (!selection->slice)
                    selection->step = int (NearestDayStep (*day, steps, state.sunClockBrushFrom));
                cancelBrush ();
            }
        }
        if (selection->step >= 0)
            highlightedStep = selection->step;
    }
    // Hover previews the exact step; release still commits the click/drag.
    int hoveredStep = -1;
    if (hovered && onBars && steps > 0)
        hoveredStep = int (NearestDayStep (*day, steps, SunStudyArcMinute (pointer.x, pointer.y, from, to)));

    // Direct sun is the diagram's yellow; the map keeps its own sunlit grey.
    const ImU32 lit = Opaque (kSunDiagramDirectRgba), shaded = Opaque (state.sunSingleShadowedRgba);
    const ImU32 behind = IM_COL32 (128, 132, 140, 255);
    double litHours = 0.0, facingHours = 0.0;
    for (size_t s = 0; s < steps; ++s) {
        const double quantum = (std::max) (0.0, day->hoursPerStep * 60.0);
        const double begin = angleOf (day->minutes[s]);
        const double end = angleOf (double (day->minutes[s]) + quantum);
        const double gap = (std::min) (0.006, (end - begin) * 0.12);
        const double a = begin + gap * 0.5, b = end - gap * 0.5;
        const double litShare = std::clamp (day->lit[s], 0.0, 1.0);
        const double facingShare = std::clamp ((std::max) (litShare, day->facing[s]), 0.0, 1.0);
        const float litEdge = inner + (outer - inner) * float (litShare);
        const float facingEdge = inner + (outer - inner) * float (facingShare);
        ArcSector (draw, centre, facingEdge, outer, a, b, behind);
        ArcSector (draw, centre, litEdge, facingEdge, a, b, shaded);
        ArcSector (draw, centre, inner, litEdge, a, b, lit);
        // The state row: the step's dominant state, so lit vs shadowed reads at
        // a glance even where the bars split between cells.
        const double shadeShare = facingShare - litShare, behindShare = 1.0 - facingShare;
        const ImU32 dominant = litShare >= shadeShare && litShare >= behindShare ? lit
                               : shadeShare >= behindShare                       ? shaded
                                                                                 : behind;
        ArcSector (draw, centre, stateFrom, stateTo, begin, end, dominant);
        litHours += day->lit[s] * day->hoursPerStep;
        facingHours += day->facing[s] * day->hoursPerStep;
    }
    // The time row: morning / day / evening, continuous across the domain.
    if (domain) {
        constexpr int kSlices = 48;
        for (int i = 0; i < kSlices; ++i) {
            const double m0 = from + (to - from) * double (i) / kSlices;
            const double m1 = from + (to - from) * double (i + 1) / kSlices;
            ArcSector (draw, centre, timeFrom, timeTo, angleOf (m0), angleOf (m1) + 0.002,
                       Opaque (SunStudyTimeOfDayRgba ((m0 + m1) * 0.5)));
        }
        // Clock ticks every hour and labels every three, on the outside.
        for (int hour = int (std::ceil (from / 60.0)); hour * 60 <= int (to); ++hour) {
            const double angle = angleOf (hour * 60.0);
            const bool major = hour % 3 == 0;
            draw->AddLine (ArcPoint (centre, timeTo + 1.0f, angle),
                           ArcPoint (centre, timeTo + (major ? 5.0f : 2.5f), angle), IM_COL32 (130, 136, 146, 150),
                           1.0f);
            if (major && double (hour * 60) - from > 40.0 && to - double (hour * 60) > 40.0) {
                char text[4];
                std::snprintf (text, sizeof (text), "%d", hour);
                const ImVec2 at = ArcPoint (centre, timeTo + line * 0.75f + (cutArc ? 11.0f : 0.0f), angle);
                const ImVec2 extent = ImGui::CalcTextSize (text);
                draw->AddText (ImVec2 (at.x - extent.x * 0.5f, at.y - extent.y * 0.5f),
                               ImGui::GetColorU32 (ImGuiCol_TextDisabled), text);
            }
        }
    }
    if (cutArc && domain) {
        // The cut slice: a faint track over the day, the slice strong, a grip
        // at each end.
        constexpr ImU32 kPink = IM_COL32 (242, 64, 191, 255);
        ArcSector (draw, centre, cutR - 1.0f, cutR + 1.0f, kLeft, kRight, IM_COL32 (242, 64, 191, 60));
        const float body = state.sunCutGrip == 3 ? 3.5f : 2.5f;
        ArcSector (draw, centre, cutR - body, cutR + body, angleOf (state.sunCutFrom), angleOf (state.sunCutTo), kPink);
        for (int grip = 1; grip <= 2; ++grip) {
            const ImVec2 at = ArcPoint (centre, cutR, angleOf (grip == 1 ? state.sunCutFrom : state.sunCutTo));
            const bool hot = state.sunCutGrip == grip;
            draw->AddCircle (at, hot ? 6.5f : 5.0f, kPink, 0, hot ? 3.0f : 2.0f); // a ring: no fill
        }
        if (onGrip || state.sunCutGrip != 0)
            ImGui::SetTooltip ("%s", SunStudyCutSliceTooltip (state.sunCutFrom, state.sunCutTo).c_str ());
    }
    const ImU32 ink = ImGui::GetColorU32 (ImGuiCol_Text);
    const auto outlineStep = [&] (int step, ImU32 colour, float thickness) {
        if (step < 0 || size_t (step) >= steps)
            return;
        const double begin = angleOf (day->minutes[size_t (step)]);
        const double end = angleOf (double (day->minutes[size_t (step)]) + day->hoursPerStep * 60.0);
        for (const double angle : { begin, end })
            draw->AddLine (ArcPoint (centre, inner - 2.0f, angle), ArcPoint (centre, timeTo + 2.0f, angle), colour,
                           thickness);
        draw->PathClear ();
        draw->PathArcTo (centre, timeTo + 2.0f, float (begin), float (end));
        draw->PathStroke (colour, 0, thickness);
    };
    // The committed slice stays drawn until it is cleared.
    const auto shade = [&] (double a, double b, ImU32 colour) {
        if (b > a)
            ArcSector (draw, centre, inner - 2.0f, timeTo + 2.0f, angleOf (a), angleOf (b), colour);
    };
    bool slice = false;
    double sliceFrom = from, sliceTo = to;
    if (domain && shown != nullptr && shown->sliceTo > shown->sliceFrom &&
        (shown->sliceFrom > from + 0.5 || shown->sliceTo < to - 0.5)) {
        slice = true;
        sliceFrom = (std::max) (from, shown->sliceFrom);
        sliceTo = (std::min) (to, shown->sliceTo);
        shade (from, sliceFrom, IM_COL32 (20, 22, 26, 120)); // dim outside the slice
        shade (sliceTo, to, IM_COL32 (20, 22, 26, 120));
        for (const double edge : { sliceFrom, sliceTo })
            draw->AddLine (ArcPoint (centre, inner - 4.0f, angleOf (edge)),
                           ArcPoint (centre, timeTo + 4.0f, angleOf (edge)), ink, 1.5f);
    }
    if (selection != nullptr && state.sunClockBrushActive && state.sunClockBrushDragged)
        shade ((std::min) (state.sunClockBrushFrom, state.sunClockBrushTo),
               (std::max) (state.sunClockBrushFrom, state.sunClockBrushTo), IM_COL32 (255, 255, 255, 54));
    outlineStep (highlightedStep, ink, 1.5f);
    if (hoveredStep != highlightedStep)
        outlineStep (hoveredStep, IM_COL32 (255, 255, 255, 140), 1.0f);

    if (day != nullptr && day->hasTotalHours) {
        litHours = day->directHours;
        facingHours = day->facingHours;
    }
    const auto centreText = [&] (const char* text, float y, ImU32 colour) {
        const ImVec2 extent = ImGui::CalcTextSize (text);
        draw->AddText (ImVec2 (centre.x - extent.x * 0.5f, centre.y + y - extent.y), colour, text);
    };
    const ImU32 faint = ImGui::GetColorU32 (ImGuiCol_TextDisabled);
    if (!domain) {
        centreText (emptyNote != nullptr ? emptyNote : "", -inner * 0.3f, faint);
        ImGui::Dummy (ImVec2 (0.0f, line));
        SunStudyNote (" ");
        return;
    }
    // The ends: sampled first/last sun, or the run's window when it cut the day.
    const bool cutFrom = day->windowFrom > 0 && from <= double (day->windowFrom) + 0.5;
    const bool cutTo = day->windowTo < 1440 && to >= double (day->windowTo) - 0.5;
    const std::string left = (cutFrom ? "window " : "sun ") + MinuteText (from);
    const std::string right = MinuteText (to) + (cutTo ? " window" : " sun");
    const float baseline = centre.y + 3.0f;
    draw->AddText (ImVec2 (centre.x - timeTo, baseline), faint, left.c_str ());
    const float rightWidth = ImGui::CalcTextSize (right.c_str ()).x;
    draw->AddText (ImVec2 (centre.x + timeTo - rightWidth, baseline), faint, right.c_str ());

    // Centre: total direct sun, then what is selected -- the step or the slice.
    const std::string total = SunStudyClock (litHours, true);
    centreText (total.c_str (), -line * 1.05f, ink);
    centreText ("direct", -line * 0.05f, faint);
    std::string picked;
    if (selection != nullptr && state.sunClockBrushActive && state.sunClockBrushDragged)
        picked = MinuteText ((std::min) (state.sunClockBrushFrom, state.sunClockBrushTo)) + "-" +
                 MinuteText ((std::max) (state.sunClockBrushFrom, state.sunClockBrushTo));
    else if (slice)
        picked = MinuteText (sliceFrom) + "-" + MinuteText (sliceTo);
    else if (highlightedStep >= 0 && size_t (highlightedStep) < steps)
        picked = MinuteText (day->minutes[size_t (highlightedStep)]);
    if (!picked.empty ())
        centreText (picked.c_str (), line * 1.0f, ink);

    if (hovered && hoveredStep >= 0 && !onGrip && state.sunCutGrip == 0) {
        const size_t s = size_t (hoveredStep);
        const double minute = day->minutes[s];
        ImGui::BeginTooltip ();
        ImGui::Text ("%s  %s", MinuteText (minute).c_str (), SunStudyDayPhase (minute));
        ImGui::Text ("lit %d%%  shadow %d%%", int (100.0 * day->lit[s] + 0.5),
                     int (100.0 * (std::max) (0.0, day->facing[s] - day->lit[s]) + 0.5));
        ImGui::Text ("behind %d%%  %zu cell%s", int (100.0 * (1.0 - day->facing[s]) + 0.5), day->samples,
                     day->samples == 1 ? "" : "s");
        ImGui::TextDisabled ("%s", selection != nullptr ? "click step, drag slice" : "display only");
        ImGui::EndTooltip ();
    }
    else if (hovered && selection != nullptr && !onBars && slice)
        ImGui::SetTooltip ("Click: clear the slice");

    LegendSwatch (lit, "sun");
    ImGui::SameLine ();
    LegendSwatch (shaded, "shadow");
    ImGui::SameLine ();
    LegendSwatch (behind, "behind");
    ImGui::SameLine ();
    LegendSwatch (unmeasured, "unmeasured");
    std::string range;
    if (day->samples > 1 && day->hasCellRange)
        range =
            "; cells " + SunStudyClock (day->minDirectHours, true) + "-" + SunStudyClock (day->maxDirectHours, true);
    char summary[192];
    std::snprintf (
        summary, sizeof (summary), "%s direct of %s in front; sun up %s%s", SunStudyClock (litHours, true).c_str (),
        SunStudyClock (facingHours, true).c_str (),
        SunStudyClock (day->hasTotalHours ? day->sunUpHours : double (steps) * day->hoursPerStep, true).c_str (),
        range.c_str ());
    SunStudyNote (summary,
                  "Semicircle from the first to the last measured sun (\"window\" where the run's hours cut the day: "
                  "daylight outside it was not measured). Bars: lit inside, shadowed middle, sun behind the surface "
                  "outside; grey where no step was measured. Outer rows: each step's dominant state, then the time "
                  "of day -- morning violet, day yellow, evening red -- the colours the obstruction view paints its "
                  "blockers in. The centre is total direct sun (mean of the cells) and the selected time.");
}

void ApplySunStudyDaySelection (HudState& state, const SunStudyClockSelection& selection)
{
    if (selection.step >= 0) {
        state.sunPlaying = false;
        state.sunStep = selection.step;
        // A step outside the kept slice widens the range back to the day, or
        // the time slider would clamp it straight back out.
        if (size_t (selection.step) < state.sunStepMinutes.size ()) {
            const double minute = state.sunStepMinutes[size_t (selection.step)];
            if (minute < double (state.sunTimeFrom) * 60.0 || minute >= double (state.sunTimeTo) * 60.0) {
                state.sunTimeFrom = 0.0f;
                state.sunTimeTo = 24.0f;
            }
        }
    }
    else if (selection.slice) {
        state.sunPlaying = false;
        state.sunTimeFrom = float (selection.fromMinute / 60.0);
        state.sunTimeTo = float (selection.toMinute / 60.0);
        // The time slider follows: its step moves into the slice.
        const auto& minutes = state.sunStepMinutes;
        for (size_t s = 0; s < minutes.size (); ++s)
            if (double (minutes[s]) >= selection.fromMinute && double (minutes[s]) < selection.toMinute) {
                state.sunStep = int (s);
                break;
            }
    }
    else if (selection.clear) {
        state.sunTimeFrom = 0.0f;
        state.sunTimeTo = 24.0f;
    }
}

void DrawSunStudyPointDay (HudState& state, const std::string& studyId, uint64_t resultVersion)
{
    if (state.sunReadingState == 1 && state.sunReadingSample != SIZE_MAX && state.sunReadingStudy == studyId) {
        state.sunHoverDayStudy = studyId;
        state.sunHoverDaySample = state.sunReadingSample;
        state.sunHoverDayPointValid = state.sunReadingPointValid;
        if (state.sunReadingPointValid)
            std::copy (state.sunReadingPoint, state.sunReadingPoint + 3, state.sunHoverDayPoint);
    }
    if (!ImGui::CollapsingHeader ("Study point", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const bool pointReady = DrawSunStudyPointHeader (state, studyId);
    const bool pinned = state.sunPointPlaced;
    if (pinned && pointReady && state.sunPointHeight > 0.0f) {
        const bool lifted = RefreshSunStudyLiftedPoint (state, studyId, resultVersion);
        char line[120];
        std::snprintf (line, sizeof (line), "Lifted %.2f m%s: one receiver in the air", state.sunPointLiftedHeight,
                       state.sunPointLiftedClamped ? " (lowered under an overhang)" : "");
        SunStudyNote (lifted ? line : "Tracing the lifted point...",
                      "Traced against the study's own occluders and suns, omnidirectionally: nothing it stands on "
                      "faces away from the sun. The cells' radius does not apply.");
        const int current =
            state.sunStep >= 0 && size_t (state.sunStep) < state.sunPointLiftedDay.minutes.size () ? state.sunStep : -1;
        SunStudyClockSelection selection;
        const SunStudyClockShown shown { double (state.sunTimeFrom) * 60.0, double (state.sunTimeTo) * 60.0 };
        DrawSunStudyDayClock (state, lifted ? &state.sunPointLiftedDay : nullptr, "##sunpointday", current,
                              lifted ? "no sun in this day's slice" : "tracing...", &selection, 352.0f, &shown,
                              SunChart::LiftedDay);
        ApplySunStudyDaySelection (state, selection);
        return;
    }
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
    const SunStudyClockShown shown { double (state.sunTimeFrom) * 60.0, double (state.sunTimeTo) * 60.0 };
    DrawSunStudyDayClock (state, day, "##sunpointday", current, why, &selection, 352.0f, &shown);
    ApplySunStudyDaySelection (state, selection); // selection never changes the current view tab
}

} // namespace geomsrv::archviz
