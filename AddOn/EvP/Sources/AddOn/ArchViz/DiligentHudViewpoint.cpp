#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/HudGumball.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/PointGumball.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "Geometry/QueryEngine.hpp"
#include "Geometry/SurfaceCriteria.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace geomsrv::archviz::viewerhud {
namespace {

// "Wall {1A2B3C4D}", or "Element {...}" for a type the criteria table does not name.
std::string ElementLabel (const Snapshot* snapshot, const std::string& guid)
{
    std::string type = "Element";
    if (snapshot != nullptr) {
        const std::string key = evp::sunstudy::CanonicalGuid (guid);
        for (const auto& mesh : snapshot->meshes)
            if (evp::sunstudy::CanonicalGuid (mesh.guid) == key) {
                const std::string name = surfacecriteria::TypeName (mesh.elemType);
                if (name.rfind ("Modeler type", 0) != 0)
                    type = name;
                break;
            }
    }
    const std::string key = evp::sunstudy::CanonicalGuid (guid);
    return type + " {" + key.substr (0, 8) + "}";
}

// Context rows: each element with the rays it stopped in the current result.
// With the whole scene as Context there is no list to edit, only what blocks.
void DrawContextList (viewpointstudy::Settings& settings, const viewpointstudy::Status& status, bool& changed)
{
    const auto snapshot = visibilitystudy::ViewerCapture ();
    std::vector<evp::sunstudy::ViewpointBlocker> blockers;
    if (status.result != nullptr && snapshot != nullptr)
        blockers = evp::sunstudy::ViewpointBlockers (*status.result, *snapshot);
    if (settings.contextAll) {
        constexpr size_t kShownBlockers = 8;
        if (!ImGui::TreeNodeEx ("##viewpoint-blockers", ImGuiTreeNodeFlags_DefaultOpen, "Blocking: %u object(s)",
                                unsigned (blockers.size ())))
            return;
        for (size_t i = 0; i < blockers.size () && i < kShownBlockers; ++i)
            ImGui::TextColored (ImVec4 (0.93f, 0.42f, 0.30f, 1.0f), "%s  %u ray(s), nearest %.2f m",
                                ElementLabel (snapshot.get (), blockers[i].guid).c_str (), unsigned (blockers[i].rays),
                                blockers[i].nearest);
        if (blockers.size () > kShownBlockers)
            ImGui::TextDisabled ("and %u more", unsigned (blockers.size () - kShownBlockers));
        ImGui::TreePop ();
        return;
    }
    const bool open = ImGui::TreeNodeEx ("##viewpoint-context", ImGuiTreeNodeFlags_DefaultOpen, "Context: %u object(s)",
                                         unsigned (settings.context.size ()));
    if (settings.context.empty ())
        ImGui::SetItemTooltip ("None: every ray reaches the radius.");
    if (!open)
        return;
    size_t remove = settings.context.size ();
    for (size_t i = 0; i < settings.context.size (); ++i) {
        ImGui::PushID (int (i));
        if (ImGui::SmallButton ("x"))
            remove = i;
        ImGui::SameLine ();
        const std::string key = evp::sunstudy::CanonicalGuid (settings.context[i]);
        const auto blocker = std::find_if (blockers.begin (), blockers.end (), [&] (const auto& b) {
            return evp::sunstudy::CanonicalGuid (b.guid) == key;
        });
        const std::string label = ElementLabel (snapshot.get (), settings.context[i]);
        if (blocker != blockers.end ())
            ImGui::TextColored (ImVec4 (0.93f, 0.42f, 0.30f, 1.0f), "%s  %u ray(s), nearest %.2f m", label.c_str (),
                                unsigned (blocker->rays), blocker->nearest);
        else
            ImGui::TextDisabled ("%s  not blocking", label.c_str ());
        ImGui::PopID ();
    }
    if (remove < settings.context.size ()) {
        settings.context.erase (settings.context.begin () + std::ptrdiff_t (remove));
        changed = true;
    }
    ImGui::TreePop ();
}

// The point's gumball: on the Viewpoint tab while it is shown, and always on
// Point visibility, whose point it also is.
bool ShowsPointGumball (const HudState& state, const viewpointstudy::Settings& settings)
{
    return (state.analysisFocus.tab == analysistabs::kViewpointTab && settings.enabled) ||
           state.analysisFocus.tab == analysistabs::kPointVisibilityTab;
}

// The cursor and a tooltip say a handle can be dragged, before the press.
void DrawGumballHint (const HudState& state)
{
    if (state.viewpointHotPart < 0)
        return;
    hudshell::OwnCursor ();
    ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
    if (state.viewpointDragging)
        return;
    static const char* const kAxes[3] = { "X", "Y", "Z" };
    const char* what = state.viewpointHotHandle == 1 ? "the cone's target" : "the point";
    if (state.viewpointHotPart == 3)
        ImGui::SetTooltip ("Drag to move %s in plan", what);
    else
        ImGui::SetTooltip ("Drag to move %s along %s", what, kAxes[state.viewpointHotPart]);
}

bool LeftTransition (const InputSnapshot& input, bool down)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && input.transitions[i].down == down)
            return true;
    return false;
}

} // namespace

