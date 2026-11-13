#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"
#include <cstdio>
#include <imgui.h>

// #191 area B, the toolbar: pinned over the page, two rows of Iconoir glyphs. Row 1: the tools, then
// the landscape Booleans in one popover. Row 2: the modifiers -- how a drag selects, the gizmo, snapping,
// the axes, the drawing plane and the contours, the larger ones in popovers. Every control keeps the
// action and the enable condition it had as a radio or checkbox (EDITOR-CONTROLS.md); the hotkey is in
// the tooltip.
namespace geomsrv::archviz::edithud {
using namespace detail;
namespace ui = geomsrv::archviz::ui;
namespace lg = tapioca::geometry;

namespace {
struct ToolButton {
    Tool tool;
    ui::IconId icon;
    const char* tip;
};

// Row 1's order, which ToolbarSelectsEveryTool191 presses by position.
const ToolButton kTools[] = {
    { Tool::Select, ui::IconId::CursorPointer, "Select (V)" },
    { Tool::Polyline, ui::IconId::Polyline, "Polyline (L)" },
    { Tool::Path, ui::IconId::PathTool, "Path (P)" },
    { Tool::Curve, ui::IconId::Curve, "Curve (B)" },
    { Tool::Rectangle, ui::IconId::Rectangle, "Rectangle (Q)" },
    { Tool::Polygon, ui::IconId::Polygon, "Polygon (N)" },
    { Tool::Ellipse, ui::IconId::Ellipse, "Ellipse (O)" },
    { Tool::Mirror, ui::IconId::Mirror, "Mirror (M)" },
    { Tool::Trim, ui::IconId::Cut, "Trim (X)" },
};

// What each tool's radio did.
void ChooseTool (Session& s, Tool tool)
{
    switch (tool) {
        case Tool::Select:
            SetTool (s, Tool::Select), s.landscapeMode = LandscapeMode::Select, s.drawing.clear ();
            break;
        case Tool::Polyline:
        case Tool::Path:
        case Tool::Curve:
            SetTool (s, tool), s.landscapeMode = LandscapeMode::DrawSource;
            break;
        case Tool::Rectangle:
        case Tool::Polygon:
        case Tool::Ellipse:
            SetTool (s, tool), s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
            break;
        case Tool::Mirror:
            ToolKey (s, 'M');
            break;
        case Tool::Trim:
            ToolKey (s, 'X');
            break;
        default:
            break;
    }
}

ui::IconId GizmoIcon (edit::GizmoMode mode)
{
    switch (mode) {
        case edit::GizmoMode::Move:
            return ui::IconId::GizmoMove;
        case edit::GizmoMode::Rotate:
            return ui::IconId::GizmoRotate;
        case edit::GizmoMode::Scale:
            return ui::IconId::GizmoScale;
        case edit::GizmoMode::Stretch:
            return ui::IconId::GizmoStretch;
        case edit::GizmoMode::Extrude:
            return ui::IconId::GizmoExtrude;
        default:
            return ui::IconId::Gumball;
    }
}

// A glyph that opens `popup` under it; the popover's body is the caller's while this returns true.
bool Popover (const char* id, ui::IconId icon, bool active, const char* tip, const char* popup)
{
    if (ui::IconButton (id, icon, active))
        ImGui::OpenPopup (popup);
    hudshell::Tip (tip, hudshell::TipSide::Above);
    return ImGui::BeginPopup (popup);
}

void BooleansPopover (Session& s)
{
    if (!s.edits.Doc ().landscape)
        return;
    ImGui::SameLine ();
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    if (Popover ("bool", ui::IconId::BooleanUnion, false, "Union, Subtract, Intersection of regions", "##bool")) {
        if (ui::IconTextButton ("union", ui::IconId::BooleanUnion, "Union"))
            BeginLandscapeCommand (s, lg::OperationKind::Union), ImGui::CloseCurrentPopup ();
        if (ui::IconTextButton ("subtract", ui::IconId::BooleanSubtract, "Subtract"))
            BeginLandscapeCommand (s, lg::OperationKind::Difference), ImGui::CloseCurrentPopup ();
        if (ui::IconTextButton ("intersect", ui::IconId::BooleanIntersect, "Intersection"))
            BeginLandscapeCommand (s, lg::OperationKind::Intersection), ImGui::CloseCurrentPopup ();
        ImGui::EndPopup ();
    }
    ImGui::EndDisabled ();
}

void GizmoPopover (Session& s)
{
    // The gumball holds applicable transforms; the same table gates individual modes, and a mode
    // waits while a gesture or a typed value owns the control.
    if (!Popover ("gizmo", GizmoIcon (s.gizmo), false, "Gizmo: gumball, or one transform (G R S T E)", "##gizmo"))
        return;
    const bool locked = GizmoModeLocked (s);
    ImGui::BeginDisabled (locked);
    if (ImGui::RadioButton ("Gumball", s.gizmo == edit::GizmoMode::Gumball))
        SetGizmoMode (s, edit::GizmoMode::Gumball);
    ImGui::EndDisabled ();
    const char* modes[] = { "Move (G)", "Rotate (R)", "Scale (S)", "Stretch (T)", "Extrude (E)" };
    for (int i = 0; i < 5; ++i) {
        ImGui::BeginDisabled (locked || !GizmoAvailable (s, edit::GizmoMode (i)));
        if (ImGui::RadioButton (modes[i], int (s.gizmo) == i))
            SetGizmoMode (s, edit::GizmoMode (i));
        ImGui::EndDisabled ();
    }
    ImGui::EndPopup ();
}

void ContoursPopover (Session& s)
{
    // The grading guide: the paths' contour lines and their heights.
    if (!Popover ("contours", ui::IconId::Contours, s.contours, "Contours", "##contours"))
        return;
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
    ImGui::EndPopup ();
}
} // namespace

void detail::Toolbar (Session& s)
{
    ImGui::PushID ("toolbar");
    // Row 1: the tools.
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    for (const auto& button : kTools) {
        if (&button != kTools)
            ImGui::SameLine ();
        if (ui::IconButton (button.tip, button.icon, s.tool == button.tool))
            ChooseTool (s, button.tool);
        hudshell::Tip (button.tip, hudshell::TipSide::Above);
    }
    ImGui::EndDisabled ();
    BooleansPopover (s);
    // Row 2: the modifiers. How a drag from empty space (or from inside an unselected region)
    // selects; Ctrl at the press takes the other one for that drag. Lasso has no Iconoir glyph.
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    if (ui::IconButton ("window", ui::IconId::WindowSelect, !s.lasso))
        s.lasso = false;
    hudshell::Tip ("Window select. Ctrl-drag: lasso.", hudshell::TipSide::Above);
    ImGui::SameLine (0.0f, 0.0f);
    if (ui::IconTextButton ("lasso", ui::IconId::None, "La", s.lasso))
        s.lasso = true;
    hudshell::Tip ("Lasso select. Ctrl-drag: window.", hudshell::TipSide::Above);
    ImGui::SameLine ();
    GizmoPopover (s);
    ImGui::SameLine ();
    if (Popover ("snap", ui::IconId::Snap, s.snapEnabled, "Snapping and snap options", "##snap")) {
        SnapControls (s, true);
        ImGui::EndPopup ();
    }
    ImGui::SameLine ();
    if (ui::IconTextButton ("axes", ui::IconId::None, s.localAxes ? "Local" : "World"))
        s.localAxes = !s.localAxes;
    hudshell::Tip (s.localAxes ? "Local axes. Click for world axes." : "World axes. Click for local axes.",
                   hudshell::TipSide::Above);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (Popover ("plane", ui::IconId::DrawingPlane, s.plane.locked || s.plane.source != edit::PlaneSource::WorldXY,
                 "Drawing plane: elevation, lock, at a face or along a path", "##plane")) {
        PlaneControls (s);
        ImGui::EndPopup ();
    }
    ImGui::SameLine ();
    ContoursPopover (s);
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::edithud
