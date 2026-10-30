#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditPathEdit.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// The virtual editor's panel in the host (the miniGUI) and the layer it publishes.
namespace geomsrv::archviz::edithud {

using namespace detail;

namespace {
// The profile's grid: lines for round heights, fainter ones for round stations, its heights.
const ImU32 kGrid = IM_COL32 (200, 205, 212, 255);
const ImU32 kGridFaint = IM_COL32 (228, 231, 236, 255);
const ImU32 kGridText = IM_COL32 (110, 118, 130, 255);

// Turns what was just drawn about its box's centre, to read along it: the same arithmetic the
// view's transient labels use (HudVirtualEditorDraw.cpp Turn), so a value reads the same
// turned in the panel as over the view.
void Turn (ImDrawList* list, int first, const editlayers::Box& box)
{
    if (box.ux == 1.0f && box.uy == 0.0f)
        return;
    const float cx = (box.x0 + box.x1) * 0.5f, cy = (box.y0 + box.y1) * 0.5f;
    for (int i = first; i < list->VtxBuffer.Size; ++i) {
        ImVec2& p = list->VtxBuffer[i].pos;
        const float x = p.x - cx, y = p.y - cy;
        p = { cx + x * box.ux - y * box.uy, cy + x * box.uy + y * box.ux };
    }
}

// A label's ink in its box, turned as the box is.
void Inked (ImDrawList* list, const editlayers::Box& box, const std::string& text, ImU32 ink)
{
    const int first = list->VtxBuffer.Size;
    list->AddText ({ box.x0 + 4.0f, box.y0 + 1.0f }, ink, text.c_str ());
    Turn (list, first, box);
}

// A downhill slope indicator along a tangent's projected run: a shaft and a filled head, the
// arrowhead the model path draws as a line terminator (EditLayers Arrow), built here in pixels.
void SlopeArrow (ImDrawList* list, ImVec2 from, ImVec2 to, ImU32 colour, float scale)
{
    const float dx = to.x - from.x, dy = to.y - from.y, len = std::hypot (dx, dy);
    if (len < 6.0f * scale)
        return;
    const float ux = dx / len, uy = dy / len, nx = -uy, ny = ux;
    const float head = std::min (10.0f * scale, len * 0.5f), half = 0.5f * head;
    const ImVec2 base { to.x - ux * head, to.y - uy * head };
    list->AddLine (from, base, colour, 1.5f * scale);
    list->AddTriangleFilled (to, { base.x + nx * half, base.y + ny * half }, { base.x - nx * half, base.y - ny * half },
                             colour);
}
// A sharp break's smoothing nubs stand this far out from it, so there is something to grab.
constexpr double kNubPixels = 16.0;
} // namespace

void Panel (Session& s)
{
    const auto& doc = Doc (s);
    s.profilePlaced.clear (); // the canvas republishes its labels every frame
    LandscapePanel (s);
    ImGui::Text ("%zu objects   %zu paths   %zu selected", doc.entities.size (), doc.paths.size (),
                 s.selection.Items ().size ());
    ImGui::BeginDisabled (s.debugExportRequested || s.edits.InGesture () || !s.drawing.empty () || s.terrainCapture ||
                          s.draftingImport || GizmoModeLocked (s) || s.landscapeCommand);
    if (ImGui::Button (s.debugExportRequested ? "Export requested..." : "Export debug scene"))
        RequestDebugExport (s);
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip ("Save paths, fillet settings, captured topography and current design to a new JSON file "
                           "in %%LOCALAPPDATA%%\\Tapioca\\dumps. Export before and after a problematic edit.");
    if (!s.debugExportPath.empty ())
        ImGui::TextWrapped ("Debug scene saved: %s", s.debugExportPath.c_str ());
    if (!s.debugExportError.empty ())
        ImGui::TextWrapped ("Debug export failed: %s", s.debugExportError.c_str ());
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    if (ImGui::RadioButton ("Select (V)", s.tool == Tool::Select))
        s.tool = Tool::Select, s.landscapeMode = LandscapeMode::Select, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Polyline (L)", s.tool == Tool::Polyline))
        s.tool = Tool::Polyline, s.landscapeMode = LandscapeMode::DrawSource;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Path (P)", s.tool == Tool::Path))
        s.tool = Tool::Path, s.landscapeMode = LandscapeMode::DrawSource;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Curve (B)", s.tool == Tool::Curve))
        s.tool = Tool::Curve, s.landscapeMode = LandscapeMode::DrawSource;
    if (ImGui::RadioButton ("Rectangle (Q)", s.tool == Tool::Rectangle))
        s.tool = Tool::Rectangle, s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Polygon (N)", s.tool == Tool::Polygon))
        s.tool = Tool::Polygon, s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Ellipse (O)", s.tool == Tool::Ellipse))
        s.tool = Tool::Ellipse, s.landscapeMode = LandscapeMode::DrawSource, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Mirror (M)", s.tool == Tool::Mirror))
        ToolKey (s, 'M');
    if (s.tool == Tool::Mirror) {
        ImGui::Checkbox ("Keep original", &s.mirrorKeeps);
        ImGui::SameLine ();
        ImGui::TextDisabled ("Click the mirror line's two points. Ctrl: the other way.");
    }
    if (s.tool == Tool::Polygon) {
        ImGui::SetNextItemWidth (120);
        ImGui::SliderInt ("Sides (Up/Down)", &s.polygonSides, 3, edit::kMaxPolygonSides);
    }
    if (ShapeTool (s.tool))
        ImGui::TextDisabled ("Click a corner or centre, then the size. Shift: square, circle.");
    // Archicad's own drafting brought in as editor lines: what is selected in Archicad now.
    ImGui::BeginDisabled (s.draftingImport || s.edits.InGesture () || !s.drawing.empty ());
    if (ImGui::SmallButton (s.draftingImport ? "Adding Archicad lines..." : "Add selected Archicad lines and fills"))
        s.draftingImport = true, s.draftingImportAs = "lines";
    // The same selection as a landscape's reference outlines, or its doors as entrances.
    if (s.edits.Doc ().landscape) {
        const struct {
            const char *label, *as;
        } refs[] = { { "As property line", "property" },
                     { "As building outline", "building" },
                     { "Doors as entrances", "entrances" } };
        for (size_t i = 0; i < std::size (refs); ++i) {
            if (i)
                ImGui::SameLine ();
            if (ImGui::SmallButton (refs[i].label))
                s.draftingImport = true, s.draftingImportAs = refs[i].as;
        }
    }
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip ("Lines, polylines, arcs, circles and fill outlines selected in Archicad, at the "
                           "working plane's height. One undo step.");
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
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    if (ImGui::SmallButton ("Undo"))
        History (s, true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Redo"))
        History (s, false);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Delete"))
        DeleteSelection (s);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Done"))
        s.close = true;
    // Context actions for what is selected.
    // An action with nothing to do here is shown greyed, as the menu shows it.
    bool offers = false, offset = false;
    for (const edit::Action* a : edit::Applicable (Actions (s), s.selection, edit::ViewKind::Plan)) {
        const auto op = a->make (doc, s.selection.Items ());
        if (offers)
            ImGui::SameLine ();
        offers = true;
        offset = offset || a->id == "offset";
        ImGui::BeginDisabled (!op);
        if (ImGui::SmallButton (a->label.c_str ())) {
            const edit::Id since = doc.nextId;
            if (!s.edits.Apply (op))
                s.message = s.edits.Message ();
            SelectMade (s, since);
        }
        ImGui::EndDisabled ();
    }
    // Its distance comes after the rows: changing it rebuilds the table they point into.
    if (offset) {
        float distance = float (s.actionDefaults.offsetDistance);
        ImGui::SetNextItemWidth (110.0f);
        if (ImGui::DragFloat ("Offset distance", &distance, 0.01f, -50.0f, 50.0f, "%.2f m"))
            s.actionDefaults.offsetDistance = double (distance);
        hudshell::Tip ("Positive: outside a closed outline or left of a line. Negative: inside or right.",
                       hudshell::TipSide::Above);
    }
    ImGui::EndDisabled ();
    OrganizeControls (s);
    EllipseControls (s);

    // What is selected, measured: the tracker's values (type a number over the view to set one).
    if (s.tracker.State () != edit::TrackerState::Hidden && !s.tracker.Items ().empty ()) {
        ImGui::Separator ();
        for (size_t i = 0; i < s.tracker.Items ().size (); ++i) {
            const auto& item = s.tracker.Items ()[i];
            const bool typing = s.tracker.State () == edit::TrackerState::Editing && i == s.tracker.Focus ();
            const std::string value =
                typing ? s.tracker.Text () + "_"
                       : editlayers::FormatValue (item.quantity, item.locked ? item.lockedValue : item.value);
            ImGui::Text ("%s  %s%s", edit::QuantityName (item.quantity), value.c_str (),
                         item.locked ? "  (locked)" : "");
            if (i + 1 < s.tracker.Items ().size ())
                ImGui::SameLine (0.0f, 18.0f);
        }
    }

    const edit::Path* path = ProfilePath (s);
    if (path != nullptr) {
        s.profilePath = path->id;
        ImGui::Separator ();
        ImGui::Text ("Path profile  (%zu breaks, %zu ranges)", path->profile.size (), path->sections.size ());
        ImGui::BeginDisabled (s.profileTyping);
        // Its kerb radius: its outside corners and its junctions with other paths rounded.
        float kerb = float (path->edgeRadius);
        ImGui::SetNextItemWidth (140.0f);
        if (ImGui::DragFloat ("Kerb radius", &kerb, 0.05f, 0.0f, 20.0f, "%.2f m") && s.drag == Drag::None) {
            const edit::Id id = path->id;
            const double r = double (kerb);
            if (!s.edits.Apply (
                    [id, r] (edit::Document& d, std::string& e) { return edit::SetPathEdgeRadius (d, id, r, e); }))
                s.message = s.edits.Message ();
        }
        // A kerb corner picked on the view: its own radius, apart from the path's.
        if (const edit::PavedCorner* k = PickedKerb (s)) {
            float r = float (k->requestedRadius);
            ImGui::SetNextItemWidth (140.0f);
            if (ImGui::DragFloat (k->key.other != edit::kNoId ? "Junction corner" : "Corner radius", &r, 0.05f, 0.0f,
                                  0.0f, "%.2f m") &&
                s.drag == Drag::None) {
                const edit::PavedCorner c = *k;
                const double v = double (r);
                if (!s.edits.Apply ([c, v] (edit::Document& d, std::string& e) {
                        return edit::SetKerbRadius (d, c.path, c.key, v, e);
                    }))
                    s.message = s.edits.Message ();
            }
            if (k->Limited ())
                ImGui::Text ("Fitted radius %.2f m. Requested %.2f m.", k->key.radius, k->requestedRadius);
        }
        // Path actions on the selected part.
        const auto& items = s.selection.Items ();
        const edit::Ref* part = items.size () == 1 && items[0].entity == path->id ? &items[0] : nullptr;
        if (part != nullptr && part->part == edit::Part::Break)
            for (const auto& b : path->profile)
                if (b.id == part->item) {
                    // Its smoothing: the vertical curve's length, as its nubs on the canvas set it.
                    size_t index = 0;
                    while (index < path->profile.size () && path->profile[index].id != b.id)
                        ++index;
                    const double room = edit::MaxCurve (*path, index);
                    if (room > 0.0) {
                        float length = float (b.curve);
                        ImGui::SetNextItemWidth (140.0f);
                        if (ImGui::SliderFloat ("Smoothing", &length, 0.0f, float (room), "%.2f m") &&
                            s.drag == Drag::None) {
                            const auto op = edit::DragCurve (path->id, b.id, double (length));
                            if (!s.edits.Apply (op))
                                s.message = s.edits.Message ();
                        }
                        ImGui::SameLine ();
                    }
                    if (ImGui::SmallButton (b.fixed ? "Unfix elevation" : "Fix elevation")) {
                        const edit::Id id = path->id, bid = b.id;
                        const double station = b.station, z = b.z;
                        const bool fixed = b.fixed;
                        s.edits.Apply ([=] (edit::Document& d, std::string& e) {
                            return fixed ? edit::UnfixBreak (d, id, bid, e)
                                         : edit::FixElevationAt (d, id, station, z, "", e);
                        });
                    }
                    break;
                }
        // How it sheds water: a picked range's fall, or every range's of the path selected whole
        // (none of the three shown chosen while they differ); and the side flipped.
        const bool whole = part != nullptr && part->part == edit::Part::Entity;
        if (part != nullptr && (whole || part->part == edit::Part::Range)) {
            const edit::Id range = whole ? edit::kNoId : part->item;
            int fall = -1;
            bool sided = false;
            for (const auto& r : path->sections)
                if (range == edit::kNoId || r.id == range) {
                    fall = fall == -1 || fall == int (r.fall) ? int (r.fall) : -2;
                    sided = sided || r.fall != edit::Fall::Crown;
                }
            const char* falls[] = { "Crown", "Falls left", "Falls right" };
            const edit::Id id = path->id;
            for (int i = 0; i < 3; ++i) {
                if (i > 0)
                    ImGui::SameLine ();
                if (ImGui::RadioButton (falls[i], fall == i) &&
                    !s.edits.Apply ([=] (edit::Document& d, std::string& e) {
                        return edit::SetPathFall (d, id, range, edit::Fall (i), e);
                    }))
                    s.message = s.edits.Message ();
            }
            ImGui::SameLine ();
            ImGui::BeginDisabled (!sided);
            if (ImGui::SmallButton ("Flip fall") && !s.edits.Apply ([=] (edit::Document& d, std::string& e) {
                    return edit::FlipPathFall (d, id, range, e);
                }))
                s.message = s.edits.Message ();
            ImGui::EndDisabled ();
        }

        ImGui::EndDisabled ();
        // The profile canvas: stations right, elevations up, exaggerated to fit, with a gutter
        // on the left for its grid's heights.
        const float width = std::max (200.0f, ImGui::GetContentRegionAvail ().x);
        const float height = 150.0f;
        ImGui::InvisibleButton ("##profile", { width, height });
        const ImVec2 min = ImGui::GetItemRectMin ();
        const bool over = ImGui::IsItemHovered ();
        ImDrawList* list = ImGui::GetWindowDrawList ();
        double highest = 0.0;
        for (const auto& b : path->profile)
            highest = std::max (highest, std::abs (b.z));
        char widest[32];
        std::snprintf (widest, sizeof widest, "-%.2f", highest);
        const float gutter = ImGui::CalcTextSize (widest).x;
        // The labels sit beside their marks: the canvas keeps a label's line height clear at
        // every edge, so an elevation above a grade point is inside it, never clipped away.
        const float lineHeight = ImGui::GetTextLineHeight ();
        const float margin = 14.0f + lineHeight;
        edit::ProfileView& frozen = s.profileView;
        const bool dragging = ProfileDrag (s.drag);
        const edit::ProfileView view =
            dragging ? frozen : edit::FitProfile (*path, min.x + gutter, min.y, width - gutter, height, margin);
        // The canvas clips its own drawing: a label that runs past an edge, or that the page has
        // scrolled out of the panel, is never drawn over the page beside it.
        list->PushClipRect (min, { min.x + width, min.y + height }, true);
        list->AddRectFilled (min, { min.x + width, min.y + height }, IM_COL32 (250, 251, 252, 255), 4.0f);
        const auto drawing = edit::DrawProfile (*path, 1.0);
        const auto screen = [&] (double station, double z) {
            double x = 0.0, y = 0.0;
            edit::ProfileToScreen (view, station, z, x, y);
            return ImVec2 { float (x), float (y) };
        };
        // The labels the canvas places, and the boxes they took. A label's ink reads along its
        // own box (Upright, never upside down), and every placed box is published for a click.
        std::vector<editlayers::Box> taken;
        const auto set = [&] (const editlayers::Box& box, const std::string& text, ImU32 ink,
                              const edit::Dimension& target) {
            if (s.profileTyping && target.ref == s.profileTarget.ref && target.quantity == s.profileTarget.quantity) {
                const bool editing = s.tracker.State () == edit::TrackerState::Editing;
                double value = 0;
                edit::ReadDimension (doc, target, value);
                char number[64];
                std::snprintf (number, sizeof number, "%.2f", value);
                const std::string entry = editing ? s.tracker.Text () : number;
                const float w = ImGui::CalcTextSize (entry.c_str ()).x + 14.0f, h = lineHeight + 6.0f;
                const float x = std::clamp ((box.x0 + box.x1 - w) * 0.5f, min.x, std::max (min.x, min.x + width - w));
                const float y = std::clamp (box.y0, min.y, min.y + height - h);
                list->AddRectFilled ({ x, y }, { x + w, y + h }, IM_COL32 (255, 248, 220, 255), 2);
                list->AddRect ({ x, y }, { x + w, y + h }, kAccent, 2, 0, 1.5f);
                if (!editing)
                    list->AddRectFilled ({ x + 3, y + 2 }, { x + w - 4, y + h - 2 }, IM_COL32 (62, 99, 221, 45));
                list->AddText ({ x + 4, y + 3 }, ink, entry.c_str ());
                if (std::fmod (ImGui::GetTime (), 1.0) < 0.6)
                    list->AddLine ({ x + w - 5, y + 3 }, { x + w - 5, y + h - 3 }, ink);
            }
            else
                Inked (list, box, text, ink);
            s.profilePlaced.push_back ({ box, target });
        };
        // The grid: about five round heights, thin and written in the gutter; round stations
        // across, fainter still.
        const float x0 = float (view.left), x1 = float (view.left + view.width);
        const float y0 = float (view.top), y1 = float (view.top + view.height);
        const double across = edit::NiceStep (view.stationMax - view.stationMin, 8);
        for (const double station : edit::Levels (view.stationMin, view.stationMax, across))
            list->AddLine ({ screen (station, 0).x, y0 }, { screen (station, 0).x, y1 }, kGridFaint, 1.0f);
        const double up = edit::NiceStep (view.zMax - view.zMin, 5);
        const int decimals = edit::StepDecimals (up);
        for (const double z : edit::Levels (view.zMin, view.zMax, up)) {
            const float y = screen (0, z).y;
            list->AddLine ({ x0, y }, { x1, y }, kGrid, 1.0f);
            char text[32];
            std::snprintf (text, sizeof text, "%.*f", decimals, z);
            const ImVec2 size = ImGui::CalcTextSize (text);
            list->AddText ({ x0 - 4.0f - size.x, y - size.y * 0.5f }, kGridText, text);
        }
        for (const auto& c : drawing.curves)
            list->AddRectFilled ({ screen (c.from, 0).x, min.y + 4 }, { screen (c.to, 0).x, min.y + height - 4 },
                                 IM_COL32 (62, 99, 221, 24));
        for (size_t i = 0; i + 1 < drawing.line.size (); ++i)
            list->AddLine (screen (drawing.line[i].first, drawing.line[i].second),
                           screen (drawing.line[i + 1].first, drawing.line[i + 1].second), kInk, 2.0f);
        for (size_t i = 0; i + 1 < drawing.breaks.size (); ++i) {
            const edit::ProfileDrawing::Marker& a = drawing.breaks[i];
            const edit::ProfileDrawing::Marker& b = drawing.breaks[i + 1];
            const ImVec2 pa = screen (a.station, a.z), pb = screen (b.station, b.z);
            float ux = pb.x - pa.x, uy = pb.y - pa.y;
            editlayers::Upright (ux, uy);
            const double percent = i < drawing.grades.size () ? drawing.grades[i].percent : 0.0;
            char text[24];
            std::snprintf (text, sizeof text, "%.1f %%", percent);
            const float w = ImGui::CalcTextSize (text).x + 8.0f, h = lineHeight + 2.0f;
            const ImVec2 mid { (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f };
            const ImVec2 n { -uy, ux };
            const auto box = editlayers::PlaceAlong (mid.x, mid.y, w, h, ux, uy, n.x, n.y, 8.0f + 0.5f * h, taken);
            set (box, text, kInk, { { path->id, edit::Part::Break, a.id }, edit::Quantity::Slope });
            // Water runs from the higher end to the lower: the arrow follows the fall.
            if (std::abs (percent) > 1.0e-9) {
                const ImVec2 from = screen (a.station + (b.station - a.station) * 0.25, a.z + (b.z - a.z) * 0.25);
                const ImVec2 to = screen (a.station + (b.station - a.station) * 0.8, a.z + (b.z - a.z) * 0.8);
                SlopeArrow (list, percent > 0.0 ? to : from, percent > 0.0 ? from : to, kInk, 1.0f);
            }
        }
        // Each interior break's smoothing nubs, where its curve meets its tangents, and a faint ring
        // through them: how far the rounding reaches. Hollow while the break is sharp.
        for (size_t i = 1; i + 1 < path->profile.size (); ++i) {
            std::pair<double, double> nubs[2];
            if (!edit::CurveNubs (*path, view, i, kNubPixels, nubs))
                continue;
            const auto& b = path->profile[i];
            const ImVec2 at = screen (b.station, b.z);
            const bool held = s.drag == Drag::ProfileCurve && s.profileIndex == i;
            const ImU32 colour = b.curve > 0.0 || held ? kAccent : kGridText;
            float reach = 0.0f;
            for (const auto& [station, z] : nubs) {
                const ImVec2 n = screen (station, z);
                reach = std::max (reach, std::hypot (n.x - at.x, n.y - at.y));
                if (b.curve > 0.0)
                    list->AddCircleFilled (n, 4.0f, colour);
                else
                    list->AddCircle (n, 4.0f, colour, 0, 1.5f);
            }
            list->AddCircle (at, reach, IM_COL32 (62, 99, 221, held ? 110 : 45), 48, 1.0f);
        }
        for (const auto& b : drawing.breaks) {
            const ImVec2 at = screen (b.station, b.z);
            const bool chosen = part != nullptr && part->part == edit::Part::Break && part->item == b.id;
            if (b.fixed)
                list->AddRectFilled ({ at.x - 5, at.y - 5 }, { at.x + 5, at.y + 5 }, kAxis[0]);
            else
                list->AddCircleFilled (at, 5.0f, kAxis[2]);
            if (chosen)
                list->AddCircle (at, 8.0f, kAccent, 0, 2.0f);
            // Its own elevation beside it, the way a grade break's height is read on the path.
            std::string text = editlayers::FormatValue (edit::Quantity::Elevation, b.z);
            if (b.fixed)
                text += " fixed";
            const float w = ImGui::CalcTextSize (text.c_str ()).x + 8.0f, h = lineHeight + 2.0f;
            const auto box = editlayers::PlaceLabel (at.x, at.y, w, h, 0.0f, -1.0f, h, taken);
            set (box, text, b.fixed ? kAxis[0] : kInk,
                 { { path->id, edit::Part::Break, b.id }, edit::Quantity::Elevation });
        }
        list->PopClipRect ();
        ImGui::TextDisabled ("Vertical scale x%.3g (linear)", view.exaggeration);
        // A click on a label binds it: the exact dimension, at this revision, for the existing
        // Tracker to type over. The pointer must really be on the canvas -- the item's own ImGui
        // hover, which an open popup, a covering window or the page's scroll clip all withdraw,
        // and inside the canvas' rectangle -- never a box alone, which a scrolled-away or
        // covered label could supply. Nothing else may own the input (a drag, the plane, a
        // typed handle, the view's own label).
        const ImVec2 mouse = ImGui::GetIO ().MousePos;
        const bool onCanvas =
            over && mouse.x >= min.x && mouse.x <= min.x + width && mouse.y >= min.y && mouse.y <= min.y + height;
        bool labelClick = false;
        const bool canBind = onCanvas && ImGui::IsMouseClicked (0) && s.drag == Drag::None && !s.edits.InGesture () &&
                             !s.penDown && s.drawing.empty () && !s.labelOpen && s.typedHandle < 0 && !s.planeTyping &&
                             s.tracker.State () != edit::TrackerState::Editing;
        if (canBind)
            for (auto it = s.profilePlaced.rbegin (); it != s.profilePlaced.rend (); ++it)
                if (LabelContains (it->box, mouse)) {
                    double value = 0.0;
                    edit::ReadDimension (doc, it->target, value);
                    s.selection.Set ({ it->target.ref });
                    s.profileTyping = true;
                    s.profileTarget = it->target;
                    s.profileRevision = doc.revision;
                    s.tracker.Hide ();
                    s.tracker.Show ({ { it->target.quantity, value } });
                    s.message.clear ();
                    labelClick = true;
                    break;
                }
        if (!labelClick && over && ImGui::IsMouseClicked (0) && s.drag == Drag::None) {
            const auto hits = edit::PickProfile (*path, view, mouse.x, mouse.y, 7.0, kNubPixels);
            if (!hits.empty () && s.edits.Begin ()) {
                s.profileTyping = false; // the drag owns the tracker now
                frozen = view;
                s.profileIndex = hits.front ().index;
                s.profileStartY = mouse.y;
                s.profileStartZ = path->profile[hits.front ().index].z;
                if (hits.front ().kind != edit::ProfileHit::Kind::Tangent) {
                    s.drag =
                        hits.front ().kind == edit::ProfileHit::Kind::Break ? Drag::ProfileBreak : Drag::ProfileCurve;
                    s.selection.Set ({ { path->id, edit::Part::Break, path->profile[s.profileIndex].id } });
                }
                else
                    s.drag = Drag::ProfileTangent;
            }
        }
        if (!labelClick && over && ImGui::IsMouseDoubleClicked (0) && s.drag == Drag::None) {
            s.profileTyping = false;
            double station = 0.0, z = 0.0;
            edit::ProfileFromScreen (view, mouse.x, mouse.y, station, z);
            const edit::Id id = path->id;
            if (!s.edits.Apply (
                    [=] (edit::Document& d, std::string& e) { return edit::InsertPathBreak (d, id, station, e); }))
                s.message = s.edits.Message ();
        }
        if (dragging && ImGui::IsMouseDown (0)) {
            const edit::Path* before = edit::FindPath (s.edits.Before (), path->id);
            double station = 0.0, z = 0.0;
            edit::ProfileFromScreen (frozen, mouse.x, mouse.y, station, z);
            edit::Operation op;
            if (before != nullptr && s.drag == Drag::ProfileBreak && s.profileIndex < before->profile.size ()) {
                std::string hint;
                edit::SnapBreak (*before, s.profileIndex, station, z, {}, hint);
                op = edit::DragBreak (before->id, before->profile[s.profileIndex].id, station, z);
                if (!hint.empty ())
                    s.message = "Grade " + hint;
            }
            else if (before != nullptr && s.drag == Drag::ProfileCurve && s.profileIndex < before->profile.size ()) {
                const double length = edit::CurveFromNub (*before, frozen, s.profileIndex, station, kNubPixels);
                op = edit::DragCurve (before->id, before->profile[s.profileIndex].id, length);
                char text[48];
                std::snprintf (text, sizeof text, "Smoothing %.2f m", length);
                s.message = text;
            }
            else if (before != nullptr) {
                double s0 = 0.0, z0 = 0.0;
                edit::ProfileFromScreen (frozen, mouse.x, s.profileStartY, s0, z0);
                op = edit::DragTangent (before->id, s.profileIndex, std::round ((z - z0) * 100.0) / 100.0);
            }
            if (op && !s.edits.Move (op))
                s.message = s.edits.Message ();
        }
        if (dragging && ImGui::IsMouseReleased (0)) {
            s.edits.End ();
            s.drag = Drag::None;
        }
        if (s.profileTyping)
            ImGui::TextDisabled ("Type the clicked label's value; Enter applies it, Esc gives it up.");
        else
            ImGui::TextDisabled (
                "Drag breaks and tangents, or a break's nubs to smooth it; double-click adds a break. Grades snap to "
                "0.5 %%.");
    }
    if (!s.message.empty ()) {
        ImGui::Separator ();
        ImGui::TextWrapped ("%s", s.message.c_str ());
    }
}

bool Follow (Session& s, overlaylayers::Layer& layer)
{
    PollTerrainDesign (s);
    const auto& doc = Doc (s);
    if (doc.landscape)
        Landscape (s);
    if (edit::FindGroup (doc, s.enteredGroup) == nullptr)
        s.enteredGroup = edit::kNoId; // ungrouped or undone since it was entered
    if (doc.revision == s.publishedRevision && s.selection.Items () == s.publishedSelection &&
        s.publishedPathStep == SurfaceStep (s) && s.quietPaths == s.publishedQuietPaths &&
        s.publishedGroup == s.enteredGroup && (!doc.landscape || s.publishedLandscapeZ == s.plane.origin.z))
        return false;
    edit::Selection shown = s.selection;
    shown.hover = {}; // hover is the view's transient feedback, not the layer's
    editlayers::Feedback f;
    f.labelsInView = true; // placed with the dimensions, clear of them (DrawPathLabels)
    f.selection = &shown;
    f.landscape = doc.landscape ? &s.landscape : nullptr;
    f.landscapeZ = s.plane.origin.z;
    f.paved = &PavedSurface (s);
    f.quietPaths = &s.quietPaths;
    f.contours = s.contours ? &Contours (s) : nullptr;
    // Inside a group, the rest of the scene fades so the group is what stands out.
    const std::vector<edit::Id> focus =
        s.enteredGroup != edit::kNoId ? edit::GroupMembers (doc, s.enteredGroup) : std::vector<edit::Id> {};
    f.focus = s.enteredGroup != edit::kNoId ? &focus : nullptr;
    s.publishedGroup = s.enteredGroup;
    // The live dimensions are the view's (HudVirtualEditorDraw.cpp): placed in pixels, they
    // can stand clear of the gizmo and the tracker, which model-anchored text cannot know.
    layer = editlayers::Build (doc, f);
    DrawTerrainDesign (s, layer);
    auto panel = hudshell::PlainLook ();
    panel.title = kPanelTitle;
    panel.widthPixels = 400;
    layer.panels.push_back (panel);
    s.publishedRevision = doc.revision;
    s.publishedPathStep = SurfaceStep (s);
    s.publishedSelection = s.selection.Items ();
    s.publishedQuietPaths = s.quietPaths;
    s.publishedLandscapeZ = s.plane.origin.z;
    return true;
}

} // namespace geomsrv::archviz::edithud
