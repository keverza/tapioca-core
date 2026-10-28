#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace geomsrv::archviz::viewerhud {

namespace {

enum class RoleEdit { Replace, Add, Remove };

void EditRole (std::vector<std::string>& role, std::vector<std::string>& other, std::vector<std::string>& third,
               const std::vector<std::string>& selection, RoleEdit edit)
{
    std::set<std::string> values (role.begin (), role.end ());
    if (edit == RoleEdit::Replace)
        values.clear ();
    for (const auto& guid : selection) {
        if (edit == RoleEdit::Remove)
            values.erase (guid);
        else
            values.insert (guid);
    }
    role.assign (values.begin (), values.end ());
    if (edit != RoleEdit::Remove) {
        const std::set<std::string> kept (role.begin (), role.end ());
        other.erase (std::remove_if (other.begin (), other.end (),
                                     [&] (const std::string& guid) { return kept.find (guid) != kept.end (); }),
                     other.end ());
        third.erase (std::remove_if (third.begin (), third.end (),
                                     [&] (const std::string& guid) { return kept.find (guid) != kept.end (); }),
                     third.end ());
    }
}

bool RoleRow (const char* label, std::vector<std::string>& role, std::vector<std::string>& other,
              std::vector<std::string>& third, const std::vector<std::string>& selection, const char* tip = nullptr)
{
    bool changed = false;
    ImGui::PushID (label);
    ImGui::Text ("%s  %u element(s)", label, unsigned (role.size ()));
    if (tip != nullptr)
        ImGui::SetItemTooltip ("%s", tip);
    const bool haveSelection = !selection.empty ();
    const auto fromSelection = [&selection] {
        ImGui::SetItemTooltip ("From the viewer selection: %u element(s)", unsigned (selection.size ()));
    };
    ImGui::BeginDisabled (!haveSelection);
    if (ImGui::SmallButton ("Replace")) {
        EditRole (role, other, third, selection, RoleEdit::Replace);
        changed = true;
    }
    fromSelection ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Add")) {
        EditRole (role, other, third, selection, RoleEdit::Add);
        changed = true;
    }
    fromSelection ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove")) {
        EditRole (role, other, third, selection, RoleEdit::Remove);
        changed = true;
    }
    fromSelection ();
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Clear")) {
        role.clear ();
        changed = true;
    }
    ImGui::PopID ();
    return changed;
}

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

void Normalize (double value[3])
{
    const double length = std::sqrt (value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length <= 1.0e-12)
        return;
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

void Cross (const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// The centre of Focus's bounding box, where an untouched cone target sits.
bool FocusCentre (const Snapshot& snapshot, const std::vector<std::string>& focus, double centre[3])
{
    std::set<std::string> wanted;
    for (const auto& guid : focus)
        wanted.insert (evp::sunstudy::CanonicalGuid (guid));
    double low[3] = { 1.0e300, 1.0e300, 1.0e300 }, high[3] = { -1.0e300, -1.0e300, -1.0e300 };
    bool any = false;
    for (const auto& mesh : snapshot.meshes) {
        if (wanted.find (evp::sunstudy::CanonicalGuid (mesh.guid)) == wanted.end ())
            continue;
        for (size_t i = 0; i + 2 < mesh.vertices.size (); i += 3)
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = (std::min) (low[axis], mesh.vertices[i + axis]);
                high[axis] = (std::max) (high[axis], mesh.vertices[i + axis]);
                any = true;
            }
    }
    if (!any)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        centre[axis] = 0.5 * (low[axis] + high[axis]);
    return true;
}

// The cone looks from the point at its target; a target on the point falls back
// to the bearing the placement click gave it.
void ConeDirection (const HudState& state, const double point[3], double direction[3])
{
    double length = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        direction[axis] = state.visibilityTarget[axis] - point[axis];
        length += direction[axis] * direction[axis];
    }
    if (!state.visibilityTargetSet || length < 1.0e-12)
        std::copy (state.visibilityDirection, state.visibilityDirection + 3, direction);
    Normalize (direction);
}

