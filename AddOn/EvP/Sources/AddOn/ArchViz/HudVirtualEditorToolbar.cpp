#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"
#include <algorithm>
#include <cstdio>
#include <imgui.h>

// #191 area B, the toolbar: pinned over the page, three rows of Iconoir glyphs. Rows 1 and 2: the tools,
// five to a row, then the landscape Booleans in one popover. Row 3: the modifiers -- how a drag selects, the gizmo,
// snapping, the axes, the drawing plane and the contours, the larger ones in popovers. Every control keeps the action
// and the enable condition it had as a radio or checkbox (EDITOR-CONTROLS.md); the hotkey is in the tooltip.
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

// The tools' order, which ToolbarSelectsEveryTool191 presses by position.
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

// #191 U3 (the user, 2026-11-13: "slightly bigger"): the toolbar's buttons are taller than a frame by
// 0.3 of the font, their glyphs with them (two thirds of the side); the gap between them is half the
// item spacing, so row 1 still fits 320 px. Only the buttons are padded, never a popover's body.
constexpr float kGlyphGrow = 0.15f;

struct Taller {
    Taller ()
    {
        const auto& style = ImGui::GetStyle ();
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding,
                             { style.FramePadding.x, style.FramePadding.y + kGlyphGrow * ImGui::GetFontSize () });
    }
    ~Taller ()
    {
        ImGui::PopStyleVar ();
    }
};

bool Glyph (const char* id, ui::IconId icon, bool active = false, float width = 0.0f)
{
    Taller taller;
    return ui::IconButton (id, icon, active, width);
}

bool GlyphText (const char* id, const char* label, bool active = false)
{
    Taller taller;
    return ui::IconTextButton (id, ui::IconId::None, label, active);
}

void Next ()
{
    ImGui::SameLine (0.0f, ToolGlyphGap ());
}

// A glyph that opens `popup` under it; the popover's body is the caller's while this returns true.
bool Popover (const char* id, ui::IconId icon, bool active, const char* tip, const char* popup, float width = 0.0f)
{
    if (Glyph (id, icon, active, width))
        ImGui::OpenPopup (popup);
    hudshell::Tip (tip, hudshell::TipSide::Above);
    return ImGui::BeginPopup (popup);
}

void BooleansPopover (Session& s, float width)
{
    if (!s.edits.Doc ().landscape)
        return;
    Next ();
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    if (Popover ("bool", ui::IconId::BooleanUnion, false, "Union, Subtract, Intersection of regions", "##bool",
                 width)) {
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

float detail::ToolGlyphSide ()
{
    return ImGui::GetFrameHeight () + 2.0f * kGlyphGrow * ImGui::GetFontSize ();
}

float detail::ToolGlyphGap ()
{
    return ImGui::GetStyle ().ItemSpacing.x * 0.5f;
}

float detail::ToolButtonWidth (float available)
{
    const float width = (available - (kToolsPerRow - 1) * ToolGlyphGap ()) / kToolsPerRow;
    return (std::max) (width, ToolGlyphSide ()); // never narrower than square
}

void detail::Toolbar (Session& s)
{
    ImGui::PushID ("toolbar");
    // Rows 1 and 2: the tools, five to a row filling the panel's width (the user, 2026-11-13), the
    // Booleans tenth.
    const float width = ToolButtonWidth (ImGui::GetContentRegionAvail ().x);
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    for (const auto& button : kTools) {
        if ((&button - kTools) % kToolsPerRow != 0)
            Next ();
        if (Glyph (button.tip, button.icon, s.tool == button.tool, width))
            ChooseTool (s, button.tool);
        hudshell::Tip (button.tip, hudshell::TipSide::Above);
    }
    ImGui::EndDisabled ();
    BooleansPopover (s, width);
    // Row 3: the modifiers. How a drag from empty space (or from inside an unselected region)
    // selects; Ctrl at the press takes the other one for that drag. Lasso has no Iconoir glyph.
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    if (Glyph ("window", ui::IconId::WindowSelect, !s.lasso))
        s.lasso = false;
    hudshell::Tip ("Window select. Ctrl-drag: lasso.", hudshell::TipSide::Above);
    ImGui::SameLine (0.0f, 0.0f);
    if (GlyphText ("lasso", "La", s.lasso))
        s.lasso = true;
    hudshell::Tip ("Lasso select. Ctrl-drag: window.", hudshell::TipSide::Above);
    Next ();
    GizmoPopover (s);
    Next ();
    if (Popover ("snap", ui::IconId::Snap, s.snapEnabled, "Snapping and snap options", "##snap")) {
        SnapControls (s, true);
        ImGui::EndPopup ();
    }
    Next ();
    if (GlyphText ("axes", s.localAxes ? "Local" : "World"))
        s.localAxes = !s.localAxes;
    hudshell::Tip (s.localAxes ? "Local axes. Click for world axes." : "World axes. Click for local axes.",
                   hudshell::TipSide::Above);
    ImGui::EndDisabled ();
    Next ();
    if (Popover ("plane", ui::IconId::DrawingPlane, s.plane.locked || s.plane.source != edit::PlaneSource::WorldXY,
                 "Drawing plane: elevation, lock, at a face or along a path", "##plane")) {
        PlaneControls (s);
        ImGui::EndPopup ();
    }
    Next ();
    ContoursPopover (s);
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::edithud
