// The sun path over the model (#152): an optional preview of the solar
// geometry the study uses -- the ground compass from the project's north, the
// solstice and equinox arcs, the chosen date's arc, the hour analemmas, the
// shown analysis in its own form, and a read-only sun reflecting the shown
// analysis selection.
//
// ⚠️ THE LINE WORK IS SCENE GEOMETRY, NOT HUD (#179). This file resolves what
// to show and builds it (SunPathScene.hpp); DiligentScene draws it depth-tested
// at a constant pixel width, so the model occludes it. The HUD keeps only the
// text (compass letters, hour numbers), the sun's hover tooltip and the band's
// hover hit-test.
//
// ⚠️ ITS OWN TRANSLATION UNIT, reading the runner (an architecture exception
// in tools/quality/check_cpp.py): the positions are Archicad's, captured on
// the main thread by NativeCommands/SunStudySunPath, and this file only asks
// for them and draws them.
//
// RENDER THREAD.

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/DiligentHudSunControls.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunPathBandDraw.hpp"
#include "ArchViz/SunPathCompass.hpp"
#include "ArchViz/SunPathLabels.hpp"
#include "ArchViz/SunPathMoment.hpp"
#include "ArchViz/SunPathScene.hpp"
#include "Geometry/MeshStore.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyBounds.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// The fixed arcs, by the runner's kind. The chosen date is lighter than the
// analysis drawn over it on the same date; the analemmas are the faint grid.
uint32_t ArcRgba (int kind)
{
    switch (kind) {
        case 0:
            return SunPathDisplayRgba (90, 140, 245, 220); // winter solstice
        case 1:
            return SunPathDisplayRgba (140, 140, 150, 220); // equinox
        case 2:
            return SunPathDisplayRgba (245, 140, 50, 220); // summer solstice
        case 3:
            return SunPathDisplayRgba (255, 214, 64, 170); // the chosen date
        default:
            return SunPathDisplayRgba (120, 120, 132, 120); // the hours
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

constexpr const char* kCompass[] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                     "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };

// The sunpath's words as world-anchored specs (#182): drawn through the scene
// text layer after the scene, so the model hides them. Compass letters run
// along the ring's tangent; hour numbers sit by the summer arc.
void AppendLabelSpecs (const sunstudyhud::SunPath& path, const double centre[3], double radius, double groundZ,
                       std::vector<SunPathLabelSpec>& specs)
{
    for (int i = 0; i < 16; ++i) {
        const double angle = path.northRadians - double (i) * 2.0 * kPi / 16.0;
        SunPathLabelSpec spec;
        spec.anchor[0] = centre[0] + radius * 1.14 * std::cos (angle);
        spec.anchor[1] = centre[1] + radius * 1.14 * std::sin (angle);
        spec.anchor[2] = groundZ;
        spec.tangent[0] = -radius * 1.14 * 0.001 * std::sin (angle);
        spec.tangent[1] = radius * 1.14 * 0.001 * std::cos (angle);
        spec.text = kCompass[i];
        spec.rgba = i == 0 ? 0xD23C3CFFu : 0x505058FFu;
        specs.push_back (std::move (spec));
    }
    for (const auto& arc : path.arcs)
        if (arc.kind == 2)
            for (const auto& point : arc.points)
                if (point.minute % 120 == 0) {
                    SunPathLabelSpec spec;
                    for (int axis = 0; axis < 3; ++axis)
                        spec.anchor[axis] = centre[axis] + radius * point.direction[axis];
                    spec.text = std::to_string (point.minute / 60);
                    spec.rgba = 0x606068FFu;
                    spec.sizePoints = 12.0f;
                    spec.centered = false;
                    specs.push_back (std::move (spec));
                }
}

// The ImGui fallback, only while the scene text layer is unavailable. It is
// NOT occluded: it paints after the scene with no depth test.
void DrawCompassLabels (const sunstudyhud::SunPath& path, const double centre[3], double radius, double groundZ,
                        const float viewProj[16], uint32_t width, uint32_t height, ImDrawList* draw)
{
    const auto at = [&] (double angle, double ring, ImVec2& screen) {
        const double point[3] = { centre[0] + radius * ring * std::cos (angle),
                                  centre[1] + radius * ring * std::sin (angle), groundZ };
        return Project (point, viewProj, width, height, screen);
    };
    for (int i = 0; i < 16; ++i) {
        const double angle = path.northRadians - double (i) * 2.0 * kPi / 16.0;
        ImVec2 before, after, screen;
        if (at (angle - 0.001, 1.14, before) && at (angle + 0.001, 1.14, after) && at (angle, 1.14, screen) &&
            std::hypot (after.x - before.x, after.y - before.y) > 1.0e-4f)
            CompassLabel (draw, screen, ImVec2 (after.x - before.x, after.y - before.y), kCompass[i],
                          i == 0 ? IM_COL32 (255, 100, 100, 255) : IM_COL32 (230, 230, 230, 225));
    }
}

// FNV-1a over exactly what the scene geometry is built from.
void Mix (uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

template <class T> void MixValue (uint64_t& hash, const T& value)
{
    Mix (hash, &value, sizeof (value));
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
    state.sunPathBandNote.clear ();
    state.sunPathHoverText.clear ();
    state.sunPathSceneKey = 0; // hidden unless this frame publishes geometry below
    state.sunPathLabels.clear ();
    if (!state.sunPathShown) {
        // Release the potentially year-sized storage once on the hidden
        // transition; subsequent hidden frames do no cache work.
        if (state.sunPathBandScreens.valid || !state.sunPathBandScreens.runs.empty ())
            state.sunPathBandScreens = {};
        state.sunPathScene.reset ();
        state.sunPathSceneBuilt = 0;
        return;
    }
    if (viewProj == nullptr || width == 0 || height == 0)
        return;
    // Hidden work is the common case: do not rebuild the normalized band or
    // its projection while the sun path is off.
    state.sunPathBand.Update (state.sunPathDay, state.sunPathPeriod, state.sunShowsPeriod);
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
    // The scene text layer draws the words, depth-tested, next frame at that
    // frame's camera. ImGui only stands in while that layer is unavailable.
    const bool sceneText = state.sunPathSceneText;
    AppendLabelSpecs (*path, centre, radius, groundZ, state.sunPathLabels);
    if (!sceneText)
        DrawCompassLabels (*path, centre, radius, groundZ, viewProj, width, height, draw);
    // Hour numbers on the summer arc, where the analemmas cross it.
    for (const auto& arc : path->arcs)
        if (arc.kind == 2 && !sceneText)
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
    int highlightMonth = sun.valid ? sun.month : 0, highlightDay = sun.valid ? sun.day : 0;
    if (!sun.valid && state.sunPathPeriod != nullptr) {
        const auto& applied = state.sunPathPeriod->applied;
        highlightMonth = applied.month;
        if (applied.frame == 2 && applied.day >= 0 &&
            size_t (applied.day) < state.sunPathPeriod->geometry->days.size ()) {
            const auto& day = state.sunPathPeriod->geometry->days[size_t (applied.day)];
            highlightMonth = day.month;
            highlightDay = day.day;
        }
    }
    const bool pointSlice = selection.source == SunPathSyncSource::PointPlayer && selection.pointActive;
    SunPathAnalysisView view;
    view.month = highlightMonth;
    view.day = highlightDay;
    view.highlightAll = state.sunPathBandHoverFrame == frame;
    view.sliceFrom = pointSlice ? state.sunPointPlayFrom : double (state.sunTimeFrom) * 60.0;
    view.sliceTo = pointSlice ? state.sunPointPlayTo : double (state.sunTimeTo) * 60.0;

    // Hover only: project once per camera/band change and hit-test, no drawing.
    UpdateSunPathBandScreenCache (state.sunPathBandScreens, state.sunPathBand.Data (), state.sunPathBand.Builds (),
                                  viewProj, centre, radius, width, height, at);
    const auto band =
        DrawSunPathBand (nullptr, state.sunPathBand.Data (), sun, highlightMonth, highlightDay, view.highlightAll,
                         view.sliceFrom, view.sliceTo, at, &state.sunPathBandScreens, state.sunPathBand.Builds ());

    // The scene geometry, rebuilt only when what it draws changes.
    const bool estimate = state.sunPathPeriod != nullptr && state.sunPathPeriod->geometry->estimate;
    const auto form = SunPathPreviewFormOf (state.sunPathBand.Data (), state.sunShowsPeriod, estimate);
    uint64_t key = 1469598103934665603ull;
    const void* source = path.get ();
    MixValue (key, source);
    Mix (key, centre, sizeof (centre));
    MixValue (key, radius);
    MixValue (key, groundZ);
    MixValue (key, state.sunPathBand.Builds ());
    MixValue (key, form);
    for (const int value : { view.month, view.day, int (view.highlightAll), int (sun.valid) })
        MixValue (key, value);
    MixValue (key, view.sliceFrom);
    MixValue (key, view.sliceTo);
    if (sun.valid)
        Mix (key, sun.direction, sizeof (sun.direction));
    key |= 1u; // never the "hidden" key
    if (state.sunPathScene == nullptr || state.sunPathSceneBuilt != key) {
        SunPathFrame dome;
        std::copy (centre, centre + 3, dome.centre);
        dome.radius = radius;
        dome.groundZ = groundZ;
        auto scene = std::make_shared<SunPathSceneGeometry> ();
        AppendSunPathCompass (dome, path->northRadians, *scene);
        // The analemmas are the faint grid under everything; the date arcs over them.
        for (const auto& arc : path->arcs)
            AppendSunPathDirections (dome, arc.points, ArcRgba (arc.kind),
                                     arc.kind >= 3 ? scene->fine.depthTested : scene->arcs.depthTested);
        AppendSunPathAnalysis (dome, state.sunPathBand.Data (), form, view, *scene);
        if (sun.valid)
            AppendSunPathSun (dome, sun.direction, *scene);
        state.sunPathScene = std::move (scene);
        state.sunPathSceneBuilt = key;
    }
    state.sunPathSceneKey = key;
    state.sunPathBandNote = SunPathSceneCaption (form, state.sunPathBand.Data (), *state.sunPathScene, estimate);

    const bool overUi = ImGui::IsWindowHovered (ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive ();
    const auto analysis = [&] {
        return SunPathAnalysisCaption (state.sunShowsPeriod, state.sunPeriodLabel, state.sunShownDate, state.sunRunYear,
                                       state.sunRunMonth, state.sunRunDay);
    };
    ImVec2 s;
    if (!sun.valid || !at (sun.direction, s)) {
        // The sun cannot be placed; the band still has a date/period caption,
        // but never the analysis-step count the hover tooltip used to expose.
        if (band.hot && !overUi) {
            state.sunPathHoverText = analysis ();
            if (!state.sunPathHoverText.empty ())
                geomsrv::archviz::hudshell::HoverTip ("%s", state.sunPathHoverText.c_str ());
        }
        return;
    }
    std::copy (sun.direction, sun.direction + 3, state.sunPathDirection);
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool hot = !overUi && std::hypot (mouse.x - s.x, mouse.y - s.y) <= kSunHoverRadius;
    state.sunPathMarkerValid = true;
    state.sunPathMarker[0] = s.x;
    state.sunPathMarker[1] = s.y;
    if (hot) {
        state.sunPathHoverText =
            SunPathHoverCaption (SunPathMomentCaption (sun.year, sun.month, sun.day, sun.minute), analysis ());
        if (sceneText) {
            // One plate per line, anchored at the sun: the scene depth there
            // hides the tooltip with the sun it describes (#182).
            const std::string& text = state.sunPathHoverText;
            float offsetY = -4.0f;
            size_t begin = 0;
            while (begin <= text.size ()) {
                const size_t end = (std::min) (text.find ('\n', begin), text.size ());
                SunPathLabelSpec line;
                for (int axis = 0; axis < 3; ++axis)
                    line.anchor[axis] = centre[axis] + radius * sun.direction[axis];
                line.text = text.substr (begin, end - begin);
                line.rgba = 0x141414FFu;
                line.haloRgba = 0;
                line.centered = false;
                line.offsetX = 10.0f;
                line.offsetY = offsetY;
                line.panel = false; // plain text, as the owner asked: no white plates
                state.sunPathLabels.push_back (std::move (line));
                offsetY += 18.0f;
                begin = end + 1;
            }
            return;
        }
        // The disc itself is scene geometry; only the hover affordance is HUD.
        ImDrawList* front = ImGui::GetForegroundDrawList ();
        front->AddCircle (s, 10.0f, IM_COL32 (60, 40, 0, 130), 0, 1.0f);
        const ImVec2 textAt (s.x + 12.0f, s.y - 7.0f);
        const ImVec2 size = ImGui::CalcTextSize (state.sunPathHoverText.c_str ());
        front->AddRectFilled (ImVec2 (textAt.x - 5.0f, textAt.y - 4.0f),
                              ImVec2 (textAt.x + size.x + 5.0f, textAt.y + size.y + 4.0f),
                              IM_COL32 (245, 245, 235, 240), 3.0f);
        front->AddText (textAt, IM_COL32 (20, 20, 20, 255), state.sunPathHoverText.c_str ());
    }
}

} // namespace geomsrv::archviz