visibilitystudy::Request MakeRequest (const HudState& state, const viewpointstudy::Settings& shared,
                                      std::shared_ptr<const Snapshot> capture)
{
    visibilitystudy::Request request;
    request.snapshot = std::move (capture);
    request.fromElements = state.visibilityFrom;
    request.toElements = state.visibilityTo;
    // Without an explicit Context the whole capture occludes.
    request.options.explicitContext = !shared.contextAll;
    request.options.contextElements = shared.context;
    request.options.domain = evp::sunstudy::SamplingDomain::SurfacePatch;
    request.options.origin = state.visibilityOrigin == 0 ? evp::sunstudy::VisibilityOrigin::Surfaces
                                                         : evp::sunstudy::VisibilityOrigin::Point;
    request.options.spacing = state.visibilityGrid;
    request.options.maxAimPoints = static_cast<size_t> ((std::max) (1, state.visibilityAimPoints));
    request.options.view =
        state.visibilityView == 1 ? evp::sunstudy::VisibilityView::Sphere : evp::sunstudy::VisibilityView::Cone;
    request.options.coneDegrees = state.visibilityConeDegrees;
    request.options.minElevationDegrees = state.visibilityGroundDegrees;
    request.options.maxElevationDegrees = state.visibilitySkyDegrees;
    std::copy (shared.options.point.begin (), shared.options.point.end (), request.options.point);
    ConeDirection (state, request.options.point, request.options.direction);
    return request;
}

// Everything a point run depends on: a different key is a different result.
std::string LiveKey (const visibilitystudy::Request& request, const viewpointstudy::Settings& shared)
{
    const auto& o = request.options;
    char numbers[320];
    std::snprintf (numbers, sizeof (numbers), "%llu|%.6f,%.6f,%.6f|%.6f,%.6f,%.6f|%d|%.3f|%.3f,%.3f|%.4f|%d",
                   static_cast<unsigned long long> (request.snapshot != nullptr ? request.snapshot->id : 0), o.point[0],
                   o.point[1], o.point[2], o.direction[0], o.direction[1], o.direction[2], int (o.view), o.coneDegrees,
                   o.minElevationDegrees, o.maxElevationDegrees, o.spacing, shared.contextAll ? 1 : 0);
    std::string key = numbers;
    for (const auto& guid : request.toElements)
        key += "|" + guid;
    key += "#";
    for (const auto& guid : shared.context)
        key += "|" + guid;
    return key;
}