bool ShowsTargetGumball (const HudState& state)
{
    return !state.readOnly && state.analysisFocus.tab == analysistabs::kPointVisibilityTab &&
           state.visibilityView == 0 && state.visibilityTargetSet;
}

void ConfigureViewpoint (HudState& state, viewpointstudy::Settings settings)
{
    // Record the revision as seen: a HUD edit from another Analysis tab (shared
    // Context, the shared point) must not read as a command and take the focus.
    viewpointstudy::Configure (std::move (settings));
    state.viewpointSeenRevision = viewpointstudy::GetSettings ()->revision;
}

bool DrawSharedContextToggle (viewpointstudy::Settings& settings)
{
    const bool changed = ImGui::Checkbox ("Whole scene as Context", &settings.contextAll);
    ImGui::SetItemTooltip ("Every element occludes, Analysis and Focus included. Shared by Surface visibility, "
                           "Point visibility and Viewpoint.");
    return changed;
}

void DrawViewpointHudSection (HudState& state)
{
    auto settings = *viewpointstudy::GetSettings ();
    bool changed = false;
    ImGui::TextWrapped ("What the point sees out to the radius. Context stops rays; empty Context sees all of it.");
    changed |= ImGui::Checkbox ("Show viewpoint", &settings.enabled);
    const auto status = viewpointstudy::GetStatus ();
    changed |= DrawSharedContextToggle (settings);
    if (ImGui::Button (state.viewpointPickingContext ? "Done picking" : "Pick Context in viewer")) {
        state.viewpointPickingContext = !state.viewpointPickingContext;
        state.viewpointPlacing = false;
        if (state.viewpointPickingContext && (!settings.enabled || settings.contextAll)) {
            // Picking edits the explicit list; the whole scene has none.
            settings.enabled = true;
            settings.contextAll = false;
            changed = true;
        }
    }
    ImGui::SetItemTooltip ("Click elements in the viewer to add or remove them; the viewer selection is not used.");
    DrawContextList (settings, status, changed);
    if (!settings.contextAll) {
        const auto fromSelection = [&state] {
            ImGui::SetItemTooltip ("From the viewer selection: %u element(s)",
                                   unsigned (state.visibilitySelection.size ()));
        };
        ImGui::BeginDisabled (state.visibilitySelection.empty ());
        if (ImGui::SmallButton ("Replace Context")) {
            settings.context = state.visibilitySelection;
            changed = true;
        }
        fromSelection ();
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Add")) {
            for (const auto& guid : state.visibilitySelection)
                if (std::find (settings.context.begin (), settings.context.end (), guid) == settings.context.end ())
                    settings.context.push_back (guid);
            changed = true;
        }
        fromSelection ();
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Remove")) {
            for (const auto& guid : state.visibilitySelection)
                settings.context.erase (std::remove (settings.context.begin (), settings.context.end (), guid),
                                        settings.context.end ());
            changed = true;
        }
        fromSelection ();
        ImGui::EndDisabled ();
        if (ImGui::SmallButton ("Clear Context")) {
            settings.context.clear ();
            changed = true;
        }
    }
    changed |= ImGui::InputScalarN ("Point XYZ (m)", ImGuiDataType_Double, settings.options.point.data (), 3, nullptr,
                                    nullptr, "%.2f");
    ImGui::SetItemTooltip ("Or drag the gumball: X/Y/Z arrows along an axis, the centre disc in plan.");
    if (changed)
        settings.pointValid = true;
    if (ImGui::Button (state.viewpointPlacing ? "Click a model surface..." : "Place viewpoint")) {
        state.viewpointPlacing = true;
        state.viewpointPickingContext = false;
        state.viewpointDragging = false;
        settings.enabled = true;
        changed = true;
    }
    auto& options = settings.options;
    int mode = int (options.mode);
    const char* const modes[] = { "Plan isovist", "Field of view", "Spherical isovist" };
    if (Choice ("##viewpoint-mode", mode, modes, 3)) {
        options.mode = evp::sunstudy::ViewpointMode (mode);
        changed = true;
    }
    ImGui::SetItemTooltip ("Plan isovist: all round, in the horizontal plane at the point's height. Field of view: "
                           "the same within a view angle. Spherical isovist: every direction within a band of "
                           "elevations, as a visible volume.");
    if (options.mode == evp::sunstudy::ViewpointMode::FieldOfView) {
        changed |= ImGui::InputDouble ("bearing (deg)", &options.bearingDegrees, 5.0, 45.0, "%.0f");
        ImGui::SetItemTooltip ("The view's centre, counterclockwise from +X (east).");
        if (state.visibilityTargetSet) {
            ImGui::SameLine ();
            if (ImGui::SmallButton ("Aim at target")) {
                options.bearingDegrees = std::atan2 (state.visibilityTarget[1] - options.point[1],
                                                     state.visibilityTarget[0] - options.point[0]) *
                                         180.0 / 3.14159265358979323846;
                changed = true;
            }
            ImGui::SetItemTooltip ("Face Point visibility's cone target.");
        }
        float angle = float (options.fieldOfViewDegrees);
        if (ImGui::SliderFloat ("view angle", &angle, 1.0f, 359.0f, "%.0f deg")) {
            options.fieldOfViewDegrees = double (angle);
            changed = true;
        }
        ImGui::SetItemTooltip ("The field of view's full horizontal angle.");
    }
    else if (options.mode == evp::sunstudy::ViewpointMode::Sphere) {
        float sky = float (options.maxElevationDegrees), ground = float (options.minElevationDegrees);
        if (ImGui::SliderFloat ("sky up to##viewpoint", &sky, -89.0f, 90.0f, "%.0f deg")) {
            options.maxElevationDegrees = double (sky);
            options.minElevationDegrees = (std::min) (options.minElevationDegrees, double (sky) - 1.0);
            changed = true;
        }
        ImGui::SetItemTooltip ("The highest elevation seen: 90 is the whole sky, 0 none of it.");
        if (ImGui::SliderFloat ("ground down to##viewpoint", &ground, -90.0f, 89.0f, "%.0f deg")) {
            options.minElevationDegrees = double (ground);
            options.maxElevationDegrees = (std::max) (options.maxElevationDegrees, double (ground) + 1.0);
            changed = true;
        }
        ImGui::SetItemTooltip ("The lowest elevation seen: -90 straight down, 0 nothing below the horizon.");
    }
    changed |= AdaptiveSlider ("Radius (m)", settings.options.radius, state.viewpointRadiusRange,
                               state.viewpointRadiusHeld, 0.1, 10000.0, "%.1f");
    ImGui::SetItemTooltip ("The range rescales on release; ctrl+click to type a value.");
    if (changed)
        ConfigureViewpoint (state, std::move (settings));
    ColourPicker ("Fill", state.viewpointFillRgba, true);
    ImGui::SameLine ();
    ColourPicker ("Open edge", state.viewpointEdgeRgba, true);
    ImGui::SameLine ();
    ColourPicker ("Blocked edge", state.viewpointBlockedRgba, true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Reset##viewpoint-colours")) {
        const HudState defaults;
        state.viewpointFillRgba = defaults.viewpointFillRgba;
        state.viewpointEdgeRgba = defaults.viewpointEdgeRgba;
        state.viewpointBlockedRgba = defaults.viewpointBlockedRgba;
    }
    if (status.calculating)
        ImGui::TextDisabled ("Updating visibility...");
    else if (!status.error.empty ())
        hudshell::ErrorNotice (status.error);
    else if (status.result != nullptr && status.result->options.mode == evp::sunstudy::ViewpointMode::Sphere)
        ImGui::Text ("%.0f m3 visible; %.0f%% of directions open", status.result->volume,
                     status.result->openFraction * 100.0);
    else if (status.result != nullptr)
        ImGui::Text ("%.1f m2 visible; %u of %u rays hit Context", status.result->area,
                     unsigned (status.result->collisionCount), unsigned (status.result->perimeter.size ()));
    ImGui::SetItemTooltip ("Edge: the blocked colour where Context stops a ray, the open colour at the radius.");
}

