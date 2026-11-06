// The sun path over the model (#152): an optional preview of the solar
// geometry the study uses -- the horizon ring with the compass points from the
// project's north, the solstice and equinox arcs, the chosen date's arc, the
// hour analemmas, and the sun at the shadow view's time.
//
// ⚠️ ITS OWN TRANSLATION UNIT, reading the runner (an architecture exception
// in tools/quality/check_cpp.py): the positions are Archicad's, captured on
// the main thread by NativeCommands/SunStudySunPath, and this file only asks
// for them and draws them.
//
// RENDER THREAD.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyBounds.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace geomsrv::archviz {

namespace {

constexpr double kPi = 3.14159265358979323846;

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

// Where the dome stands and how big it is: on the study point if one is
// placed, else on the scene's plan centre at its floor; a radius a little past
// the scene's half diagonal.
bool Dome (const HudState& state, double centre[3], double& radius)
{
    static const void* seen = nullptr;
    static uint64_t seenId = 0;
    static bool valid = false;
    static double lo[3] = {}, hi[3] = {};
    auto snapshot = MeshStore::Get ().Current ();
    if (snapshot == nullptr)
        snapshot = MeshStore::Get ().Shared ();
    if (snapshot != nullptr && (snapshot.get () != seen || snapshot->id != seenId)) {
        seen = snapshot.get ();
        seenId = snapshot->id;
        valid = evp::sunstudy::SnapshotBounds (*snapshot, lo, hi);
    }
    if (!valid && !state.sunPointPlaced)
        return false;
    radius = valid ? (std::max) (10.0, 0.6 * std::hypot (hi[0] - lo[0], hi[1] - lo[1])) : 20.0;
    if (state.sunPointPlaced)
        std::copy (state.sunPointCentre, state.sunPointCentre + 3, centre);
    else {
        centre[0] = 0.5 * (lo[0] + hi[0]);
        centre[1] = 0.5 * (lo[1] + hi[1]);
        centre[2] = lo[2];
    }
    return true;
}

ImU32 ArcColour (int kind)
{
    switch (kind) {
        case 0:
            return IM_COL32 (110, 160, 255, 220); // winter solstice
        case 1:
            return IM_COL32 (200, 200, 200, 200); // equinox
        case 2:
            return IM_COL32 (255, 150, 60, 220); // summer solstice
        case 3:
            return IM_COL32 (255, 225, 90, 255); // the chosen date
        default:
            return IM_COL32 (200, 200, 210, 90); // the hours
    }
}

} // namespace

void DrawSunStudySunPath (HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    if (!state.sunPathShown || viewProj == nullptr || width == 0 || height == 0)
        return;
    sunstudyhud::RequestSunPath (state.sunRunYear, state.sunRunMonth, state.sunRunDay);
    const auto path = sunstudyhud::LatestSunPath ();
    double centre[3] = {}, radius = 0.0;
    if (path == nullptr || !path->ready || !path->error.empty () || !Dome (state, centre, radius))
        return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList ();
    const auto at = [&] (const float direction[3], ImVec2& screen) {
        const double p[3] = { centre[0] + radius * direction[0], centre[1] + radius * direction[1],
                              centre[2] + radius * direction[2] };
        return Project (p, viewProj, width, height, screen);
    };
    // The horizon and the compass points, from the project's north.
    constexpr int kSegments = 96;
    ImVec2 previous;
    bool had = false;
    for (int i = 0; i <= kSegments; ++i) {
        const double a = 2.0 * kPi * double (i) / double (kSegments);
        const float d[3] = { float (std::cos (a)), float (std::sin (a)), 0.0f };
        ImVec2 s;
        const bool ok = at (d, s);
        if (ok && had)
            draw->AddLine (previous, s, IM_COL32 (230, 230, 230, 120), 1.0f);
        previous = s;
        had = ok;
    }
    constexpr const char* kCompass[] = { "N", "W", "S", "E" }; // CCW from north
    for (int i = 0; i < 4; ++i) {
        const double a = path->northRadians + double (i) * kPi * 0.5;
        const float d[3] = { float (std::cos (a)), float (std::sin (a)), 0.0f };
        ImVec2 s;
        if (at (d, s))
            draw->AddText (ImVec2 (s.x - 4.0f, s.y - 7.0f),
                           i == 0 ? IM_COL32 (255, 90, 90, 255) : IM_COL32 (230, 230, 230, 220), kCompass[i]);
    }
    // The arcs, the analemmas faint underneath.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& arc : path->arcs) {
            if ((arc.kind == 4) != (pass == 0))
                continue;
            const ImU32 colour = ArcColour (arc.kind);
            const float thickness = arc.kind == 3 ? 2.5f : arc.kind == 4 ? 1.0f : 1.5f;
            had = false;
            for (const auto& point : arc.points) {
                ImVec2 s;
                const bool ok = at (point.direction, s);
                if (ok && had)
                    draw->AddLine (previous, s, colour, thickness);
                previous = s;
                had = ok;
            }
        }
    // Hour numbers on the summer arc, where the analemmas cross it.
    for (const auto& arc : path->arcs)
        if (arc.kind == 2)
            for (const auto& point : arc.points)
                if (point.minute % 120 == 0) {
                    ImVec2 s;
                    if (at (point.direction, s)) {
                        char text[8];
                        std::snprintf (text, sizeof (text), "%d", point.minute / 60);
                        draw->AddText (ImVec2 (s.x + 3.0f, s.y - 14.0f), IM_COL32 (230, 230, 230, 200), text);
                    }
                }
    // The sun at the single shadow's time, on the chosen date's arc.
    if (state.sunStep >= 0 && size_t (state.sunStep) < state.sunStepMinutes.size ()) {
        const int minute = state.sunStepMinutes[size_t (state.sunStep)];
        int bestGap = 1 << 30;
        const sunstudyhud::SunPathPoint* sun = nullptr;
        for (const auto& arc : path->arcs)
            if (arc.kind == 3)
                for (const auto& point : arc.points)
                    if (const int gap = std::abs (point.minute - minute); gap < bestGap) {
                        bestGap = gap;
                        sun = &point;
                    }
        ImVec2 s;
        if (sun != nullptr && bestGap <= 10 && at (sun->direction, s)) {
            draw->AddCircleFilled (s, 7.0f, IM_COL32 (255, 220, 60, 255));
            draw->AddCircle (s, 7.0f, IM_COL32 (60, 40, 0, 255), 0, 1.5f);
        }
    }
}

} // namespace geomsrv::archviz
