// The sun path over the model (#152): an optional preview of the solar
// geometry the study uses -- the horizon ring with the compass points from the
// project's north, the solstice and equinox arcs, the chosen date's arc, the
// hour analemmas, and a read-only sun reflecting the shown analysis selection.
//
// ⚠️ ITS OWN TRANSLATION UNIT, reading the runner (an architecture exception
// in tools/quality/check_cpp.py): the positions are Archicad's, captured on
// the main thread by NativeCommands/SunStudySunPath, and this file only asks
// for them and draws them.
//
// RENDER THREAD.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunPathCompass.hpp"
#include "ArchViz/SunPathMoment.hpp"
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

// The preview is fixed to the scene's plan centre and floor, independently of
// the analysis point. Radius remains a little past the scene's half diagonal.
bool Dome (const HudState& state, double centre[3], double& radius, double& groundZ)
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
    if (snapshot == nullptr || !valid)
        return false;
    if (!SunPathSceneDome (lo, hi, state.sunPathScale, centre, radius))
        return false;
    groundZ = centre[2];
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

constexpr float kSunHoverRadius = 11.0f;

void CompassLabel (ImDrawList* draw, ImVec2 centre, ImVec2 tangent, const char* text, ImU32 colour)
{
    const ImVec2 size = ImGui::CalcTextSize (text);
    const int firstVertex = draw->VtxBuffer.Size;
    draw->AddText (ImVec2 (centre.x - size.x * 0.5f, centre.y - size.y * 0.5f), colour, text);
    const float angle = float (SunPathCompassAngle (tangent.x, tangent.y));
    const float c = std::cos (angle), s = std::sin (angle);
    for (int i = firstVertex; i < draw->VtxBuffer.Size; ++i) {
        ImDrawVert& vertex = draw->VtxBuffer[i];
        const float x = vertex.pos.x - centre.x, y = vertex.pos.y - centre.y;
        vertex.pos = ImVec2 (centre.x + c * x - s * y, centre.y + s * x + c * y);
    }
}

// A projected ground overlay, like the dome's line work: no depth occlusion is
// implied. Its centre and Z are shared with the scene-centred dome.
void DrawGroundCompass (const sunstudyhud::SunPath& path, const double centre[3], double radius, double groundZ,
                        const float viewProj[16], uint32_t width, uint32_t height, ImDrawList* draw)
{
    const auto at = [&] (double angle, double ring, ImVec2& screen) {
        const double point[3] = { centre[0] + radius * ring * std::cos (angle),
                                  centre[1] + radius * ring * std::sin (angle), groundZ };
        return Project (point, viewProj, width, height, screen);
    };
    constexpr int kSegments = 128;
    constexpr double kRings[] = { 1.0, 1.035, 1.07 };
    for (int ring = 0; ring < 3; ++ring) {
        ImVec2 previous;
        bool had = false;
        for (int i = 0; i <= kSegments; ++i) {
            const double angle = 2.0 * kPi * double (i) / double (kSegments);
            ImVec2 screen;
            const bool ok = at (angle, kRings[ring], screen);
            if (ok && had)
                draw->AddLine (previous, screen, IM_COL32 (225, 225, 225, ring == 0 ? 150 : 105), 1.0f);
            previous = screen;
            had = ok;
        }
    }
    // Thirty-two ticks, with the eight principal directions reaching the inner
    // ring. Sixteen labels use compass bearings clockwise from project north.
    for (int i = 0; i < 32; ++i) {
        const double angle = path.northRadians - double (i) * 2.0 * kPi / 32.0;
        ImVec2 from, to;
        if (at (angle, i % 4 == 0 ? 1.0 : 1.035, from) && at (angle, 1.07, to))
            draw->AddLine (from, to, IM_COL32 (225, 225, 225, i % 4 == 0 ? 180 : 120), 1.0f);
    }
    constexpr const char* kCompass[] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                         "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    for (int i = 0; i < 16; ++i) {
        const double angle = path.northRadians - double (i) * 2.0 * kPi / 16.0;
        ImVec2 before, after, screen;
        if (at (angle - 0.001, 1.14, before) && at (angle + 0.001, 1.14, after) && at (angle, 1.14, screen) &&
            std::hypot (after.x - before.x, after.y - before.y) > 1.0e-4f)
            CompassLabel (draw, screen, ImVec2 (after.x - before.x, after.y - before.y), kCompass[i],
                          i == 0 ? IM_COL32 (255, 100, 100, 255) : IM_COL32 (230, 230, 230, 225));
    }
}

} // namespace

