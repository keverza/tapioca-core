#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <imgui.h>

namespace geomsrv::archviz::edithud {

const std::vector<SnapOption>& SnapOptions ()
{
    static const std::vector<SnapOption> options {
        { edit::kSnapEndpoint, "Endpoints / control points" },
        { edit::kSnapMidpoint, "Midpoints" },
        { edit::kSnapIntersection, "Intersections" },
        { edit::kSnapCentre, "Centres" },
        { edit::kSnapPerpendicular, "Perpendicular" },
        { edit::kSnapNearest, "Nearest point on edge" },
        { edit::kSnapAxis, "Axis guides" },
        { edit::kSnapExtension, "Edge extensions" },
        { edit::kSnapGrid, "Grid" },
        { edit::kSnapTerrain, "Terrain elevation" },
        { edit::kSnapParallel, "Parallel / square guides" },
        { edit::kSnapAngle, "Angle guides" },
    };
    return options;
}

namespace detail {

void SnapControls (Session& s)
{
    ImGui::Checkbox ("Snapping", &s.snapEnabled);
    hudshell::Tip ("Suspend snapping without clearing your selected snap modes.", hudshell::TipSide::Above);
    ImGui::SameLine ();
    if (!ImGui::TreeNode ("Snap options"))
        return;
    for (const auto& option : SnapOptions ()) {
        bool enabled = (s.snapModes & option.mode) != 0;
        if (ImGui::Checkbox (option.label, &enabled))
            s.snapModes = enabled ? s.snapModes | option.mode : s.snapModes & ~option.mode;
        if (option.mode == edit::kSnapTerrain)
            hudshell::Tip ("Uses the supplied terrain snapshot only; no terrain means no elevation snap.",
                           hudshell::TipSide::Above);
    }
    ImGui::Checkbox ("Flush edges near pointer", &s.edgeSnap);
    hudshell::Tip ("Align the grabbed edge with a nearby parallel target. Edge alignment has a wider pointer reach "
                   "than point snaps; distant edges never move the whole selection.",
                   hudshell::TipSide::Above);
    if ((s.snapModes & edit::kSnapAngle) != 0) {
        float step = float (s.angleStep);
        ImGui::SetNextItemWidth (120.0f);
        if (ImGui::SliderFloat ("Angle step", &step, 5.0f, 45.0f, "%.0f deg"))
            s.angleStep = step;
    }
    if ((s.snapModes & edit::kSnapGrid) != 0) {
        float grid = float (s.snapGrid);
        ImGui::SetNextItemWidth (120.0f);
        if (ImGui::DragFloat ("Grid spacing", &grid, 0.05f, 0.01f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp))
            s.snapGrid = grid;
    }
    // How far in 3D a host point may be and still be "here" for a snap; the viewport and camera
    // are screened separately; a positive bound is always kept in 3D.
    float proximity = float (s.snapProximity);
    ImGui::SetNextItemWidth (120.0f);
    if (ImGui::DragFloat ("3D proximity", &proximity, 0.05f, 0.01f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp))
        s.snapProximity = proximity;
    hudshell::Tip ("A host point farther than this, in 3D, is not snapped to.", hudshell::TipSide::Above);
    ImGui::TreePop ();
}

} // namespace detail
} // namespace geomsrv::archviz::edithud