// Point visibility's own controls: the view, the point, and the cone's target or
// the sphere's band. Returns true when the result they describe changed.
bool DrawPointControls (HudState& state, viewpointstudy::Settings& shared, const Snapshot* capture)
{
    bool changed = false;
    const char* const views[] = { "View cone", "View sphere" };
    changed |= Choice ("##visibility-view", state.visibilityView, views, 2);
    ImGui::SetItemTooltip ("Cone: what the point sees toward its target. Sphere: all round, within a band of "
                           "elevations.");
    // The Viewpoint tab's point: placed, typed or dragged in either tab.
    if (ImGui::Button (state.viewpointPlacing ? "Click a model surface..." : "Place view point")) {
        state.viewpointPlacing = true;
        state.viewpointPlacingTarget = false;
        state.viewpointPlacementMiss = false;
        state.viewpointDragging = false;
    }
    if (ImGui::InputScalarN ("Point XYZ (m)", ImGuiDataType_Double, shared.options.point.data (), 3, nullptr, nullptr,
                             "%.2f")) {
        shared.pointValid = true;
        ConfigureViewpoint (state, shared);
    }
    ImGui::SetItemTooltip ("Or drag the gumball; the Viewpoint tab shares this point.");
    if (state.viewpointPlacementMiss)
        ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "Point missed the model; click a surface.");
    if (state.visibilityView == 0) {
        // Until the user moves it, the target follows Focus's centre.
        if (!state.visibilityTargetEdited && capture != nullptr) {
            std::string key = std::to_string (capture->id);
            for (const auto& guid : state.visibilityTo)
                key += "|" + guid;
            if (key != state.visibilityTargetFollowKey) {
                state.visibilityTargetFollowKey = key;
                double centre[3];
                if (FocusCentre (*capture, state.visibilityTo, centre)) {
                    std::copy (centre, centre + 3, state.visibilityTarget);
                    state.visibilityTargetSet = true;
                    changed = true;
                }
            }
        }
        if (ImGui::InputScalarN ("Target XYZ (m)", ImGuiDataType_Double, state.visibilityTarget, 3, nullptr, nullptr,
                                 "%.2f")) {
            state.visibilityTargetSet = true;
            state.visibilityTargetEdited = true;
            changed = true;
        }
        ImGui::SetItemTooltip ("Where the cone looks: Focus's centre until you move it. The cone keeps its target "
                               "when the point moves; drag the target's gumball too.");
        if (ImGui::SmallButton (state.viewpointPlacingTarget ? "Click a model surface...##target" : "Place target")) {
            state.viewpointPlacingTarget = true;
            state.viewpointPlacing = false;
            state.viewpointDragging = false;
        }
        ImGui::SetItemTooltip ("Click the model where the cone should look.");
        ImGui::SameLine ();
        ImGui::BeginDisabled (!state.visibilityTargetEdited);
        if (ImGui::SmallButton ("Focus centre")) {
            state.visibilityTargetEdited = false;
            state.visibilityTargetFollowKey.clear ();
        }
        ImGui::SetItemTooltip ("Aim at the centre of Focus's bounding box again, and follow it.");
        ImGui::EndDisabled ();
        changed |= ImGui::SliderFloat ("cone angle", &state.visibilityConeDegrees, 10.0f, 179.0f, "%.0f deg");
        ImGui::SetItemTooltip ("The cone's full opening angle.");
    }
    else {
        changed |= ImGui::SliderFloat ("sky up to", &state.visibilitySkyDegrees, -89.0f, 90.0f, "%.0f deg");
        ImGui::SetItemTooltip ("The highest elevation seen: 90 is the whole sky, 0 none of it.");
        changed |= ImGui::SliderFloat ("ground down to", &state.visibilityGroundDegrees, -90.0f, 89.0f, "%.0f deg");
        ImGui::SetItemTooltip ("The lowest elevation seen: -90 straight down, 0 nothing below the horizon.");
        // Keep a band at least a degree wide; the edited end wins.
        if (state.visibilityGroundDegrees > state.visibilitySkyDegrees - 1.0f) {
            if (ImGui::IsItemEdited ())
                state.visibilitySkyDegrees = (std::min) (90.0f, state.visibilityGroundDegrees + 1.0f);
            else
                state.visibilityGroundDegrees = (std::max) (-90.0f, state.visibilitySkyDegrees - 1.0f);
        }
    }
    return changed;
}

} // namespace

