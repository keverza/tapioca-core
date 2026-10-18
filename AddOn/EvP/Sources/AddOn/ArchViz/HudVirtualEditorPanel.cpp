#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathContours.hpp"
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
} // namespace

void Panel (Session& s)
{
    const auto& doc = Doc (s);
    ImGui::Text ("%zu objects   %zu paths   %zu selected", doc.entities.size (), doc.paths.size (),
                 s.selection.Items ().size ());
    if (ImGui::RadioButton ("Select (V)", s.tool == Tool::Select))
        s.tool = Tool::Select, s.drawing.clear ();
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Polyline (L)", s.tool == Tool::Polyline))
        s.tool = Tool::Polyline;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Path (P)", s.tool == Tool::Path))
        s.tool = Tool::Path;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Curve (B)", s.tool == Tool::Curve))
        s.tool = Tool::Curve;
    // The gumball holds applicable transforms; the same table gates individual modes.
    if (ImGui::RadioButton ("Gumball", s.gizmo == edit::GizmoMode::Gumball))
        s.gizmo = edit::GizmoMode::Gumball;
    const char* modes[] = { "Move (G)", "Rotate (R)", "Scale (S)", "Stretch (T)", "Extrude (E)" };
    for (int i = 0; i < 5; ++i) {
        ImGui::SameLine ();
        ImGui::BeginDisabled (!GizmoAvailable (s, edit::GizmoMode (i)));
        if (ImGui::RadioButton (modes[i], int (s.gizmo) == i))
            SetGizmoMode (s, edit::GizmoMode (i));
        ImGui::EndDisabled ();
    }
    // Snapping, in the groups a drafter thinks in.
    const auto group = [&] (const char* label, uint32_t bits) {
        bool on = (s.snapModes & bits) != 0;
        if (ImGui::Checkbox (label, &on))
            s.snapModes = on ? s.snapModes | bits : s.snapModes & ~bits;
    };
    ImGui::TextUnformatted ("Snap");
    ImGui::SameLine ();
    group ("Points", kSnapPoints);
    ImGui::SameLine ();
    group ("Edges", kSnapEdges);
    ImGui::SameLine ();
    group ("Guides", kSnapGuides);
    ImGui::SameLine ();
    ImGui::Checkbox ("Flush", &s.edgeSnap);
    hudshell::Tip ("A moved object's edges snap flush to parallel edges", hudshell::TipSide::Above);
    if ((s.snapModes & edit::kSnapAngle) != 0) {
        float step = float (s.angleStep);
        ImGui::SetNextItemWidth (120.0f);
        if (ImGui::SliderFloat ("Angle step", &step, 5.0f, 45.0f, "%.0f deg"))
            s.angleStep = std::round (step);
    }
    if (ImGui::RadioButton ("Local axes", s.localAxes))
        s.localAxes = true;
    ImGui::SameLine ();
    if (ImGui::RadioButton ("World axes", !s.localAxes))
        s.localAxes = false;
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
    ImGui::BeginDisabled (s.drag != Drag::None);
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
    static const std::vector<edit::Action> table = edit::PolylineActions ();
    const auto actions = edit::Applicable (table, s.selection, edit::ViewKind::Plan);
    for (size_t i = 0; i < actions.size (); ++i) {
        if (i > 0)
            ImGui::SameLine ();
        if (ImGui::SmallButton (actions[i]->label.c_str ())) {
            const auto op = actions[i]->make (doc, s.selection.Items ());
            const edit::Id since = doc.nextId;
            if (op && !s.edits.Apply (op))
                s.message = s.edits.Message ();
            SelectMade (s, since);
        }
    }
    ImGui::EndDisabled ();

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
        edit::ProfileView& frozen = s.profileView;
        const bool dragging = s.drag == Drag::ProfileBreak || s.drag == Drag::ProfileTangent;
        const edit::ProfileView view =
            dragging ? frozen : edit::FitProfile (*path, min.x + gutter, min.y, width - gutter, height, 14.0);
        list->AddRectFilled (min, { min.x + width, min.y + height }, IM_COL32 (250, 251, 252, 255), 4.0f);
        const auto drawing = edit::DrawProfile (*path, 1.0);
        const auto screen = [&] (double station, double z) {
            double x = 0.0, y = 0.0;
            edit::ProfileToScreen (view, station, z, x, y);
            return ImVec2 { float (x), float (y) };
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
        for (const auto& g : drawing.grades) {
            char text[24];
            std::snprintf (text, sizeof text, "%.1f %%", g.percent);
            const ImVec2 at = screen (g.station, g.z);
            list->AddText ({ at.x - 14, at.y - 18 }, kInk, text);
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
        }
        const ImVec2 mouse = ImGui::GetIO ().MousePos;
        if (over && ImGui::IsMouseClicked (0) && s.drag == Drag::None) {
            const auto hits = edit::PickProfile (*path, view, mouse.x, mouse.y, 7.0);
            if (!hits.empty () && s.edits.Begin ()) {
                frozen = view;
                s.profileIndex = hits.front ().index;
                s.profileStartY = mouse.y;
                s.profileStartZ = path->profile[hits.front ().index].z;
                if (hits.front ().kind == edit::ProfileHit::Kind::Break) {
                    s.drag = Drag::ProfileBreak;
                    s.selection.Set ({ { path->id, edit::Part::Break, path->profile[s.profileIndex].id } });
                }
                else
                    s.drag = Drag::ProfileTangent;
            }
        }
        if (over && ImGui::IsMouseDoubleClicked (0) && s.drag == Drag::None) {
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
        ImGui::TextDisabled ("Drag breaks and tangents; double-click adds a break. Grades snap to 0.5 %%.");
    }
    if (!s.message.empty ()) {
        ImGui::Separator ();
        ImGui::TextWrapped ("%s", s.message.c_str ());
    }
}

bool Follow (Session& s, overlaylayers::Layer& layer)
{
    const auto& doc = Doc (s);
    if (doc.revision == s.publishedRevision && s.selection.Items () == s.publishedSelection)
        return false;
    edit::Selection shown = s.selection;
    shown.hover = {}; // hover is the view's transient feedback, not the layer's
    editlayers::Feedback f;
    f.labelsInView = true; // placed with the dimensions, clear of them (DrawPathLabels)
    f.selection = &shown;
    f.contours = s.contours ? &Contours (s) : nullptr;
    // The live dimensions are the view's (HudVirtualEditorDraw.cpp): placed in pixels, they
    // can stand clear of the gizmo and the tracker, which model-anchored text cannot know.
    layer = editlayers::Build (doc, f);
    auto panel = hudshell::PlainLook ();
    panel.title = kPanelTitle;
    panel.widthPixels = 400;
    layer.panels.push_back (panel);
    s.publishedRevision = doc.revision;
    s.publishedSelection = s.selection.Items ();
    return true;
}

} // namespace geomsrv::archviz::edithud
