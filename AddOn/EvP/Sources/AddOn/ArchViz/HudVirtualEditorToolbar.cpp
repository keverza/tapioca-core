#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <cstdio>
#include <imgui.h>

// #191 area B, the toolbar: pinned over the page. The tools, how a drag selects, the gizmo, snapping,
// the axes, the working plane and the contours.
namespace geomsrv::archviz::edithud {
using namespace detail;

void detail::Toolbar (Session& s)
{
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    if (ImGui::RadioButton ("Select (V)", s.tool == Tool::Select))
        SetTool (s, Tool::Select), s.landscapeMode = LandscapeMode::Select, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Polyline (L)", s.tool == Tool::Polyline))
        SetTool (s, Tool::Polyline), s.landscapeMode = LandscapeMode::DrawSource;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Path (P)", s.tool == Tool::Path))
        SetTool (s, Tool::Path), s.landscapeMode = LandscapeMode::DrawSource;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Curve (B)", s.tool == Tool::Curve))
        SetTool (s, Tool::Curve), s.landscapeMode = LandscapeMode::DrawSource;
    if (ImGui::RadioButton ("Rectangle (Q)", s.tool == Tool::Rectangle))
        SetTool (s, Tool::Rectangle), s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Polygon (N)", s.tool == Tool::Polygon))
        SetTool (s, Tool::Polygon), s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Ellipse (O)", s.tool == Tool::Ellipse))
        SetTool (s, Tool::Ellipse), s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Mirror (M)", s.tool == Tool::Mirror))
        ToolKey (s, 'M');
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Trim (X)", s.tool == Tool::Trim))
        ToolKey (s, 'X');
    // How a drag from empty space (or from inside an unselected region) selects; Ctrl at the
    // press takes the other one for that drag.
    if (ImGui::RadioButton ("Window select", !s.lasso))
        s.lasso = false;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Lasso select", s.lasso))
        s.lasso = true;
    ImGui::SameLine ();
    ImGui::TextDisabled ("Ctrl-drag: the other");
    // The gumball holds applicable transforms; the same table gates individual modes, and a
    // mode waits while a gesture or a typed value owns the control.
    const bool locked = GizmoModeLocked (s);
    ImGui::BeginDisabled (locked);
    if (ImGui::RadioButton ("Gumball", s.gizmo == edit::GizmoMode::Gumball))
        SetGizmoMode (s, edit::GizmoMode::Gumball);
    ImGui::EndDisabled ();
    const char* modes[] = { "Move (G)", "Rotate (R)", "Scale (S)", "Stretch (T)", "Extrude (E)" };
    for (int i = 0; i < 5; ++i) {
        ImGui::SameLine ();
        ImGui::BeginDisabled (locked || !GizmoAvailable (s, edit::GizmoMode (i)));
        if (ImGui::RadioButton (modes[i], int (s.gizmo) == i))
            SetGizmoMode (s, edit::GizmoMode (i));
        ImGui::EndDisabled ();
    }
    SnapControls (s);
    if (ImGui::RadioButton ("Local axes", s.localAxes))
        s.localAxes = true;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("World axes", !s.localAxes))
        s.localAxes = false;
    ImGui::EndDisabled ();
    PlaneControls (s);
    // The grading guide: the paths' contour lines and their heights.
    bool shown = s.contours;
    if (ImGui::Checkbox ("Contours", &shown))
        s.contours = shown, s.publishedRevision = UINT64_MAX; // the layer draws them
    if (s.contours) {
        static const double steps[] = { 0.0, 0.05, 0.1, 0.25, 0.5, 1.0 };
        char preview[32];
        if (s.contourStep > 0.0)
            std::snprintf (preview, sizeof preview, "%g m", s.contourStep);
        else
            std::snprintf (preview, sizeof preview, "Auto (%g m)", s.contoursInterval);
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (110.0f);
        if (ImGui::BeginCombo ("##contourStep", preview)) {
            for (const double step : steps) {
                char label[32];
                std::snprintf (label, sizeof label, step > 0.0 ? "%g m" : "Auto", step);
                if (ImGui::Selectable (label, step == s.contourStep))
                    s.contourStep = step, s.publishedRevision = UINT64_MAX;
            }
            ImGui::EndCombo ();
        }
        hudshell::Tip ("Height between contour lines", hudshell::TipSide::Above);
    }
}
} // namespace geomsrv::archviz::edithud
