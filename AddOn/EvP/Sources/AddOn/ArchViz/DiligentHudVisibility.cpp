#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>

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
              std::vector<std::string>& third, const std::vector<std::string>& selection)
{
    bool changed = false;
    ImGui::PushID (label);
    ImGui::Text ("%s  %u element(s)", label, unsigned (role.size ()));
    const bool haveSelection = !selection.empty ();
    ImGui::BeginDisabled (!haveSelection);
    if (ImGui::SmallButton ("Replace")) {
        EditRole (role, other, third, selection, RoleEdit::Replace);
        changed = true;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Add")) {
        EditRole (role, other, third, selection, RoleEdit::Add);
        changed = true;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove")) {
        EditRole (role, other, third, selection, RoleEdit::Remove);
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

bool DrawConeAim (double direction[3])
{
    constexpr float kDegrees = 57.2957795130823208768f;
    float azimuth = float (std::atan2 (direction[1], direction[0])) * kDegrees;
    float elevation = float (std::asin ((std::max) (-1.0, (std::min) (1.0, direction[2])))) * kDegrees;
    bool changed = ImGui::SliderFloat ("cone azimuth", &azimuth, -180.0f, 180.0f, "%.0f deg");
    changed |= ImGui::SliderFloat ("cone elevation", &elevation, -89.0f, 89.0f, "%.0f deg");
    if (!changed)
        return false;
    const double a = double (azimuth / kDegrees), e = double (elevation / kDegrees);
    direction[0] = std::cos (e) * std::cos (a);
    direction[1] = std::cos (e) * std::sin (a);
    direction[2] = std::sin (e);
    return true;
}

} // namespace

void DrawVisibilityHudSection (HudState& state, const DiligentSceneStats& scene, int origin)
{
    const auto status = visibilitystudy::GetStatus ();
    // Switching between the two visibility tabs only hides the other's result
    // (AnalysisTabs.hpp); a change here clears a result only when it is this tab's.
    state.visibilityOrigin = origin;
    const bool ownResult =
        scene.sunStudy.drawing && scene.sunStudy.analysisKind == 1 && state.analysisFocus.shownOrigin == origin;
    bool changed = false;
    ImGui::BeginDisabled (status.running);
    ImGui::TextWrapped (
        origin == 0 ? "Analysis receives the gradient. Context occludes. Focus is checked for visibility from Analysis."
                    : "Focus receives the gradient. Context occludes. Visibility is checked from a placed view point.");
    ImGui::TextDisabled ("Current viewport selection: %u element(s)", unsigned (state.visibilitySelection.size ()));
    // Context is the viewpoint's, shared by all three view-analysis tabs; Analysis
    // and Focus are shared by the two visibility tabs.
    auto shared = *viewpointstudy::GetSettings ();
    const auto sharedContext = shared.context;
    bool contextChanged = DrawSharedContextToggle (shared);
    if (state.visibilityOrigin == 0)
        changed |=
            RoleRow ("Analysis", state.visibilityFrom, state.visibilityTo, shared.context, state.visibilitySelection);
    if (shared.contextAll)
        ImGui::TextDisabled ("Every element occludes, Analysis and Focus included.");
    else {
        contextChanged |=
            RoleRow ("Context", shared.context, state.visibilityFrom, state.visibilityTo, state.visibilitySelection);
        ImGui::TextDisabled ("Empty Context: no context blockers; unassigned objects are ignored.");
    }
    changed |= RoleRow ("Focus", state.visibilityTo, state.visibilityFrom, shared.context, state.visibilitySelection);
    if (contextChanged || shared.context != sharedContext) {
        ConfigureViewpoint (state, shared);
        changed = true;
    }

    if (state.visibilityOrigin == 1) {
        // The Viewpoint tab's point: placed, typed or dragged in either tab.
        if (ImGui::Button (state.viewpointPlacing ? "Click a model surface..." : "Place view point")) {
            state.viewpointPlacing = true;
            state.viewpointPlacementMiss = false;
            state.viewpointDragging = false;
        }
        if (ImGui::InputScalarN ("Point XYZ (m)", ImGuiDataType_Double, shared.options.point.data (), 3, nullptr,
                                 nullptr, "%.2f")) {
            shared.pointValid = true;
            ConfigureViewpoint (state, shared);
        }
        if (state.viewpointPlacementMiss)
            ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "Point missed the model; click a surface.");
        changed |= ImGui::SliderFloat ("view cone", &state.visibilityConeDegrees, 10.0f, 179.0f, "%.0f deg");
        changed |= DrawConeAim (state.visibilityDirection);
        ImGui::TextDisabled ("Drag the gumball to move the point; the Viewpoint tab shares it.");
        // A shown result was run from where the point was then; once it moves the
        // gradient no longer describes it.
        if (ownResult &&
            !std::equal (state.visibilityRunPoint, state.visibilityRunPoint + 3, shared.options.point.begin ()))
            changed = true;
    }

    changed |= ImGui::SliderFloat ("square cells", &state.visibilityGrid, 0.25f, 10.0f, "%.2f m",
                                   ImGuiSliderFlags_Logarithmic);
    const char* domains[] = { "Triangle (oracle)", "Patch (coplanar surfaces)" };
    changed |= ImGui::Combo ("sampling domain", &state.visibilityDomain, domains, 2);
    if (state.visibilityOrigin == 0)
        changed |= ImGui::SliderInt ("Focus aim points", &state.visibilityAimPoints, 1, 128);
    ImGui::EndDisabled ();
    if (changed) {
        if (ownResult)
            visibilitystudy::ClearDisplay ();
        state.visibilitySubmitError.clear ();
    }

    const auto capture = visibilitystudy::ViewerCapture ();
    if (capture == nullptr)
        ImGui::TextDisabled ("Waiting for a complete, current viewer capture.");
    const bool ready = capture != nullptr && !status.running && !state.visibilityTo.empty () &&
                       (state.visibilityOrigin == 0 ? !state.visibilityFrom.empty () : shared.pointValid);
    ImGui::BeginDisabled (!ready);
    if (ImGui::Button ("Run visibility", ImVec2 (-1.0f, 0.0f))) {
        state.viewpointPlacing = false;
        visibilitystudy::Request request;
        request.snapshot = capture;
        request.fromElements = state.visibilityFrom;
        request.toElements = state.visibilityTo;
        // Without an explicit Context the whole capture occludes.
        request.options.explicitContext = !shared.contextAll;
        request.options.contextElements = shared.context;
        request.options.domain = state.visibilityDomain == 1 ? evp::sunstudy::SamplingDomain::SurfacePatch
                                                             : evp::sunstudy::SamplingDomain::TriangleLegacy;
        request.options.origin = state.visibilityOrigin == 0 ? evp::sunstudy::VisibilityOrigin::Surfaces
                                                             : evp::sunstudy::VisibilityOrigin::Point;
        request.options.spacing = state.visibilityGrid;
        request.options.maxAimPoints = static_cast<size_t> ((std::max) (1, state.visibilityAimPoints));
        request.options.coneDegrees = state.visibilityConeDegrees;
        std::copy (shared.options.point.begin (), shared.options.point.end (), request.options.point);
        std::copy (shared.options.point.begin (), shared.options.point.end (), state.visibilityRunPoint);
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
        hudshell::ErrorNotice ("Visibility failed: " + state.visibilitySubmitError);
    else if (!changed && !status.error.empty () && status.origin == origin)
        hudshell::ErrorNotice ("Visibility failed: " + status.error);
    else if (!changed && status.complete && ownResult && scene.sunStudy.studyId == status.studyId) {
        ImGui::Text ("%.1f%% mean visible", status.meanVisibility * 100.0);
        ImGui::TextDisabled ("%u cells, %u aim point(s), %.2f M rays, %.0f ms", unsigned (status.sampleCount),
                             unsigned (status.aimPointCount), double (status.rayCount) / 1.0e6,
                             status.analysisMilliseconds);
    }

    ImGui::TextDisabled (origin == 0 ? "Outlined: Focus magenta, Analysis violet." : "Outlined: Focus magenta.");
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
    ServiceSunStudyCentrePick (state, input, origin, direction);
}

void DrawVisibilityConeOverlay (const HudState& state, const float viewProj[16], uint32_t width, uint32_t height)
{
    const auto shared = viewpointstudy::GetSettings ();
    if (state.analysisFocus.tab != analysistabs::kPointVisibilityTab || !shared->pointValid || viewProj == nullptr ||
        width == 0 || height == 0)
        return;
    const double* point = shared->options.point.data ();
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
        double at[3];
        for (int axis = 0; axis < 3; ++axis)
            at[axis] = point[axis] + direction[axis] * length +
                       radius * (u[axis] * std::cos (angle) + v[axis] * std::sin (angle));
        visible[i] = Project (at, viewProj, width, height, ring[i]);
    }
    ImVec2 apex;
    if (!Project (point, viewProj, width, height, apex))
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
