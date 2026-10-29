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
    const char* what = state.viewpointHotHandle == 1 ? "the target" : "the view point";
    if (state.viewpointHotHandle != state.viewpointActiveHandle)
        ImGui::SetTooltip ("Click to select %s; drag to move it in plan", what);
    else if (state.viewpointHotPart == kGumballCentre)
        ImGui::SetTooltip ("Drag to place %s on the surface under the cursor", what);
    else if (state.viewpointHotPart == kGumballPlane)
        ImGui::SetTooltip ("Drag to move %s in plan, keeping its height", what);
    else
        ImGui::SetTooltip ("Drag to move %s along %s", what, kAxes[state.viewpointHotPart]);
}

// A target-facing field of view turns with its target, whichever moved: set
// in the same settings as the move, so a drag configures once per frame.
bool FaceTarget (const HudState& state, viewpointstudy::Settings& settings)
{
    if (!ShowsTargetPoint (state) || state.analysisFocus.tab != analysistabs::kViewpointTab)
        return false;
    const double bearing = std::atan2 (state.visibilityTarget[1] - settings.options.point[1],
                                       state.visibilityTarget[0] - settings.options.point[0]) *
                           180.0 / 3.14159265358979323846;
    if (std::abs (bearing - settings.options.bearingDegrees) <= 1.0e-6)
        return false;
    settings.options.bearingDegrees = bearing;
    return true;
}

// The centre disc's drag: onto the model surface under the cursor. The view
// point stands 5 cm off it toward the camera, as "Place viewpoint" puts it.
bool SnapUnderCursor (const double origin[3], const double direction[3], bool eye, double at[3])
{
    const auto snapshot = visibilitystudy::ViewerCapture ();
    const auto engine = snapshot != nullptr ? QueryIndexCache::Get ().For (snapshot) : nullptr;
    if (engine == nullptr)
        return false;
    const double length =
        std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length <= 1.0e-12)
        return false;
    const auto hit = engine->Raycast (origin, direction, 1.0e6);
    if (!hit.hit)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        at[axis] = hit.point[axis] - (eye ? direction[axis] / length * 0.05 : 0.0);
    return true;
}

// The drag's world move for a gumball part: an arrow's own axis, or the plan for
// the square, the disc (until it finds a surface) and a marker.
int DragAxis (int part)
{
    return part >= kGumballCentre ? 3 : part;
}

bool LeftTransition (const InputSnapshot& input, bool down)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && input.transitions[i].down == down)
            return true;
    return false;
}

} // namespace