void ServiceViewpointInteractions (HudState& state, const InputSnapshot& input, const float viewProj[16],
                                   uint32_t width, uint32_t height, const float rayOrigin[3],
                                   const float rayDirection[3])
{
    const auto shared = viewpointstudy::GetSettings ();
    if (shared->revision != state.viewpointSeenRevision) {
        // Command changes discover the page once; our own gumball updates record
        // their revision below and never steal another analysis tab each frame.
        state.viewpointSeenRevision = shared->revision;
        state.viewpointDragging = false;
        if (shared->enabled)
            analysistabs::Select (state.analysisFocus, analysistabs::kViewpointTab);
    }
    state.viewpointOwnsMouse = false;
    state.viewpointHotPart = -1;
    const bool point = !state.readOnly && shared->pointValid && ShowsPointGumball (state, *shared);
    const bool target = ShowsTargetGumball (state);
    if (state.viewpointPlacing || (!point && !target)) {
        state.viewpointDragging = false;
        viewpointstudy::Tick (visibilitystudy::ViewerCapture ());
        return;
    }
    // Two handles: the shared point, and point visibility's cone target. The
    // point wins where they overlap -- it is the one the target is seen from.
    const float x = float (CursorTargetX (input, width)), y = float (CursorTargetY (input, height));
    const int pointHit = point && input.inside
                             ? HitGumball (MakeGumball (shared->options.point.data (), viewProj, width, height), x, y)
                             : -1;
    const int targetHit = target && input.inside && pointHit < 0
                              ? HitGumball (MakeGumball (state.visibilityTarget, viewProj, width, height), x, y)
                              : -1;
    const int hit = pointHit >= 0 ? pointHit : targetHit;
    const int handle = pointHit >= 0 ? 0 : 1;
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    if (!state.viewpointDragging && !state.wantsMouse && hit >= 0 && LeftTransition (input, true)) {
        state.viewpointDragAxis = hit;
        state.viewpointDragHandle = handle;
        if (handle == 0)
            std::copy (shared->options.point.begin (), shared->options.point.end (), state.viewpointDragPoint);
        else
            std::copy (state.visibilityTarget, state.visibilityTarget + 3, state.viewpointDragPoint);
        state.viewpointDragging =
            PointGumballParameter (state.viewpointDragPoint, hit, origin, direction, state.viewpointDragParameter);
    }
    state.viewpointOwnsMouse =
        state.viewpointDragging ||
        (!state.wantsMouse && hit >= 0 && (LeftTransition (input, true) || (input.buttons & kMouseLeft)));
    if (state.viewpointDragging) {
        double parameter[3];
        if (state.viewpointDragHandle == 1 &&
            PointGumballParameter (state.viewpointDragPoint, state.viewpointDragAxis, origin, direction, parameter)) {
            for (int axis = 0; axis < 3; ++axis)
                state.visibilityTarget[axis] =
                    state.viewpointDragPoint[axis] + parameter[axis] - state.viewpointDragParameter[axis];
            state.visibilityTargetEdited = true; // moved by hand: it no longer follows Focus
        }
        else if (state.viewpointDragHandle == 0 &&
                 PointGumballParameter (state.viewpointDragPoint, state.viewpointDragAxis, origin, direction,
                                        parameter)) {
            auto settings = *shared;
            for (int axis = 0; axis < 3; ++axis)
                settings.options.point[axis] =
                    state.viewpointDragPoint[axis] + parameter[axis] - state.viewpointDragParameter[axis];
            if (settings.options.point != shared->options.point) {
                viewpointstudy::Configure (std::move (settings));
                state.viewpointSeenRevision = viewpointstudy::GetSettings ()->revision;
            }
        }
        if (LeftTransition (input, false) || !(input.buttons & kMouseLeft))
            state.viewpointDragging = false;
    }
    state.viewpointHotPart = state.viewpointDragging ? state.viewpointDragAxis : (state.wantsMouse ? -1 : hit);
    state.viewpointHotHandle = state.viewpointDragging ? state.viewpointDragHandle : handle;
    viewpointstudy::Tick (visibilitystudy::ViewerCapture ());
}