void DrawVisibilityHudSection (HudState& state, const DiligentSceneStats& scene, int origin)
{
    const auto status = visibilitystudy::GetStatus ();
    // Switching between the two visibility tabs only hides the other's result
    // (AnalysisTabs.hpp); a change here clears a result only when it is this tab's.
    state.visibilityOrigin = origin;
    const bool live = origin == 1;
    const bool ownResult =
        scene.sunStudy.drawing && scene.sunStudy.analysisKind == 1 && state.analysisFocus.shownOrigin == origin;
    const auto capture = visibilitystudy::ViewerCapture ();
    bool changed = false;
    // A point run follows its controls live, so they stay usable while it runs.
    ImGui::BeginDisabled (status.running && !live);
    ImGui::TextWrapped (
        origin == 0 ? "Analysis receives the gradient. Context occludes. Focus is checked for visibility from Analysis."
                    : "Focus receives the gradient. Context occludes. Visibility is checked from a placed view point.");
    // Context is the viewpoint's, shared by all three view-analysis tabs; Analysis
    // and Focus are shared by the two visibility tabs.
    auto shared = *viewpointstudy::GetSettings ();
    const auto sharedContext = shared.context;
    bool contextChanged = DrawSharedContextToggle (shared);
    if (state.visibilityOrigin == 0)
        changed |= RoleRow ("Analysis", state.visibilityFrom, state.visibilityTo, shared.context,
                            state.visibilitySelection, "Receives the gradient; outlined violet.");
    if (!shared.contextAll)
        contextChanged |=
            RoleRow ("Context", shared.context, state.visibilityFrom, state.visibilityTo, state.visibilitySelection,
                     "Occludes. Empty: no blockers; unassigned objects are ignored.");
    changed |= RoleRow ("Focus", state.visibilityTo, state.visibilityFrom, shared.context, state.visibilitySelection,
                        origin == 0 ? "Checked for visibility from Analysis; outlined magenta."
                                    : "Receives the gradient; outlined magenta.");
    if (contextChanged || shared.context != sharedContext) {
        ConfigureViewpoint (state, shared);
        changed = true;
    }
    if (live)
        changed |= DrawPointControls (state, shared, capture.get ());

    changed |= ImGui::SliderFloat ("square cells", &state.visibilityGrid, 0.25f, 10.0f, "%.2f m",
                                   ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip ("Cells over coplanar surface patches.");
    if (state.visibilityOrigin == 0)
        changed |= ImGui::SliderInt ("Focus aim points", &state.visibilityAimPoints, 1, 128);
    ImGui::EndDisabled ();
    // A surface result no longer describes its settings once they change. A
    // point result is replaced by the next live run instead, so it stays up.
    if (changed) {
        if (ownResult && !live)
            visibilitystudy::ClearDisplay ();
        state.visibilitySubmitError.clear ();
    }

    if (capture == nullptr)
        ImGui::TextDisabled ("Waiting for a complete, current viewer capture.");
    if (live) {
        // ⚠️ NO RUN BUTTON: every change re-runs, coalesced -- a run still going is
        // cancelled first, and the newest inputs are submitted once it stops.
        const bool ready = capture != nullptr && !state.visibilityTo.empty () && shared.pointValid;
        if (ready) {
            auto request = MakeRequest (state, shared, capture);
            const std::string key = LiveKey (request, shared);
            if (key != state.visibilityLiveKey) {
                if (status.running)
                    visibilitystudy::Cancel ();
                else {
                    state.visibilitySubmitError.clear ();
                    if (visibilitystudy::Submit (std::move (request), state.visibilitySubmitError))
                        state.visibilityLiveKey = key;
                }
            }
        }
        else if (state.visibilityTo.empty ())
            ImGui::TextDisabled ("Assign Focus to see what the point sees.");
    }
    else {
        const bool ready =
            capture != nullptr && !status.running && !state.visibilityTo.empty () && !state.visibilityFrom.empty ();
        ImGui::BeginDisabled (!ready);
        if (ImGui::Button ("Run visibility", ImVec2 (-1.0f, 0.0f))) {
            state.viewpointPlacing = false;
            state.visibilitySubmitError.clear ();
            visibilitystudy::Submit (MakeRequest (state, shared, capture), state.visibilitySubmitError);
        }
        ImGui::EndDisabled ();
    }
    // A live run superseded by the next one is cancelled on purpose: not a failure.
    const bool superseded = live && status.error == "visibility study cancelled";
    if (status.running) {
        ImGui::TextUnformatted (live ? "Updating visibility..." : "Calculating visibility...");
        if (!live && ImGui::SmallButton ("Cancel calculation"))
            visibilitystudy::Cancel ();
    }
    else if (!state.visibilitySubmitError.empty ())
        hudshell::ErrorNotice ("Visibility failed: " + state.visibilitySubmitError);
    else if (!changed && !status.error.empty () && status.origin == origin && !superseded)
        hudshell::ErrorNotice ("Visibility failed: " + status.error);
    else if (!changed && status.complete && ownResult && scene.sunStudy.studyId == status.studyId) {
        ImGui::Text ("%.1f%% mean visible", status.meanVisibility * 100.0);
        ImGui::SetItemTooltip ("%u cells, %u aim point(s), %.2f M rays, %.0f ms", unsigned (status.sampleCount),
                               unsigned (status.aimPointCount), double (status.rayCount) / 1.0e6,
                               status.analysisMilliseconds);
    }

    // Both visibility tabs share the gradient; the tint redraws with it at once.
    ColourPicker ("not visible", state.visibilityLowRgba, false);
    ImGui::SameLine ();
    ColourPicker ("visible", state.visibilityHighRgba, false);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Reset##visibility-colours")) {
        const HudState defaults;
        state.visibilityLowRgba = defaults.visibilityLowRgba;
        state.visibilityHighRgba = defaults.visibilityHighRgba;
    }
    if (ownResult) {
        if (ImGui::SmallButton ("Clear visibility")) {
            visibilitystudy::ClearDisplay ();
            state.visibilityLiveKey.clear ();
        }
        ImGui::SetItemTooltip ("%s, %u tinted element(s)", scene.sunStudy.studyId.c_str (),
                               unsigned (scene.sunStudy.elementsAttached));
    }
}

