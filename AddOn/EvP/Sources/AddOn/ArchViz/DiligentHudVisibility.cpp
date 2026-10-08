#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>

namespace geomsrv::archviz::viewerhud {

namespace {

enum class RoleEdit { Replace, Add, Remove };

void EditRole (std::vector<std::string>& role, std::vector<std::string>& other,
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
    }
}

bool RoleRow (const char* label, std::vector<std::string>& role, std::vector<std::string>& other,
              const std::vector<std::string>& selection)
{
    bool changed = false;
    ImGui::PushID (label);
    ImGui::Text ("%s  %u element(s)", label, unsigned (role.size ()));
    const bool haveSelection = !selection.empty ();
    ImGui::BeginDisabled (!haveSelection);
    if (ImGui::SmallButton ("Replace")) {
        EditRole (role, other, selection, RoleEdit::Replace);
        changed = true;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Add")) {
        EditRole (role, other, selection, RoleEdit::Add);
        changed = true;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove")) {
        EditRole (role, other, selection, RoleEdit::Remove);
        changed = true;
    }
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Clear")) {
        role.clear ();
        changed = true;
    }
    ImGui::PopID ();
    return changed;
}

void Swatch (const char* id, unsigned rgba)
{
    const ImVec4 colour (float ((rgba >> 24) & 0xff) / 255.0f, float ((rgba >> 16) & 0xff) / 255.0f,
                         float ((rgba >> 8) & 0xff) / 255.0f, float (rgba & 0xff) / 255.0f);
    ImGui::ColorButton (id, colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                        ImVec2 (16.0f, 16.0f));
}

bool LeftReleased (const InputSnapshot& input)
{
    for (int i = 0; i < input.transitionCount; ++i)
        if (input.transitions[i].button == kMouseLeft && !input.transitions[i].down)
            return true;
    return false;
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

} // namespace

void DrawVisibilityHudSection (HudState& state, const DiligentSceneStats& scene)
{
    const auto status = visibilitystudy::GetStatus ();
    bool changed = false;
    ImGui::BeginDisabled (status.running);
    ImGui::TextWrapped ("Measure which square surface cells can see a target. FROM and TO are separate roles.");
    ImGui::TextDisabled ("Current viewport selection: %u element(s)", unsigned (state.visibilitySelection.size ()));

    changed |= ImGui::RadioButton ("FROM surfaces", &state.visibilityOrigin, 0);
    ImGui::SameLine ();
    changed |= ImGui::RadioButton ("Placed view point", &state.visibilityOrigin, 1);
    if (state.visibilityOrigin == 0)
        changed |= RoleRow ("FROM", state.visibilityFrom, state.visibilityTo, state.visibilitySelection);
    changed |= RoleRow ("TO", state.visibilityTo, state.visibilityFrom, state.visibilitySelection);

    if (state.visibilityOrigin == 1) {
        if (ImGui::Button (state.visibilityPlacingPoint ? "Click the model..." : "Place view point")) {
            state.visibilityPlacingPoint = true;
            state.visibilityPlacementMiss = false;
            changed = true;
        }
        ImGui::SameLine ();
        if (state.visibilityPointValid)
            ImGui::TextDisabled ("%.2f, %.2f, %.2f m", state.visibilityPoint[0], state.visibilityPoint[1],
                                 state.visibilityPoint[2]);
        else
            ImGui::TextDisabled ("not placed");
        if (state.visibilityPlacementMiss)
            ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "Point missed the model; click a surface.");
        changed |= ImGui::SliderFloat ("view cone", &state.visibilityConeDegrees, 10.0f, 179.0f, "%.0f deg");
        ImGui::TextDisabled ("The cone looks back toward the camera used to place it.");
    }

    changed |= ImGui::SliderFloat ("square cells", &state.visibilityGrid, 0.25f, 10.0f, "%.2f m",
                                   ImGuiSliderFlags_Logarithmic);
    if (state.visibilityOrigin == 0)
        changed |= ImGui::SliderInt ("TO aim points", &state.visibilityAimPoints, 1, 128);
    ImGui::EndDisabled ();
    if (state.visibilityOrigin == 0)
        state.visibilityPlacingPoint = false;
    if (changed) {
        visibilitystudy::ClearDisplay ();
        state.visibilitySubmitError.clear ();
    }

    const auto capture = visibilitystudy::ViewerCapture ();
    if (capture == nullptr)
        ImGui::TextDisabled ("Waiting for a complete, current viewer capture.");
    const bool ready = capture != nullptr && !status.running && !state.visibilityTo.empty () &&
                       (state.visibilityOrigin == 0 ? !state.visibilityFrom.empty () : state.visibilityPointValid);
    ImGui::BeginDisabled (!ready);
    if (ImGui::Button ("Run visibility", ImVec2 (-1.0f, 0.0f))) {
        state.visibilityPlacingPoint = false;
        visibilitystudy::Request request;
        request.snapshot = capture;
        request.fromElements = state.visibilityFrom;
        request.toElements = state.visibilityTo;
        request.options.origin = state.visibilityOrigin == 0 ? evp::sunstudy::VisibilityOrigin::Surfaces
                                                             : evp::sunstudy::VisibilityOrigin::Point;
        request.options.spacing = state.visibilityGrid;
        request.options.maxAimPoints = static_cast<size_t> ((std::max) (1, state.visibilityAimPoints));
        request.options.coneDegrees = state.visibilityConeDegrees;
        std::copy (state.visibilityPoint, state.visibilityPoint + 3, request.options.point);
        std::copy (state.visibilityDirection, state.visibilityDirection + 3, request.options.direction);
        state.visibilitySubmitError.clear ();
        visibilitystudy::Submit (std::move (request), state.visibilitySubmitError);
    }
    ImGui::EndDisabled ();
    if (status.running) {
        ImGui::TextUnformatted ("Calculating visibility...");
        if (ImGui::SmallButton ("Cancel calculation"))
            visibilitystudy::Cancel ();
    }
    else if (!state.visibilitySubmitError.empty ())
        ImGui::TextWrapped ("Visibility failed: %s", state.visibilitySubmitError.c_str ());
    else if (!changed && !status.error.empty ())
        ImGui::TextWrapped ("Visibility failed: %s", status.error.c_str ());
    else if (!changed && status.complete && scene.sunStudy.analysisKind == 1 && scene.sunStudy.drawing &&
             scene.sunStudy.studyId == status.studyId) {
        ImGui::Text ("%.1f%% mean visible", status.meanVisibility * 100.0);
        ImGui::TextDisabled ("%u cells, %u aim point(s), %.2f M rays, %.0f ms", unsigned (status.sampleCount),
                             unsigned (status.aimPointCount), double (status.rayCount) / 1.0e6,
                             status.analysisMilliseconds);
    }

    Swatch ("##visibility-blocked", 0xB84E45FFu);
    ImGui::SameLine ();
    ImGui::TextUnformatted ("not visible");
    ImGui::SameLine ();
    Swatch ("##visibility-clear", 0x4B8358FFu);
    ImGui::SameLine ();
    ImGui::TextUnformatted ("visible");
    if (scene.sunStudy.drawing && scene.sunStudy.analysisKind == 1) {
        ImGui::TextDisabled ("%s, %u tinted element(s)", scene.sunStudy.studyId.c_str (),
                             unsigned (scene.sunStudy.elementsAttached));
        if (ImGui::SmallButton ("Clear visibility"))
            visibilitystudy::ClearDisplay ();
    }
}