void PlaceViewpointFromPick (HudState& state, const DiligentScene&, const InputSnapshot& input,
                             const float rayOrigin[3], const float rayDirection[3])
{
    if (state.viewpointPickingContext && !state.wantsMouse && input.inside && LeftTransition (input, false) &&
        state.hover.valid) {
        auto settings = *viewpointstudy::GetSettings ();
        evp::sunstudy::ToggleViewpointContext (settings.context, state.hover.guid);
        settings.enabled = true;
        settings.contextAll = false;
        ConfigureViewpoint (state, std::move (settings));
        return;
    }
    if (!state.viewpointPlacing || state.wantsMouse || !input.inside || !LeftTransition (input, false))
        return;
    state.viewpointPlacementMiss = true;
    const auto snapshot = visibilitystudy::ViewerCapture ();
    if (snapshot == nullptr || !state.hover.valid)
        return;
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    QueryEngine::RayHit hit;
    for (size_t mesh = 0; mesh < snapshot->meshes.size (); ++mesh) {
        if (evp::sunstudy::CanonicalGuid (snapshot->meshes[mesh].guid) ==
            evp::sunstudy::CanonicalGuid (state.hover.guid)) {
            const auto candidate =
                QueryEngine::RaycastMesh (*snapshot, mesh, origin, direction, hit.hit ? hit.t : 1.0e6);
            if (candidate.hit)
                hit = candidate;
        }
    }
    if (!hit.hit)
        return;
    auto settings = *viewpointstudy::GetSettings ();
    settings.pointValid = true;
    for (int axis = 0; axis < 3; ++axis) {
        // Pull the eye 5 cm toward the camera: no self-hit at the surface. Point
        // visibility's cone looks back along the click, which gives it a bearing.
        settings.options.point[axis] = hit.point[axis] - direction[axis] * 0.05;
        state.visibilityDirection[axis] = -direction[axis];
    }
    ConfigureViewpoint (state, std::move (settings));
    state.viewpointPlacing = false;
    state.viewpointPlacementMiss = false;
}