void ServiceAnalysisInteractions (HudState& state, const DiligentScene& scene, const InputSnapshot& input,
                                  const float origin[3], const float direction[3])
{
    ServiceSunStudyInspector (state, scene, origin, direction);
    ServiceSunStudyCentrePick (state, input, origin, direction);
}

void DrawVisibilityConeOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const auto shared = viewpointstudy::GetSettings ();
    if (state.analysisFocus.tab != analysistabs::kPointVisibilityTab || !shared->pointValid || viewProj == nullptr ||
        width == 0 || height == 0)
        return;
    const double* point = shared->options.point.data ();
    ImVec2 apex;
    if (!Project (point, viewProj, width, height, apex))
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList ();
    const ImU32 colour = IM_COL32 (75, 131, 88, 230);
    draw->AddCircleFilled (apex, 4.0f, colour);
    constexpr size_t kSegments = 48;
    constexpr double kPi = 3.14159265358979323846;
    const auto ring = [&] (const double centre[3], const double u[3], const double v[3], double radius, ImU32 ink,
                           float thickness) {
        ImVec2 previous;
        bool had = false;
        for (size_t i = 0; i <= kSegments; ++i) {
            const double angle = 2.0 * kPi * double (i) / double (kSegments);
            double at[3];
            for (int axis = 0; axis < 3; ++axis)
                at[axis] = centre[axis] + radius * (u[axis] * std::cos (angle) + v[axis] * std::sin (angle));
            ImVec2 pixel;
            const bool has = Project (at, viewProj, width, height, pixel);
            if (has && had)
                draw->AddLine (previous, pixel, ink, thickness);
            previous = pixel;
            had = has;
        }
    };
    if (state.visibilityView == 1) {
        // The sphere: the horizon, and the band's sky and ground limits, 1 m out.
        const double x[3] = { 1.0, 0.0, 0.0 }, y[3] = { 0.0, 1.0, 0.0 };
        ring (point, x, y, 1.0, IM_COL32 (75, 131, 88, 140), 1.0f);
        for (const float degrees : { state.visibilitySkyDegrees, state.visibilityGroundDegrees }) {
            if (std::abs (degrees) >= 89.5f)
                continue; // the pole: nothing cut off
            const double e = double (degrees) * kPi / 180.0;
            const double centre[3] = { point[0], point[1], point[2] + std::sin (e) };
            ring (centre, x, y, std::cos (e), colour, 2.0f);
        }
        return;
    }
    double direction[3];
    ConeDirection (state, point, direction);
    // The line to the target the cone keeps while the point moves.
    ImVec2 target;
    if (state.visibilityTargetSet && Project (state.visibilityTarget, viewProj, width, height, target))
        draw->AddLine (apex, target, IM_COL32 (75, 131, 88, 170), 1.5f);
    const bool nearVertical = std::abs (direction[2]) > 0.9;
    const double reference[3] = { 0.0, nearVertical ? 1.0 : 0.0, nearVertical ? 0.0 : 1.0 };
    double u[3], v[3];
    Cross (direction, reference, u);
    Normalize (u);
    Cross (direction, u, v);
    Normalize (v);
    const double halfAngle = double (state.visibilityConeDegrees) * 0.5 * kPi / 180.0;
    const double tangent = (std::max) (std::tan (halfAngle), 1.0e-4);
    const double radius = (std::min) (1.0, 2.0 * tangent);
    const double length = radius / tangent;
    const double centre[3] = { point[0] + direction[0] * length, point[1] + direction[1] * length,
                               point[2] + direction[2] * length };
    ring (centre, u, v, radius, colour, 2.0f);
    for (int i = 0; i < 4; ++i) {
        const double angle = 0.5 * kPi * double (i);
        double at[3];
        for (int axis = 0; axis < 3; ++axis)
            at[axis] = centre[axis] + radius * (u[axis] * std::cos (angle) + v[axis] * std::sin (angle));
        ImVec2 pixel;
        if (Project (at, viewProj, width, height, pixel))
            draw->AddLine (apex, pixel, colour, 1.5f);
    }
}

} // namespace geomsrv::archviz::viewerhud