void ServiceAnalysisInteractions (HudState& state, const DiligentScene& scene, const InputSnapshot& input,
                                  const float origin[3], const float direction[3])
{
    ServiceSunStudyInspector (state, scene, origin, direction);
    if (!state.visibilityPlacingPoint || state.wantsMouse || !input.inside || !LeftReleased (input))
        return;
    state.visibilityPlacementMiss = true;
    const auto snapshot = visibilitystudy::ViewerCapture ();
    if (snapshot == nullptr || !state.hover.valid)
        return;
    const double rayOrigin[3] = { origin[0], origin[1], origin[2] };
    const double rayDirection[3] = { direction[0], direction[1], direction[2] };
    QueryEngine::RayHit hit;
    const auto picked = evp::sunstudy::CanonicalGuid (state.hover.guid);
    for (size_t mesh = 0; mesh < snapshot->meshes.size (); ++mesh) {
        if (evp::sunstudy::CanonicalGuid (snapshot->meshes[mesh].guid) != picked)
            continue;
        const auto candidate =
            QueryEngine::RaycastMesh (*snapshot, mesh, rayOrigin, rayDirection, hit.hit ? hit.t : 1.0e6);
        if (candidate.hit)
            hit = candidate;
    }
    if (!hit.hit)
        return;
    for (int axis = 0; axis < 3; ++axis) {
        // Pull the eye 5 cm toward the camera and look back at it. This avoids a
        // self-hit at the placement surface and gives the click a visible bearing.
        state.visibilityPoint[axis] = hit.point[axis] - double (direction[axis]) * 0.05;
        state.visibilityDirection[axis] = -double (direction[axis]);
    }
    state.visibilityPointValid = true;
    state.visibilityPlacingPoint = false;
    state.visibilityPlacementMiss = false;
}

void DrawVisibilityConeOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    if (state.visibilityOrigin != 1 || !state.visibilityPointValid || viewProj == nullptr || width == 0 || height == 0)
        return;
    double direction[3] = { state.visibilityDirection[0], state.visibilityDirection[1], state.visibilityDirection[2] };
    Normalize (direction);
    const bool nearVertical = std::abs (direction[2]) > 0.9;
    const double reference[3] = { 0.0, nearVertical ? 1.0 : 0.0, nearVertical ? 0.0 : 1.0 };
    double u[3], v[3];
    Cross (direction, reference, u);
    Normalize (u);
    Cross (direction, u, v);
    Normalize (v);

    constexpr size_t kSegments = 24;
    const double halfAngle = double (state.visibilityConeDegrees) * 0.5 * 3.14159265358979323846 / 180.0;
    const double tangent = (std::max) (std::tan (halfAngle), 1.0e-4);
    const double radius = (std::min) (1.0, 2.0 * tangent);
    const double length = radius / tangent;
    std::array<ImVec2, kSegments> ring;
    std::array<bool, kSegments> visible = {};
    for (size_t i = 0; i < kSegments; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * double (i) / double (kSegments);
        double point[3];
        for (int axis = 0; axis < 3; ++axis)
            point[axis] = state.visibilityPoint[axis] + direction[axis] * length +
                          radius * (u[axis] * std::cos (angle) + v[axis] * std::sin (angle));
        visible[i] = Project (point, viewProj, width, height, ring[i]);
    }
    ImVec2 apex;
    if (!Project (state.visibilityPoint, viewProj, width, height, apex))
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList ();
    const ImU32 colour = IM_COL32 (75, 131, 88, 230);
    draw->AddCircleFilled (apex, 4.0f, colour);
    for (size_t i = 0; i < kSegments; ++i) {
        const size_t next = (i + 1) % kSegments;
        if (visible[i] && visible[next])
            draw->AddLine (ring[i], ring[next], colour, 2.0f);
        if (visible[i] && i % 6 == 0)
            draw->AddLine (apex, ring[i], colour, 1.5f);
    }
}

} // namespace geomsrv::archviz::viewerhud
