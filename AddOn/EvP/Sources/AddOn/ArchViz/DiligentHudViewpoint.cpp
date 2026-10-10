#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/PointGumball.hpp"
#include "ArchViz/ViewpointProjection.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>

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

struct Gumball {
    ImVec2 center, ends[3];
    bool visible[3] = {};
    bool valid = false;
};

Gumball MakeGumball (const double point[3], const float matrix[16], uint32_t width, uint32_t height)
{
    Gumball result;
    result.valid = Project (point, matrix, width, height, result.center);
    if (!result.valid)
        return result;
    float pixelsPerMetre = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        double tip[3] = { point[0], point[1], point[2] };
        tip[axis] += 1.0;
        ImVec2 pixel;
        if (Project (tip, matrix, width, height, pixel))
            pixelsPerMetre =
                (std::max) (pixelsPerMetre, std::hypot (pixel.x - result.center.x, pixel.y - result.center.y));
    }
    if (pixelsPerMetre <= 1.0e-4f)
        return result;
    const double length = 70.0 / pixelsPerMetre;
    for (int axis = 0; axis < 3; ++axis) {
        double tip[3] = { point[0], point[1], point[2] };
        tip[axis] += length;
        result.visible[axis] =
            Project (tip, matrix, width, height, result.ends[axis]) &&
            std::hypot (result.ends[axis].x - result.center.x, result.ends[axis].y - result.center.y) > 15.0f;
    }
    return result;
}

int HitGumball (const Gumball& gizmo, float x, float y)
{
    if (!gizmo.valid)
        return -1;
    if (std::hypot (x - gizmo.center.x, y - gizmo.center.y) <= 9.0f)
        return 3;
    int picked = -1;
    float best = 9.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (!gizmo.visible[axis])
            continue;
        const float dx = gizmo.ends[axis].x - gizmo.center.x, dy = gizmo.ends[axis].y - gizmo.center.y;
        const float t = (std::max) (0.18f, (std::min) (1.1f, ((x - gizmo.center.x) * dx + (y - gizmo.center.y) * dy) /
                                                                 (dx * dx + dy * dy)));
        const float distance = std::hypot (x - gizmo.center.x - t * dx, y - gizmo.center.y - t * dy);
        if (distance < best) {
            best = distance;
            picked = axis;
        }
    }
    return picked;
}

bool LeftTransition (const InputSnapshot& input, bool down)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && input.transitions[i].down == down)
            return true;
    return false;
}

} // namespace

void DrawViewpointHudSection (HudState& state)
{
    auto settings = *viewpointstudy::GetSettings ();
    bool changed = false;
    ImGui::TextWrapped (
        "Horizontal visibility at the point's height. Context stops rays; empty Context gives the full circle.");
    changed |= ImGui::Checkbox ("Show viewpoint", &settings.enabled);
    ImGui::Text ("Context: %u object(s)", unsigned (settings.context.size ()));
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
    changed |= ImGui::InputScalarN ("Point XYZ (m)", ImGuiDataType_Double, settings.options.point.data (), 3, nullptr,
                                    nullptr, "%.2f");
    if (changed)
        settings.pointValid = true;
    if (ImGui::Button (state.viewpointPlacing ? "Click a model surface..." : "Place viewpoint")) {
        state.viewpointPlacing = true;
        state.viewpointDragging = false;
        settings.enabled = true;
        changed = true;
    }
    changed |= ImGui::InputDouble ("Radius (m)", &settings.options.radius, 1.0, 10.0, "%.2f");
    if (changed) {
        settings.options.radius = (std::max) (0.1, (std::min) (10000.0, settings.options.radius));
        viewpointstudy::Configure (std::move (settings));
    }
    ImGui::TextDisabled ("Drag X/Y/Z arrows; drag the centre in XY. Fill: warm yellow, 50%%.");
    const auto status = viewpointstudy::GetStatus ();
    if (status.calculating)
        ImGui::TextDisabled ("Updating visibility...");
    else if (!status.error.empty ())
        ImGui::TextWrapped ("%s", status.error.c_str ());
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
            state.viewpointSelectTab = true;
    }
    state.viewpointOwnsMouse = false;
    if (state.readOnly || !shared->enabled || !shared->pointValid || state.viewpointPlacing) {
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
    viewpointstudy::Configure (std::move (settings));
    state.viewpointPlacing = false;
}

void DrawViewpointOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const auto settings = viewpointstudy::GetSettings ();
    if (state.readOnly || !settings->enabled || !settings->pointValid || viewProj == nullptr)
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
                draw->AddLine (a, b, IM_COL32 (225, 164, 40, 230), 2.0f);
            }
        }
        draw->Flags = flags;
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
    draw = ImGui::GetForegroundDrawList ();
    const ImU32 colours[3] = { IM_COL32 (235, 80, 70, 255), IM_COL32 (70, 190, 100, 255),
                               IM_COL32 (80, 140, 245, 255) };
    const char* names[3] = { "X", "Y", "Z" };
    for (int axis = 0; axis < 3; ++axis) {
        if (!gizmo.visible[axis])
            continue;
        const auto tip = gizmo.ends[axis];
        draw->AddLine (gizmo.center, tip, colours[axis], 3.0f);
        const float dx = tip.x - gizmo.center.x, dy = tip.y - gizmo.center.y;
        const float length = std::hypot (dx, dy), ux = dx / length, uy = dy / length;
        draw->AddTriangleFilled (tip, ImVec2 (tip.x - ux * 10 - uy * 5, tip.y - uy * 10 + ux * 5),
                                 ImVec2 (tip.x - ux * 10 + uy * 5, tip.y - uy * 10 - ux * 5), colours[axis]);
        draw->AddText (ImVec2 (tip.x + 5, tip.y + 5), colours[axis], names[axis]);
    }
    draw->AddCircleFilled (gizmo.center, 7.0f, IM_COL32 (245, 196, 79, 255));
}

} // namespace geomsrv::archviz::viewerhud
