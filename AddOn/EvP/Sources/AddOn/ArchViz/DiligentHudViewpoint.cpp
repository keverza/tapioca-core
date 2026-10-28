#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/HudGumball.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/PointGumball.hpp"
#include "ArchViz/ViewpointProjection.hpp"
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

bool Project (const double point[3], const float matrix[16], uint32_t width, uint32_t height, ImVec2& pixel)
{
    const float world[4] = { float (point[0]), float (point[1]), float (point[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, world, matrix);
    if (!std::isfinite (clip[3]) || clip[3] <= 1.0e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    pixel =
        ImVec2 ((clip[0] / clip[3] * 0.5f + 0.5f) * float (width), (0.5f - clip[1] / clip[3] * 0.5f) * float (height));
    return std::isfinite (pixel.x) && std::isfinite (pixel.y);
}

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
    if (!ImGui::TreeNodeEx ("##viewpoint-context", ImGuiTreeNodeFlags_DefaultOpen, "Context: %u object(s)",
                            unsigned (settings.context.size ())))
        return;
    if (settings.context.empty ())
        ImGui::TextDisabled ("None: every ray reaches the radius.");
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

bool LeftTransition (const InputSnapshot& input, bool down)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && input.transitions[i].down == down)
            return true;
    return false;
}

} // namespace

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
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Shared by Surface visibility, Point visibility and Viewpoint.");
    return changed;
}