bool ShowsTargetPoint (const HudState& state)
{
    if (state.readOnly || !state.visibilityTargetSet)
        return false;
    if (state.analysisFocus.tab == analysistabs::kPointVisibilityTab)
        return state.visibilityView == 0;
    const auto settings = viewpointstudy::GetSettings ();
    return state.analysisFocus.tab == analysistabs::kViewpointTab && settings->enabled &&
           settings->options.mode == evp::sunstudy::ViewpointMode::FieldOfView && state.viewpointFaceTarget;
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
        state.viewpointPlacingTarget = false;
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
                           "elevations; the surfaces it reaches within the radius are coloured.");
    if (options.mode == evp::sunstudy::ViewpointMode::FieldOfView) {
        ImGui::Checkbox ("Face target", &state.viewpointFaceTarget);
        ImGui::SetItemTooltip ("The view turns to face a target point, and keeps facing it as either moves. "
                               "Point visibility's cone shares the target.");
        if (state.viewpointFaceTarget) {
            if (ImGui::SmallButton (state.viewpointPlacingTarget ? "Click a model surface...##target"
                                                                 : "Place target")) {
                state.viewpointPlacingTarget = true;
                state.viewpointPlacing = false;
                state.viewpointPickingContext = false;
                state.viewpointDragging = false;
            }
            ImGui::SetItemTooltip ("Click the model where the view should face.");
            if (ImGui::InputScalarN ("Target XYZ (m)##viewpoint", ImGuiDataType_Double, state.visibilityTarget, 3,
                                     nullptr, nullptr, "%.2f")) {
                state.visibilityTargetSet = true;
                state.visibilityTargetEdited = true;
            }
            ImGui::SetItemTooltip ("Or click its blue marker in the view and drag its gumball.");
            if (!state.visibilityTargetSet)
                ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "Place a target to face.");
            else
                ImGui::Text ("bearing %.0f deg", options.bearingDegrees);
        }
        else {
            changed |= ImGui::InputDouble ("bearing (deg)", &options.bearingDegrees, 5.0, 45.0, "%.0f");
            ImGui::SetItemTooltip ("The view's centre, counterclockwise from +X (east).");
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
        float grid = float (options.surfaceCellMetres);
        if (ImGui::SliderFloat ("surface grid##viewpoint", &grid, 0.0f, 2.0f, grid < 0.05f ? "auto" : "%.2f m")) {
            options.surfaceCellMetres = grid < 0.05f ? 0.0 : double (grid);
            changed = true;
        }
        ImGui::SetItemTooltip ("The cell side surfaces are tested at; patch borders are cut where they cross a cell.\n"
                               "Auto picks one from the radius. Finer finds smaller shadows and is slower; a run\n"
                               "over its budget is coarsened.");
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
        ImGui::Text ("%.0f m2 of surface seen (%.2f m grid%s); %.0f%% of directions open", status.result->seenArea,
                     status.result->surfaceCellMetres,
                     status.result->surfaceCellMetres > status.result->options.surfaceCellMetres + 1e-9 &&
                             status.result->options.surfaceCellMetres > 0.0
                         ? ", coarsened"
                         : "",
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
    const bool target = ShowsTargetPoint (state);
    // Last, after this frame's move: the bearing faces where the point now is.
    const auto tick = [&state] {
        auto settings = *viewpointstudy::GetSettings ();
        if (FaceTarget (state, settings))
            ConfigureViewpoint (state, std::move (settings));
        viewpointstudy::Tick (visibilitystudy::ViewerCapture ());
    };
    // -1 is nothing selected: both handles show as markers, for a clean view.
    if (state.viewpointActiveHandle == 1 && !target)
        state.viewpointActiveHandle = point ? 0 : -1;
    if (state.viewpointActiveHandle == 0 && !point)
        state.viewpointActiveHandle = target ? 1 : -1;
    if (state.viewpointPlacing || state.viewpointPlacingTarget || (!point && !target)) {
        state.viewpointDragging = false;
        tick ();
        return;
    }
    // Two handles, the shared point and the target. Only the selected one has a
    // gumball; the other is a marker, and pressing it selects it and drags it in
    // plan at once, the way the square would. A click on neither deselects both.
    const float x = float (CursorTargetX (input, width)), y = float (CursorTargetY (input, height));
    const bool shown[2] = { point, target };
    const double* const position[2] = { shared->options.point.data (), state.visibilityTarget };
    const int active = state.viewpointActiveHandle;
    int hit = -1, handle = -1;
    if (input.inside && active >= 0 && shown[active]) {
        hit = HitGumball (MakeGumball (position[active], viewProj, width, height, true), x, y);
        handle = hit >= 0 ? active : -1;
    }
    for (int other = 0; other < 2 && input.inside && hit < 0; ++other) {
        if (other == active || !shown[other])
            continue;
        const auto marker = MakeGumball (position[other], viewProj, width, height);
        if (marker.valid && std::hypot (x - marker.center.x, y - marker.center.y) <= kGumballMarkerPickPixels) {
            hit = kGumballPlane;
            handle = other;
        }
    }
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    const bool pressed = LeftTransition (input, true), released = LeftTransition (input, false);
    if (!state.viewpointDragging && !state.wantsMouse && hit >= 0 && pressed) {
        state.viewpointActiveHandle = handle;
        state.viewpointDragAxis = hit;
        state.viewpointDragHandle = handle;
        std::copy (position[handle], position[handle] + 3, state.viewpointDragPoint);
        state.viewpointDragging = PointGumballParameter (state.viewpointDragPoint, DragAxis (hit), origin, direction,
                                                         state.viewpointDragParameter) ||
                                  hit == kGumballCentre;
    }
    // A click off every handle -- pressed and released in place -- deselects.
    if (!state.wantsMouse && input.inside && hit < 0 && pressed && !state.viewpointDragging) {
        state.viewpointPressedOff = true;
        state.viewpointPressAt[0] = x;
        state.viewpointPressAt[1] = y;
    }
    if (state.viewpointPressedOff && (released || !(input.buttons & kMouseLeft))) {
        if (released && std::hypot (x - state.viewpointPressAt[0], y - state.viewpointPressAt[1]) <= 4.0f)
            state.viewpointActiveHandle = -1;
        state.viewpointPressedOff = false;
    }
    state.viewpointOwnsMouse =
        state.viewpointDragging || (!state.wantsMouse && hit >= 0 && (pressed || (input.buttons & kMouseLeft)));
    if (state.viewpointDragging) {
        double moved[3];
        bool ok = state.viewpointDragAxis == kGumballCentre &&
                  SnapUnderCursor (origin, direction, state.viewpointDragHandle == 0, moved);
        double parameter[3];
        if (!ok && PointGumballParameter (state.viewpointDragPoint, DragAxis (state.viewpointDragAxis), origin,
                                          direction, parameter)) {
            for (int axis = 0; axis < 3; ++axis)
                moved[axis] = state.viewpointDragPoint[axis] + parameter[axis] - state.viewpointDragParameter[axis];
            ok = true;
        }
        if (ok && state.viewpointDragHandle == 1) {
            std::copy (moved, moved + 3, state.visibilityTarget);
            state.visibilityTargetEdited = true; // moved by hand: it no longer follows Focus
        }
        else if (ok && state.viewpointDragHandle == 0) {
            auto settings = *shared;
            std::copy (moved, moved + 3, settings.options.point.begin ());
            if (settings.options.point != shared->options.point) {
                FaceTarget (state, settings);
                ConfigureViewpoint (state, std::move (settings));
            }
        }
        if (released || !(input.buttons & kMouseLeft))
            state.viewpointDragging = false;
    }
    state.viewpointHotPart = state.viewpointDragging ? state.viewpointDragAxis : (state.wantsMouse ? -1 : hit);
    state.viewpointHotHandle = state.viewpointDragging ? state.viewpointDragHandle : handle;
    tick ();
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
    const bool placingTarget = state.viewpointPlacingTarget;
    if ((!state.viewpointPlacing && !placingTarget) || state.wantsMouse || !input.inside ||
        !LeftTransition (input, false))
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
    if (placingTarget) {
        // On the surface itself: the target is what is looked at, not an eye.
        std::copy (hit.point, hit.point + 3, state.visibilityTarget);
        state.visibilityTargetSet = true;
        state.visibilityTargetEdited = true;
        state.viewpointActiveHandle = 1;
        state.viewpointPlacingTarget = false;
        state.viewpointPlacementMiss = false;
        return;
    }
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
    state.viewpointActiveHandle = 0;
}

// The isovist itself is drawn occluded by the model (UpdateAndDrawAnalysisOverlay);
// only what must stay on top of it is drawn here.
void DrawViewpointOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const auto settings = viewpointstudy::GetSettings ();
    if (viewProj == nullptr)
        return;
    ImDrawList* front = ImGui::GetForegroundDrawList ();
    const auto handle = [&] (int which, const double* at, uint32_t rgba) {
        const auto gizmo = MakeGumball (at, viewProj, width, height, state.viewpointActiveHandle == which);
        if (!gizmo.valid)
            return;
        const ImU32 centre = IM_COL32 ((rgba >> 24) & 0xffu, (rgba >> 16) & 0xffu, (rgba >> 8) & 0xffu, rgba & 0xffu);
        const int hot = state.viewpointHotHandle == which ? state.viewpointHotPart : -1;
        if (state.viewpointActiveHandle == which)
            DrawGumball (gizmo, front, hot, centre);
        else
            DrawGumballMarker (gizmo.center, front, hot >= 0, centre);
    };
    if (ShowsTargetPoint (state))
        handle (1, state.visibilityTarget, kTargetPointRgba);
    if (state.readOnly || !settings->pointValid || !ShowsPointGumball (state, *settings))
        return DrawGumballHint (state);
    if (state.viewpointPickingContext && state.hover.valid && !state.wantsMouse) {
        auto context = settings->context;
        const bool adding = evp::sunstudy::ToggleViewpointContext (context, state.hover.guid);
        ImGui::SetTooltip ("Click to %s Context:\n%s", adding ? "add to" : "remove from",
                           ElementLabel (visibilitystudy::ViewerCapture ().get (), state.hover.guid).c_str ());
    }
    handle (0, settings->options.point.data (), kViewPointRgba);
    DrawGumballHint (state);
}

} // namespace geomsrv::archviz::viewerhud