// The isovist itself is drawn occluded by the model (UpdateAndDrawAnalysisOverlay);
// only what must stay on top of it is drawn here.
void DrawViewpointOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const auto settings = viewpointstudy::GetSettings ();
    if (viewProj == nullptr)
        return;
    if (ShowsTargetGumball (state)) {
        const auto aim = MakeGumball (state.visibilityTarget, viewProj, width, height);
        if (aim.valid)
            DrawGumball (aim, ImGui::GetForegroundDrawList (),
                         state.viewpointHotHandle == 1 ? state.viewpointHotPart : -1);
    }
    if (state.readOnly || !settings->pointValid || !ShowsPointGumball (state, *settings))
        return DrawGumballHint (state);
    if (state.viewpointPickingContext && state.hover.valid && !state.wantsMouse) {
        auto context = settings->context;
        const bool adding = evp::sunstudy::ToggleViewpointContext (context, state.hover.guid);
        ImGui::SetTooltip ("Click to %s Context:\n%s", adding ? "add to" : "remove from",
                           ElementLabel (visibilitystudy::ViewerCapture ().get (), state.hover.guid).c_str ());
    }
    const auto gizmo = MakeGumball (settings->options.point.data (), viewProj, width, height);
    if (!gizmo.valid)
        return;
    DrawGumball (gizmo, ImGui::GetForegroundDrawList (), state.viewpointHotHandle == 0 ? state.viewpointHotPart : -1);
    DrawGumballHint (state);
}

} // namespace geomsrv::archviz::viewerhud