void DrawViewpointHudSection (HudState& state)
{
    auto settings = *viewpointstudy::GetSettings ();
    bool changed = false;
    ImGui::TextWrapped (
        "Horizontal visibility at the point's height. Context stops rays; empty Context gives the full circle.");
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
    if (state.viewpointPickingContext)
        ImGui::TextDisabled ("Click elements to add or remove them; the viewer selection is not used.");
    DrawContextList (settings, status, changed);
    if (!settings.contextAll) {
        ImGui::TextDisabled ("From the viewer selection:");
        ImGui::BeginDisabled (state.visibilitySelection.empty ());
        if (ImGui::SmallButton ("Replace Context")) {
            settings.context = state.visibilitySelection;
            changed = true;
        }
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Add")) {
            for (const auto& guid : state.visibilitySelection)
                if (std::find (settings.context.begin (), settings.context.end (), guid) == settings.context.end ())
                    settings.context.push_back (guid);
            changed = true;
        }
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Remove")) {
            for (const auto& guid : state.visibilitySelection)
                settings.context.erase (std::remove (settings.context.begin (), settings.context.end (), guid),
                                        settings.context.end ());
            changed = true;
        }
        ImGui::EndDisabled ();
        if (ImGui::SmallButton ("Clear Context")) {
            settings.context.clear ();
            changed = true;
        }
    }
    changed |= ImGui::InputScalarN ("Point XYZ (m)", ImGuiDataType_Double, settings.options.point.data (), 3, nullptr,
                                    nullptr, "%.2f");
    if (changed)
        settings.pointValid = true;
    if (ImGui::Button (state.viewpointPlacing ? "Click a model surface..." : "Place viewpoint")) {
        state.viewpointPlacing = true;
        state.viewpointPickingContext = false;
        state.viewpointDragging = false;
        settings.enabled = true;
        changed = true;
    }
    changed |= ImGui::InputDouble ("Radius (m)", &settings.options.radius, 1.0, 10.0, "%.2f");
    if (changed) {
        settings.options.radius = (std::max) (0.1, (std::min) (10000.0, settings.options.radius));
        ConfigureViewpoint (state, std::move (settings));
    }
    ImGui::TextDisabled ("Drag X/Y/Z arrows; drag the centre in XY. Fill: warm yellow, 50%%.");
    ImGui::TextDisabled ("Edge: orange where Context stops the ray, yellow at the radius.");
    if (status.calculating)
        ImGui::TextDisabled ("Updating visibility...");
    else if (!status.error.empty ())
        hudshell::ErrorNotice (status.error);
    else if (status.result != nullptr)
        ImGui::Text ("%.1f m2 visible; %u of %u rays hit Context", status.result->area,
                     unsigned (status.result->collisionCount), unsigned (status.result->perimeter.size ()));
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
    if (state.readOnly || !shared->enabled || !shared->pointValid || state.viewpointPlacing ||
        state.analysisFocus.tab != analysistabs::kViewpointTab) {
        state.viewpointDragging = false;
        viewpointstudy::Tick (visibilitystudy::ViewerCapture ());
        return;
    }
    const auto gizmo = MakeGumball (shared->options.point.data (), viewProj, width, height);
    const int hit =
        input.inside ? HitGumball (gizmo, float (CursorTargetX (input, width)), float (CursorTargetY (input, height)))
                     : -1;
    const double origin[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    const double direction[3] = { rayDirection[0], rayDirection[1], rayDirection[2] };
    if (!state.viewpointDragging && !state.wantsMouse && hit >= 0 && LeftTransition (input, true)) {
        state.viewpointDragAxis = hit;
        std::copy (shared->options.point.begin (), shared->options.point.end (), state.viewpointDragPoint);
        state.viewpointDragging =
            PointGumballParameter (state.viewpointDragPoint, hit, origin, direction, state.viewpointDragParameter);
    }
    state.viewpointOwnsMouse =
        state.viewpointDragging ||
        (!state.wantsMouse && hit >= 0 && (LeftTransition (input, true) || (input.buttons & kMouseLeft)));
    if (state.viewpointDragging) {
        double parameter[3];
        if (PointGumballParameter (state.viewpointDragPoint, state.viewpointDragAxis, origin, direction, parameter)) {
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
    if (!state.viewpointPlacing || state.wantsMouse || !input.inside || !LeftTransition (input, false) ||
        !state.hover.valid)
        return;
    const auto snapshot = visibilitystudy::ViewerCapture ();
    if (snapshot == nullptr)
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
    for (int axis = 0; axis < 3; ++axis)
        settings.options.point[axis] = hit.point[axis] - direction[axis] * 0.05;
    ConfigureViewpoint (state, std::move (settings));
    state.viewpointPlacing = false;
}

void DrawViewpointOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const auto settings = viewpointstudy::GetSettings ();
    if (state.readOnly || !settings->enabled || !settings->pointValid || viewProj == nullptr ||
        state.analysisFocus.tab != analysistabs::kViewpointTab)
        return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList ();
    const auto status = viewpointstudy::GetStatus ();
    if (status.result != nullptr) {
        const auto& perimeter = status.result->perimeter;
        const auto flags = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (size_t i = 0; i < perimeter.size (); ++i) {
            const auto polygon =
                ProjectViewpointTriangle (status.result->options.point.data (), perimeter[i].data (),
                                          perimeter[(i + 1) % perimeter.size ()].data (), viewProj, width, height);
            for (size_t triangle = 1; triangle + 1 < polygon.count; ++triangle)
                draw->AddTriangleFilled (ImVec2 (polygon.points[0][0], polygon.points[0][1]),
                                         ImVec2 (polygon.points[triangle][0], polygon.points[triangle][1]),
                                         ImVec2 (polygon.points[triangle + 1][0], polygon.points[triangle + 1][1]),
                                         IM_COL32 (245, 196, 79, 128));
            ImVec2 a, b;
            if (Project (perimeter[i].data (), viewProj, width, height, a) &&
                Project (perimeter[(i + 1) % perimeter.size ()].data (), viewProj, width, height, b)) {
                // An edge between two stopped rays lies on Context: draw it as a collision.
                const auto& rayMesh = status.result->rayMesh;
                const bool blocked = rayMesh.size () == perimeter.size () && rayMesh[i] >= 0 &&
                                     rayMesh[(i + 1) % perimeter.size ()] >= 0;
                draw->AddLine (a, b, blocked ? IM_COL32 (236, 98, 64, 240) : IM_COL32 (225, 164, 40, 230),
                               blocked ? 3.0f : 2.0f);
            }
        }
        draw->Flags = flags;
        // Mark where each Context run starts and ends: the edges of what blocks the view.
        if (status.result->rayMesh.size () == perimeter.size ())
            for (const auto& arc : evp::sunstudy::ViewpointArcs (status.result->rayMesh)) {
                if (arc.mesh < 0 || arc.count == perimeter.size ())
                    continue;
                for (const size_t ray : { arc.first, (arc.first + arc.count - 1) % perimeter.size () }) {
                    ImVec2 at;
                    if (Project (perimeter[ray].data (), viewProj, width, height, at))
                        draw->AddCircleFilled (at, 4.0f, IM_COL32 (236, 98, 64, 255));
                }
            }
    }
    if (state.viewpointPickingContext && state.hover.valid && !state.wantsMouse) {
        auto context = settings->context;
        const bool adding = evp::sunstudy::ToggleViewpointContext (context, state.hover.guid);
        ImGui::SetTooltip ("Click to %s Context:\n%s", adding ? "add to" : "remove from",
                           ElementLabel (visibilitystudy::ViewerCapture ().get (), state.hover.guid).c_str ());
    }
    // The full circle is a radius guide, distinct from the clipped visibility boundary.
    for (size_t i = 0; i < 180; ++i) {
        ImVec2 ends[2];
        bool visible = true;
        for (int end = 0; end < 2; ++end) {
            const double angle = 6.28318530717958647692 * double (i + end) / 180.0;
            const double point[3] = { settings->options.point[0] + settings->options.radius * std::cos (angle),
                                      settings->options.point[1] + settings->options.radius * std::sin (angle),
                                      settings->options.point[2] };
            visible &= Project (point, viewProj, width, height, ends[end]);
        }
        if (visible)
            draw->AddLine (ends[0], ends[1], IM_COL32 (245, 196, 79, 150), 1.0f);
    }
    const auto gizmo = MakeGumball (settings->options.point.data (), viewProj, width, height);
    if (!gizmo.valid)
        return;
    DrawGumball (gizmo, ImGui::GetForegroundDrawList ());
}

} // namespace geomsrv::archviz::viewerhud
