#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include <imgui.h>
#include <algorithm>

// The virtual editor's right-click menu: what was under the pointer decides what it
// offers. The items are data (MenuItems), so what each place offers is tested without
// ImGui; Menu only draws them and RunMenuItem does what one says.
namespace geomsrv::archviz::edithud {

namespace detail {

// The document actions the menu, the keys and the panel share.
void History (Session& s, bool undo)
{
    if (s.profileTyping) {
        s.message = "Apply the profile value with Enter or give it up with Esc before undoing.";
        return;
    }
    if (s.drag != Drag::None)
        return;
    if (!(undo ? s.edits.Undo () : s.edits.Redo ()))
        s.message = undo ? "Nothing to undo." : "Nothing to redo.";
    s.selection.Prune (Doc (s));
}

void DeleteSelection (Session& s)
{
    if (s.profileTyping) {
        s.message = "Apply the profile value with Enter or give it up with Esc before deleting.";
        return;
    }
    const auto items = s.selection.Items ();
    if (items.empty () || s.drag != Drag::None)
        return;
    const bool ok = s.edits.Apply ([items] (edit::Document& d, std::string& error) {
        for (const auto& r : items) {
            if (edit::FindPath (d, r.entity) != nullptr) {
                bool done = true;
                switch (r.part) {
                    case edit::Part::Entity:
                        d.paths.erase (std::remove_if (d.paths.begin (), d.paths.end (),
                                                       [&] (const edit::Path& p) { return p.id == r.entity; }),
                                       d.paths.end ());
                        break;
                    case edit::Part::PathPoint:
                        done = edit::DeletePathPoint (d, r.entity, r.item, error);
                        break;
                    case edit::Part::Break:
                        done = edit::DeletePathBreak (d, r.entity, r.item, error);
                        break;
                    case edit::Part::Range:
                        done = edit::DeletePathSection (d, r.entity, r.item, error);
                        break;
                    default:
                        break;
                }
                if (!done)
                    return false;
                continue;
            }
            if (r.part == edit::Part::Entity)
                d.entities.erase (std::remove_if (d.entities.begin (), d.entities.end (),
                                                  [&] (const edit::Entity& e) { return e.id == r.entity; }),
                                  d.entities.end ());
            else if (r.part == edit::Part::Vertex) {
                if (!edit::DeleteVertex (d, r.entity, r.item, error))
                    return false;
            }
            else {
                error = "Delete removes vertices, path parts or whole objects.";
                return false;
            }
        }
        return true;
    });
    if (ok)
        s.selection.Clear ();
    else
        s.message = s.edits.Message ();
}
} // namespace detail

void detail::SelectMade (Session& s, edit::Id since)
{
    const auto& doc = Doc (s);
    s.selection.Prune (doc);
    if (!s.selection.Items ().empty ())
        return;
    std::vector<edit::Ref> made;
    for (const auto& p : doc.paths)
        if (p.id >= since)
            made.push_back ({ p.id, edit::Part::Entity, edit::kNoId });
    for (const auto& e : doc.entities)
        if (e.id >= since)
            made.push_back ({ e.id, edit::Part::Entity, edit::kNoId });
    if (!made.empty ())
        s.selection.Set (made);
}

namespace {

using namespace detail;

constexpr char kMenuId[] = "##virtualEditorMenu";

MenuItem Item (std::string id, std::string label, std::string shortcut = {}, bool enabled = true, bool checked = false)
{
    MenuItem m;
    m.id = std::move (id);
    m.label = std::move (label);
    m.shortcut = std::move (shortcut);
    m.enabled = enabled;
    m.checked = checked;
    return m;
}

void Rule (std::vector<MenuItem>& items)
{
    if (!items.empty ())
        items.back ().ruleAfter = true;
}

const std::vector<edit::Action>& Actions ()
{
    static const std::vector<edit::Action> table = edit::PolylineActions ();
    return table;
}

// The polyline actions that apply to the selection, minus those this menu says better.
void AddActions (const Session& s, std::vector<MenuItem>& items, std::initializer_list<const char*> skip)
{
    bool any = false;
    for (const edit::Action* a : edit::Applicable (Actions (), s.selection, edit::ViewKind::Plan)) {
        if (std::any_of (skip.begin (), skip.end (), [&] (const char* id) { return a->id == id; }))
            continue;
        if (!any)
            Rule (items);
        any = true;
        // An action with nothing to do here (a sharp corner made sharp) is shown, greyed.
        items.push_back (Item ("action." + a->id, a->label, {}, bool (a->make (Doc (s), s.selection.Items ()))));
    }
}

void AddHistory (const Session& s, std::vector<MenuItem>& items)
{
    Rule (items);
    const bool idle = s.drag == Drag::None;
    items.push_back (Item ("undo", "Undo", "Ctrl+Z", idle && s.edits.CanUndo ()));
    items.push_back (Item ("redo", "Redo", "Ctrl+Y", idle && s.edits.CanRedo ()));
}

const edit::Path* MenuPath (const Session& s)
{
    return edit::FindPath (Doc (s), s.menu.ref.entity);
}

// The station of the place the menu was opened at, on its path.
bool MenuStation (const Session& s, double& station)
{
    const edit::Path* path = MenuPath (s);
    edit::AlignmentGeometry g;
    std::string why;
    double offset = 0.0;
    if (path == nullptr || !edit::BuildAlignment (path->points, g, why))
        return false;
    if (s.menu.ref.part == edit::Part::PathPoint) {
        for (size_t i = 0; i < path->points.size (); ++i)
            if (path->points[i].id == s.menu.ref.item) {
                station = g.pointStations[i];
                return true;
            }
        return false;
    }
    if (s.menu.ref.part == edit::Part::Break) {
        for (const auto& b : path->profile)
            if (b.id == s.menu.ref.item) {
                station = b.station;
                return true;
            }
        return false;
    }
    return edit::StationOf (g, s.menu.at, station, offset);
}

template <class F> void ApplyPath (Session& s, F change)
{
    const edit::Id id = s.menu.ref.entity;
    double station = 0.0;
    if (!MenuStation (s, station))
        return;
    if (!s.edits.Apply ([=] (edit::Document& d, std::string& e) { return change (d, id, station, e); }))
        s.message = s.edits.Message ();
}

} // namespace

std::vector<MenuItem> MenuItems (const Session& s)
{
    std::vector<MenuItem> items;
    const edit::Document& doc = Doc (s);
    switch (s.menu.target) {
        case MenuTarget::Drawing: {
            const size_t n = s.drawing.size ();
            items.push_back (Item ("draw.finish", "Finish", "Enter", n >= 2));
            if (s.tool == Tool::Polyline || s.tool == Tool::Curve)
                items.push_back (Item ("draw.close", "Close outline", {}, n >= 3));
            items.push_back (Item ("draw.back", "Remove last point", "Backspace", n >= 1));
            items.push_back (Item ("draw.cancel", "Cancel drawing", "Esc"));
            break;
        }
        case MenuTarget::Vertex:
            AddActions (s, items, {});
            break;
        case MenuTarget::Segment:
            items.push_back (Item ("segment.insert", "Insert vertex here"));
            items.push_back (Item ("segment.flip", "Keep the other end fixed", "F"));
            AddActions (s, items, { "insert-vertex" });
            break;
        case MenuTarget::Object: {
            const edit::Entity* e = edit::Find (doc, s.menu.ref.entity);
            items.push_back (Item ("delete", "Delete object", "Del"));
            if (s.menu.ref.part == edit::Part::Face)
                items.push_back (Item ("plane.face", "Drawing plane at selected face"));
            Rule (items);
            const char* modes[] = { "Move", "Rotate", "Scale", "Stretch", "Push/pull face" };
            const char* keys[] = { "G", "R", "S", "T", "E" };
            items.push_back (Item ("gizmo.5", "Gumball", {}, true, s.gizmo == edit::GizmoMode::Gumball));
            for (int i = 0; i < 5; ++i)
                if (i < 4 || (e != nullptr && e->kind == edit::Kind::Extrusion))
                    items.push_back (Item ("gizmo." + std::to_string (i), modes[i], keys[i],
                                           GizmoAvailable (s, edit::GizmoMode (i)), int (s.gizmo) == i));
            items.push_back (Item ("gizmo.local", "Axes along the shape", {}, true, s.localAxes));
            if (e != nullptr && e->kind == edit::Kind::Polyline && !e->closed)
                AddActions (s, items, {});
            else
                AddActions (s, items, { "make-path" }); // a path has two ends
            break;
        }
        case MenuTarget::Path: {
            const edit::Path* p = MenuPath (s);
            bool all[3] = { p != nullptr, p != nullptr, p != nullptr }, sided = false;
            if (p != nullptr)
                for (const auto& r : p->sections) {
                    for (int i = 0; i < 3; ++i)
                        all[i] = all[i] && int (r.fall) == i;
                    sided = sided || r.fall != edit::Fall::Crown;
                }
            items.push_back (Item ("fall.crown", "Crown", {}, p != nullptr, all[0]));
            items.push_back (Item ("fall.left", "Falls left", {}, p != nullptr, all[1]));
            items.push_back (Item ("fall.right", "Falls right", {}, p != nullptr, all[2]));
            items.push_back (Item ("fall.flip", "Flip fall", {}, sided));
            Rule (items);
            items.push_back (Item ("path.insertPoint", "Insert control point here"));
            items.push_back (Item ("path.insertBreak", "Add grade break here"));
            items.push_back (Item ("path.splitRange", "Split cross-section here"));
            items.push_back (Item ("path.split", "Split path here"));
            Rule (items);
            items.push_back (Item ("delete", "Delete path", "Del"));
            break;
        }
        case MenuTarget::PathPoint: {
            const edit::Path* p = MenuPath (s);
            items.push_back (Item ("delete", "Delete control point", "Del", p != nullptr && p->points.size () > 2));
            items.push_back (Item ("path.insertBreak", "Add grade break here"));
            items.push_back (Item ("path.split", "Split path at this point"));
            break;
        }
        case MenuTarget::GradeBreak: {
            const edit::Path* p = MenuPath (s);
            bool fixed = false;
            if (p != nullptr)
                for (const auto& b : p->profile)
                    fixed = fixed || (b.id == s.menu.ref.item && b.fixed);
            items.push_back (fixed ? Item ("break.unfix", "Release elevation") : Item ("break.fix", "Fix elevation"));
            items.push_back (Item ("delete", "Delete grade break", "Del", p != nullptr && p->profile.size () > 2));
            items.push_back (Item ("path.split", "Split path at this point"));
            break;
        }
        case MenuTarget::Range: {
            const edit::Path* p = MenuPath (s);
            const edit::Section* r = nullptr;
            if (p != nullptr)
                for (const auto& x : p->sections)
                    if (x.id == s.menu.ref.item)
                        r = &x;
            items.push_back (Item ("fall.crown", "Crown", {}, r != nullptr, r && r->fall == edit::Fall::Crown));
            items.push_back (Item ("fall.left", "Falls left", {}, r != nullptr, r && r->fall == edit::Fall::Left));
            items.push_back (Item ("fall.right", "Falls right", {}, r != nullptr, r && r->fall == edit::Fall::Right));
            items.push_back (Item ("fall.flip", "Flip fall", {}, r != nullptr && r->fall != edit::Fall::Crown));
            Rule (items);
            items.push_back (Item ("path.splitRange", "Split cross-section here"));
            items.push_back (Item ("path.split", "Split path here"));
            items.push_back (Item ("delete", "Delete cross-section", "Del", p != nullptr && p->sections.size () > 1));
            break;
        }
        case MenuTarget::View:
            items.push_back (Item ("tool.select", "Select", "V", true, s.tool == Tool::Select));
            items.push_back (Item ("tool.polyline", "Draw polyline", "L", true, s.tool == Tool::Polyline));
            items.push_back (Item ("tool.path", "Draw path", "P", true, s.tool == Tool::Path));
            items.push_back (Item ("tool.curve", "Draw curves (pen)", "B", true, s.tool == Tool::Curve));
            Rule (items);
            items.push_back (Item ("select.all", "Select all", {}, !doc.entities.empty () || !doc.paths.empty ()));
            items.push_back (Item ("select.none", "Deselect", "Esc", !s.selection.Items ().empty ()));
            Rule (items);
            items.push_back (Item ("snap.enabled", "Snapping enabled", {}, true, s.snapEnabled));
            items.push_back (Item ("snap.points", "Snap to points", {}, true, (s.snapModes & kSnapPoints) != 0));
            items.push_back (Item ("snap.edges", "Snap to edges", {}, true, (s.snapModes & kSnapEdges) != 0));
            items.push_back (
                Item ("snap.guides", "Axes, angles and parallels", {}, true, (s.snapModes & kSnapGuides) != 0));
            items.push_back (Item ("snap.flush", "Flush edges near pointer", {}, true, s.edgeSnap));
            break;
    }
    if (edit::FindPath (doc, s.menu.ref.entity) != nullptr && s.menu.target != MenuTarget::Drawing) {
        Rule (items);
        if (s.menu.ref.part == edit::Part::PathPoint || s.menu.ref.part == edit::Part::Break ||
            s.menu.ref.part == edit::Part::Range) {
            const char* label = s.menu.ref.part == edit::Part::Range   ? "Drawing plane at range start"
                                : s.menu.ref.part == edit::Part::Break ? "Drawing plane at grade point"
                                                                       : "Drawing plane at control point";
            items.push_back (Item ("plane.pathPart", label));
        }
        items.push_back (Item ("plane.pathStation", "Horizontal drawing plane along path..."));
    }
    AddHistory (s, items);
    if (s.menu.target == MenuTarget::View) {
        Rule (items);
        items.push_back (Item ("close", "Close editor"));
    }
    return items;
}

void RunMenuItem (Session& s, const std::string& id)
{
    const edit::Document& doc = Doc (s);
    if (id == "undo" || id == "redo")
        History (s, id == "undo");
    else if (id == "delete")
        DeleteSelection (s);
    else if (id == "close")
        s.close = true;
    else if (id == "plane.face")
        UseFacePlane (s, s.menu.ref);
    else if (id == "plane.pathPart")
        UsePathPartPlane (s, s.menu.ref);
    else if (id == "plane.pathStation")
        BeginPathPlaneEntry (s, s.menu.ref.entity);
    else if (id == "tool.select")
        s.tool = Tool::Select, s.drawing.clear ();
    else if (id == "tool.polyline")
        s.tool = Tool::Polyline;
    else if (id == "tool.path")
        s.tool = Tool::Path;
    else if (id == "tool.curve")
        s.tool = Tool::Curve;
    else if (id == "select.none")
        s.selection.Clear ();
    else if (id == "select.all") {
        std::vector<edit::Ref> all;
        for (const auto& e : doc.entities)
            all.push_back ({ e.id, edit::Part::Entity, 0 });
        for (const auto& p : doc.paths)
            all.push_back ({ p.id, edit::Part::Entity, 0 });
        s.selection.Set (all);
    }
    else if (id == "draw.finish")
        FinishDrawing (s);
    else if (id == "draw.close" && s.drawing.size () >= 3) {
        s.drawing.push_back (s.drawing.front ());
        FinishDrawing (s);
    }
    else if (id == "draw.back" && !s.drawing.empty ())
        s.drawing.pop_back ();
    else if (id == "draw.cancel")
        s.drawing.clear ();
    else if (id == "snap.enabled")
        s.snapEnabled = !s.snapEnabled;
    else if (id == "snap.points" || id == "snap.edges" || id == "snap.guides") {
        const uint32_t group = id == "snap.points" ? kSnapPoints : id == "snap.edges" ? kSnapEdges : kSnapGuides;
        s.snapModes = (s.snapModes & group) != 0 ? s.snapModes & ~group : s.snapModes | group;
    }
    else if (id == "snap.flush")
        s.edgeSnap = !s.edgeSnap;
    else if (id == "segment.flip")
        s.fromEnd = !s.fromEnd;
    else if (id == "gizmo.local")
        s.localAxes = !s.localAxes;
    else if (id.rfind ("gizmo.", 0) == 0)
        SetGizmoMode (s, edit::GizmoMode (std::stoi (id.substr (6))));
    else if (id == "segment.insert") {
        const edit::Ref r = s.menu.ref;
        const edit::Vec3 at = s.menu.at;
        if (!s.edits.Apply ([=] (edit::Document& d, std::string& e) {
                const edit::Entity* entity = edit::Find (d, r.entity);
                const int i = entity != nullptr ? edit::SegmentIndex (*entity, r.item) : -1;
                if (i < 0)
                    return false;
                // On the segment, under the pointer: its nearest point, at the segment's height.
                size_t from = 0, to = 0;
                edit::SegmentEnds (*entity, size_t (i), from, to);
                const edit::Vec3 a = entity->vertices[from].p, b = entity->vertices[to].p;
                const edit::Vec3 ab = b - a;
                const double len2 = edit::Dot (ab, ab);
                const double t = len2 > 0.0 ? std::clamp (edit::Dot (at - a, ab) / len2, 0.05, 0.95) : 0.5;
                return edit::InsertVertex (d, r.entity, r.item, edit::Lerp (a, b, t), e);
            }))
            s.message = s.edits.Message ();
    }
    else if (id.rfind ("action.", 0) == 0) {
        for (const auto& a : Actions ())
            if ("action." + a.id == id) {
                const auto op = a.make (doc, s.selection.Items ());
                const edit::Id since = doc.nextId;
                if (op && !s.edits.Apply (op))
                    s.message = s.edits.Message ();
                SelectMade (s, since);
                break;
            }
    }
    else if (id == "path.insertPoint")
        ApplyPath (s, [] (edit::Document& d, edit::Id p, double st, std::string& e) {
            return edit::InsertPathPoint (d, p, st, e);
        });
    else if (id == "path.insertBreak")
        ApplyPath (s, [] (edit::Document& d, edit::Id p, double st, std::string& e) {
            return edit::InsertPathBreak (d, p, st, e);
        });
    else if (id == "path.splitRange")
        ApplyPath (s, [] (edit::Document& d, edit::Id p, double st, std::string& e) {
            return edit::SplitPathSection (d, p, st, e);
        });
    else if (id == "path.split") {
        double station = 0;
        if (!MenuStation (s, station)) {
            s.message = "The path split point could not be resolved.";
            return;
        }
        const edit::Id path = s.menu.ref.entity;
        edit::Id created = edit::kNoId;
        // End must not immediately rejoin the two deliberately separated ends.
        if (!s.edits.Apply (
                [&created, path, station] (edit::Document& d, std::string& e) {
                    return edit::SplitPath (d, path, station, e, &created);
                },
                false))
            s.message = s.edits.Message ();
        else {
            s.selection.Set ({ { path, edit::Part::Entity, 0 }, { created, edit::Part::Entity, 0 } });
            s.message.clear ();
        }
    }
    else if (id == "break.fix" || id == "break.unfix") {
        const edit::Path* p = MenuPath (s);
        if (p == nullptr)
            return;
        for (const auto& b : p->profile)
            if (b.id == s.menu.ref.item) {
                const edit::Id path = p->id, bid = b.id;
                const double station = b.station, z = b.z;
                const bool fix = id == "break.fix";
                if (!s.edits.Apply ([=] (edit::Document& d, std::string& e) {
                        return fix ? edit::FixElevationAt (d, path, station, z, "", e)
                                   : edit::UnfixBreak (d, path, bid, e);
                    }))
                    s.message = s.edits.Message ();
                break;
            }
    }
    else if (id.rfind ("fall.", 0) == 0) {
        const edit::Path* p = MenuPath (s);
        if (p == nullptr)
            return;
        // A range's own, from its edge; every range's, from the centreline.
        const edit::Id path = p->id, range = s.menu.target == MenuTarget::Range ? s.menu.ref.item : edit::kNoId;
        const edit::Fall fall = id == "fall.left"    ? edit::Fall::Left
                                : id == "fall.right" ? edit::Fall::Right
                                                     : edit::Fall::Crown;
        const bool flip = id == "fall.flip";
        if (!s.edits.Apply ([=] (edit::Document& d, std::string& e) {
                return flip ? edit::FlipPathFall (d, path, range, e) : edit::SetPathFall (d, path, range, fall, e);
            }))
            s.message = s.edits.Message ();
    }
}

void OpenMenu (Session& s, MenuTarget target, edit::Ref ref, edit::Vec3 at)
{
    s.menu.target = target;
    s.menu.ref = ref;
    s.menu.at = at;
    s.menu.open = true;
    hudshell::ClaimRightClick (); // the HUD's own menu is for its background, not the editor's
    ImGui::OpenPopup (kMenuId);
}

void Menu (Session& s)
{
    if (!s.menu.open)
        return;
    if (ImGui::BeginPopup (kMenuId, ImGuiWindowFlags_NoMove)) {
        for (const auto& item : MenuItems (s)) {
            if (ImGui::MenuItem (item.label.c_str (), item.shortcut.empty () ? nullptr : item.shortcut.c_str (),
                                 item.checked, item.enabled))
                RunMenuItem (s, item.id);
            if (item.ruleAfter)
                ImGui::Separator ();
        }
        ImGui::EndPopup ();
    }
    s.menu.open = ImGui::IsPopupOpen (kMenuId);
}

} // namespace geomsrv::archviz::edithud