void RefreshSunStudySunPathData (HudState& state, const std::string& shownStudyId)
{
    const auto previousId = state.sunPathDay.studyId;
    const auto previousRevision = state.sunPathDay.revision;
    sunstudyhud::ReadSunPathStudyDay (shownStudyId, state.sunPathDay);
    state.sunPathPeriod = sunstudyhud::LatestSunPathPeriod ();
    if (state.sunPathPeriod != nullptr &&
        (state.sunPathPeriod->geometry == nullptr || state.sunPathPeriod->geometry->studyId != shownStudyId ||
         state.sunPathPeriod->geometry->revision != state.sunPathDay.revision))
        state.sunPathPeriod.reset ();
    state.sunShowsPeriod =
        state.sunPathPeriod != nullptr || (!shownStudyId.empty () && shownStudyId == state.sunPeriodStudyId);
    state.sunShownDateFor = state.sunPathDay.available ? shownStudyId : std::string ();
    state.sunShownDate = state.sunPathDay.available
                             ? state.sunPathDay.year * 10000 + state.sunPathDay.month * 100 + state.sunPathDay.day
                             : 0;
    if (previousId != state.sunPathDay.studyId || previousRevision != state.sunPathDay.revision) {
        state.sunPathSyncSource = SunPathSyncSource::None;
        state.sunPathHover = {};
        state.sunPathHoverFrame = -1;
    }
}

