#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/AnalysisLiveRerun.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/ViewpointOverlayGeometry.hpp"
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

void Normalize (double value[3])
{
    const double length = std::sqrt (value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length <= 1.0e-12)
        return;
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

// The centre of the receivers' bounding box (point visibility: Analysis), where
// an untouched cone target sits.
bool RoleCentre (const Snapshot& snapshot, const std::vector<std::string>& focus, double centre[3])
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
    // ⚠️ POINT VISIBILITY'S RECEIVERS ARE ANALYSIS, NOT FOCUS. On both tabs
    // Analysis is what takes the gradient; the kernel's point mode calls its
    // receivers TO, so Analysis goes there and there is no FROM.
    const bool point = state.visibilityOrigin != 0;
    request.fromElements = point ? std::vector<std::string> {} : state.visibilityFrom;
    request.toElements = point ? state.visibilityFrom : state.visibilityTo;
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

// Everything a run depends on, the capture included: a different key is a
// different result. Point and surface runs both follow it.
std::string LiveKey (const visibilitystudy::Request& request, const viewpointstudy::Settings& shared)
{
    auto o = request.options;
    if (o.origin == evp::sunstudy::VisibilityOrigin::Surfaces) {
        // The shared point and its view do not enter a surface run: moving the
        // viewpoint on another tab must not re-run it.
        o = {};
        o.origin = request.options.origin;
        o.spacing = request.options.spacing;
        o.maxAimPoints = request.options.maxAimPoints;
    }
    else
        o.maxAimPoints = 0; // the surface tab's slider; a point sees from itself
    char numbers[360];
    std::snprintf (numbers, sizeof (numbers), "%llu|%d|%.6f,%.6f,%.6f|%.6f,%.6f,%.6f|%d|%.3f|%.3f,%.3f|%.4f|%u|%d",
                   static_cast<unsigned long long> (request.snapshot != nullptr ? request.snapshot->id : 0),
                   int (o.origin), o.point[0], o.point[1], o.point[2], o.direction[0], o.direction[1], o.direction[2],
                   int (o.view), o.coneDegrees, o.minElevationDegrees, o.maxElevationDegrees, o.spacing,
                   unsigned (o.maxAimPoints), shared.contextAll ? 1 : 0);
    std::string key = numbers;
    for (const auto& guid : request.fromElements)
        key += "|" + guid;
    key += "#";
    for (const auto& guid : request.toElements)
        key += "|" + guid;
    key += "#";
    for (const auto& guid : shared.context)
        key += "|" + guid;
    return key;
}

// How long surface visibility's inputs must stay the same before it re-runs. A
// surface run is far heavier than a point run, so a drag or a burst of edits
// runs it once, at the end, rather than cancelling a run every frame.
constexpr double kSurfaceQuietSeconds = 0.35;

// Point visibility's own controls: the view, the point, and the cone's target or
// the sphere's band. Returns true when the result they describe changed.
bool DrawPointControls (HudState& state, viewpointstudy::Settings& shared, const Snapshot* capture)
{
    bool changed = false;
    // The Viewpoint tab's point: placed, typed, dragged or set on a path in either tab.
    ImGui::SeparatorText ("Point");
    if (ImGui::Button (state.viewpointPlacing ? "Click a model surface..." : "Place view point")) {
        state.viewpointPlacing = true;
        state.viewpointPlacingTarget = false;
        state.viewpointPlacementMiss = false;
        state.viewpointDragging = false;
    }
    DrawAnalysisPathButtons (state);
    if (ImGui::InputScalarN ("Point XYZ (m)", ImGuiDataType_Double, shared.options.point.data (), 3, nullptr, nullptr,
                             "%.2f")) {
        shared.pointValid = true;
        ConfigureViewpoint (state, shared);
    }
    ImGui::SetItemTooltip ("Or drag the gumball; the Viewpoint tab shares this point.");
    if (state.viewpointPlacementMiss)
        ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "Point missed the model; click a surface.");
    if (DrawAnalysisPathControls (state, shared, false))
        ConfigureViewpoint (state, shared);
    ImGui::SeparatorText ("View");
    const char* const views[] = { "View cone", "View sphere" };
    changed |= Choice ("##visibility-view", state.visibilityView, views, 2);
    ImGui::SetItemTooltip ("Cone: what the point sees toward its target. Sphere: all round, within a band of "
                           "elevations.");
    if (state.visibilityView == 0) {
        // Until the user moves it, the target follows Analysis's centre.
        if (!state.visibilityTargetEdited && capture != nullptr) {
            std::string key = std::to_string (capture->id);
            for (const auto& guid : state.visibilityFrom)
                key += "|" + guid;
            if (key != state.visibilityTargetFollowKey) {
                state.visibilityTargetFollowKey = key;
                double centre[3];
                if (RoleCentre (*capture, state.visibilityFrom, centre)) {
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
        ImGui::SetItemTooltip ("Where the cone looks: Analysis's centre until you move it. The cone keeps its target "
                               "when the point moves; drag the target's gumball too.");
        if (ImGui::SmallButton (state.viewpointPlacingTarget ? "Click a model surface...##target" : "Place target")) {
            state.viewpointPlacingTarget = true;
            state.viewpointPlacing = false;
            state.viewpointDragging = false;
        }
        ImGui::SetItemTooltip ("Click the model where the cone should look.");
        ImGui::SameLine ();
        ImGui::BeginDisabled (!state.visibilityTargetEdited);
        if (ImGui::SmallButton ("Analysis centre")) {
            state.visibilityTargetEdited = false;
            state.visibilityTargetFollowKey.clear ();
        }
        ImGui::SetItemTooltip ("Aim at the centre of Analysis's bounding box again, and follow it.");
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
    // ⚠️ SURFACE VISIBILITY FOLLOWS TOO, ONCE IT HAS RUN. A model edit or a changed
    // setting re-runs it and the last result stays up until the new one replaces
    // it, so a minor change never asks for another Run. "Live update" off restores
    // the old contract: a change clears the result.
    auto& follow = state.visibilitySurfaceFollow;
    const bool following = !live && follow.armed && state.visibilityLiveUpdate;
    bool changed = false;
    // A run that follows its controls keeps them usable while it runs.
    ImGui::BeginDisabled (status.running && !live && !following);
    ImGui::TextWrapped (
        origin == 0
            ? "Analysis receives the gradient. Context occludes. Focus is checked for visibility from Analysis."
            : "Analysis receives the gradient. Context occludes. Visibility is checked from a placed view point.");
    // Context is the viewpoint's, shared by all three view-analysis tabs; Analysis
    // is shared by the two visibility tabs, and receives on both. Focus is the
    // surface tab's alone: a point sees from itself.
    auto shared = *viewpointstudy::GetSettings ();
    const auto sharedContext = shared.context;
    ImGui::SeparatorText ("Roles");
    bool contextChanged = DrawSharedContextToggle (shared);
    changed |= RoleRow ("Analysis", state.visibilityFrom, state.visibilityTo, shared.context, state.visibilitySelection,
                        "Receives the gradient; outlined violet.");
    if (!shared.contextAll)
        contextChanged |=
            RoleRow ("Context", shared.context, state.visibilityFrom, state.visibilityTo, state.visibilitySelection,
                     "Occludes. Empty: no blockers; unassigned objects are ignored.");
    if (origin == 0)
        changed |= RoleRow ("Focus", state.visibilityTo, state.visibilityFrom, shared.context,
                            state.visibilitySelection, "Checked for visibility from Analysis; outlined magenta.");
    if (contextChanged || shared.context != sharedContext) {
        ConfigureViewpoint (state, shared);
        changed = true;
    }
    if (live)
        changed |= DrawPointControls (state, shared, capture.get ());

    ImGui::SeparatorText ("Sampling");
    changed |= ImGui::SliderFloat ("square cells", &state.visibilityGrid, 0.25f, 10.0f, "%.2f m",
                                   ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip ("Cells over coplanar surface patches.");
    if (state.visibilityOrigin == 0)
        changed |= ImGui::SliderInt ("Focus aim points", &state.visibilityAimPoints, 1, 128);
    ImGui::EndDisabled ();
    // A result that is not followed no longer describes its settings once they
    // change. A followed one is replaced by the next run instead, so it stays up.
    if (changed) {
        if (ownResult && !live && !following)
            visibilitystudy::ClearDisplay ();
        state.visibilitySubmitError.clear ();
    }

    ImGui::SeparatorText (live ? "Result" : "Run");
    if (capture == nullptr)
        ImGui::TextDisabled (following || live ? "Model changed: updates once the viewer has read it again."
                                               : "Waiting for a complete, current viewer capture.");
    if (live) {
        // ⚠️ NO RUN BUTTON: every change re-runs, coalesced -- a run still going is
        // cancelled first, and the newest inputs are submitted once it stops.
        const bool ready = capture != nullptr && !state.visibilityFrom.empty () && shared.pointValid;
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
        else if (state.visibilityFrom.empty ())
            ImGui::TextDisabled ("Assign Analysis to see what the point sees.");
    }
    else {
        const bool inputs = capture != nullptr && !state.visibilityTo.empty () && !state.visibilityFrom.empty ();
        const std::string key = inputs ? LiveKey (MakeRequest (state, shared, capture), shared) : std::string ();
        if (following) {
            switch (liverun::Decide (follow, key, status.running, ImGui::GetTime (), kSurfaceQuietSeconds)) {
                case liverun::Step::Cancel:
                    visibilitystudy::Cancel ();
                    break;
                case liverun::Step::Submit:
                    state.visibilitySubmitError.clear ();
                    if (visibilitystudy::Submit (MakeRequest (state, shared, capture), state.visibilitySubmitError))
                        liverun::Submitted (follow, key);
                    break;
                case liverun::Step::Idle:
                case liverun::Step::Wait:
                    break;
            }
        }
        ImGui::BeginDisabled (!inputs || status.running);
        if (ImGui::Button (following ? "Run again" : "Run visibility", ImVec2 (-1.0f, 0.0f))) {
            state.viewpointPlacing = false;
            state.visibilitySubmitError.clear ();
            if (visibilitystudy::Submit (MakeRequest (state, shared, capture), state.visibilitySubmitError))
                liverun::Submitted (follow, key); // arms the follow
        }
        ImGui::EndDisabled ();
        ImGui::Checkbox ("Live update", &state.visibilityLiveUpdate);
        ImGui::SetItemTooltip ("After a run, edits in Archicad and changed settings re-run it; the last result stays "
                               "up until the new one replaces it. Off: a change clears the result.");
        if (!following && ownResult && capture != nullptr && status.snapshotId != 0 && status.snapshotId != capture->id)
            ImGui::TextDisabled ("The model changed since this run.");
    }
    // A run superseded by the next one, or by a model edit, is not a failure when
    // the tab follows its inputs: the next run replaces it.
    const bool superseded = (live || following) && status.superseded;
    if (status.running) {
        ImGui::TextUnformatted (live || following ? "Updating visibility..." : "Calculating visibility...");
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
    ImGui::SeparatorText ("Display");
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
            if (!live)
                liverun::Disarm (follow); // nothing brings a cleared result back
        }
        ImGui::SetItemTooltip ("%s, %u tinted element(s)", scene.sunStudy.studyId.c_str (),
                               unsigned (scene.sunStudy.elementsAttached));
    }
}

bool PointVisibilityGuideOf (const HudState& state, PointVisibilityGuide& guide)
{
    const auto shared = viewpointstudy::GetSettings ();
    if (state.readOnly || state.analysisFocus.tab != analysistabs::kPointVisibilityTab || !shared->pointValid)
        return false;
    guide = {};
    std::copy (shared->options.point.begin (), shared->options.point.end (), guide.point);
    guide.sphere = state.visibilityView == 1;
    guide.minElevationDegrees = state.visibilityGroundDegrees;
    guide.maxElevationDegrees = state.visibilitySkyDegrees;
    guide.coneDegrees = state.visibilityConeDegrees;
    ConeDirection (state, guide.point, guide.direction);
    guide.hasTarget = state.visibilityTargetSet;
    std::copy (state.visibilityTarget, state.visibilityTarget + 3, guide.target);
    return true;
}

} // namespace geomsrv::archviz::viewerhud