void DrawSunStudySunPath (HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    state.sunPathMarkerValid = false;
    state.sunPathMinute = -1;
    if (!state.sunPathShown || viewProj == nullptr || width == 0 || height == 0)
        return;
    // Fixed arcs follow the shown study/run date, never transient chart hover.
    const auto& dayData = state.sunPathDay;
    sunstudyhud::RequestSunPath (dayData.available ? dayData.year : state.sunRunYear,
                                 dayData.available ? dayData.month : state.sunRunMonth,
                                 dayData.available ? dayData.day : state.sunRunDay);
    const auto path = sunstudyhud::LatestSunPath ();
    double centre[3] = {}, radius = 0.0, groundZ = 0.0;
    if (path == nullptr || !path->ready || !path->error.empty () || !Dome (state, centre, radius, groundZ))
        return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList ();
    const auto at = [&] (const float direction[3], ImVec2& screen) {
        const double p[3] = { centre[0] + radius * direction[0], centre[1] + radius * direction[1],
                              centre[2] + radius * direction[2] };
        return Project (p, viewProj, width, height, screen);
    };
    DrawGroundCompass (*path, centre, radius, groundZ, viewProj, width, height, draw);
    // The arcs, the analemmas faint underneath.
    ImVec2 previous;
    bool had = false;
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
    SunPathSelection selection;
    const int frame = ImGui::GetFrameCount ();
    selection.hover = state.sunPathHover;
    selection.hoverCurrent = state.sunPathHoverFrame == frame;
    selection.shadowActive = state.sunPathShadowFrame == frame;
    selection.mapActive = state.sunPathMapFrame == frame;
    selection.pointActive = state.sunPathPointFrame == frame;
    selection.point = state.sunPathPointMoment;
    if ((state.sunPathSyncSource == SunPathSyncSource::Shadow && !selection.shadowActive) ||
        (state.sunPathSyncSource == SunPathSyncSource::MapPlayer && !selection.mapActive) ||
        (state.sunPathSyncSource == SunPathSyncSource::PointPlayer && !selection.pointActive))
        state.sunPathSyncSource = SunPathSyncSource::None;
    selection.source = state.sunPathSyncSource;
    selection.aggregate = state.sunShowsPeriod;
    if (dayData.available) {
        int step = state.sunStep;
        if (step < 0 || size_t (step) >= dayData.steps.size ()) {
            step = -1;
            for (size_t i = 0; i < dayData.steps.size (); ++i)
                if (step < 0 || dayData.steps[i].direction[2] > dayData.steps[size_t (step)].direction[2])
                    step = int (i);
        }
        selection.day = SunPathStepMoment (dayData.year, dayData.month, dayData.day, dayData.steps, step);
        selection.shadow = selection.day;
    }
    if (state.sunPathPeriod != nullptr) {
        selection.map = SunPathAppliedMoment (*state.sunPathPeriod);
        // A committed kept-day instant is already exact. Pending shadow date
        // fields deliberately never enter the resolver.
        if (state.sunPathPeriod->applied.frame == 1) {
            selection.shadow = selection.map;
            if (selection.source == SunPathSyncSource::Shadow && state.sunStep >= 0 &&
                size_t (state.sunStep) < state.sunStepMinutes.size () && state.sunPathPeriod->applied.day >= 0 &&
                size_t (state.sunPathPeriod->applied.day) < state.sunPathPeriod->geometry->days.size ()) {
                const auto& day = state.sunPathPeriod->geometry->days[size_t (state.sunPathPeriod->applied.day)];
                const int minute = state.sunStepMinutes[size_t (state.sunStep)];
                selection.shadow = SunPathNearestMoment (state.sunPathPeriod->geometry->year, day.month, day.day,
                                                         day.steps, minute, minute, minute + 1);
            }
        }
    }
    SunPathMoment sun = ResolveSunPathMoment (selection);
    if (!dayData.available && !state.sunShowsPeriod) {
        // Before any analysis, only the captured chosen-date arc supplies a sun.
        for (const auto& arc : path->arcs)
            if (arc.kind == 3)
                for (const auto& point : arc.points)
                    if (!sun.valid || point.direction[2] > sun.direction[2]) {
                        sun.valid = true;
                        sun.year = path->year;
                        sun.month = path->month;
                        sun.day = path->day;
                        sun.minute = point.minute;
                        std::copy (point.direction, point.direction + 3, sun.direction);
                    }
    }
    state.sunPathMinute = sun.valid ? sun.minute : -1;
    ImVec2 s;
    if (!sun.valid || !at (sun.direction, s))
        return;
    std::copy (sun.direction, sun.direction + 3, state.sunPathDirection);
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool hot = std::hypot (mouse.x - s.x, mouse.y - s.y) <= kSunHoverRadius;
    ImDrawList* front = ImGui::GetForegroundDrawList ();
    front->AddCircleFilled (s, hot ? 9.0f : 7.0f, IM_COL32 (255, 220, 60, 255));
    if (hot)
        front->AddCircle (s, 9.0f, IM_COL32 (60, 40, 0, 130), 0, 1.0f);
    state.sunPathMarkerValid = true;
    state.sunPathMarker[0] = s.x;
    state.sunPathMarker[1] = s.y;
    if (hot) {
        std::string label = SunPathMomentCaption (sun.year, sun.month, sun.day, sun.minute);
        const std::string analysis =
            SunPathAnalysisCaption (state.sunShowsPeriod, state.sunPeriodLabel, state.sunShownDate, state.sunRunYear,
                                    state.sunRunMonth, state.sunRunDay);
        if (!analysis.empty ())
            label += (label.empty () ? "" : "\n") + analysis;
        const ImVec2 textAt (s.x + 12.0f, s.y - 7.0f);
        const ImVec2 size = ImGui::CalcTextSize (label.c_str ());
        front->AddRectFilled (ImVec2 (textAt.x - 5.0f, textAt.y - 4.0f),
                              ImVec2 (textAt.x + size.x + 5.0f, textAt.y + size.y + 4.0f),
                              IM_COL32 (245, 245, 235, 240), 3.0f);
        front->AddText (textAt, IM_COL32 (20, 20, 20, 255), label.c_str ());
    }
}

} // namespace geomsrv::archviz
