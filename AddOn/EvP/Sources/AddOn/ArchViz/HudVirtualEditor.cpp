#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "Edit/EditPathEdit.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::edithud {

namespace detail {

using edit::Vec3;
namespace rc = roofcanvas;

const edit::Document& Doc (const Session& s)
{
    return s.edits.Doc ();
}

void History (Session& s, bool undo)
{
    if (s.drag != Drag::None)
        return;
    if (!(undo ? s.edits.Undo () : s.edits.Redo ()))
        s.message = undo ? "Nothing to undo." : "Nothing to redo.";
    s.selection.Prune (Doc (s));
}

void DeleteSelection (Session& s)
{
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

bool Map::Screen (Vec3 p, ImVec2& out) const
{
    float x = 0.0f, y = 0.0f;
    if (v.planar) {
        x = float (v.plan[0] * p.x + v.plan[1] * p.y + v.plan[2]);
        y = float (v.plan[3] * p.x + v.plan[4] * p.y + v.plan[5]);
    }
    else if (!v.project || !v.project (p.x, p.y, p.z, x, y))
        return false;
    out = { x, y };
    return std::isfinite (x) && std::isfinite (y);
}

// The point at height `z` that this pixel shows. The plan inverts its affine
// transform; 3D refines a seed by Newton steps through the projection and refuses a
// plane seen edge-on.
bool Map::Onto (float px, float py, double z, Vec3 seed, Vec3& out) const
{
    if (v.planar) {
        const double det = v.plan[0] * v.plan[4] - v.plan[1] * v.plan[3];
        if (std::abs (det) < 1.0e-12)
            return false;
        const double dx = px - v.plan[2], dy = py - v.plan[5];
        out = { (dx * v.plan[4] - v.plan[1] * dy) / det, (v.plan[0] * dy - v.plan[3] * dx) / det, z };
        return true;
    }
    Vec3 at { seed.x, seed.y, z };
    for (int iteration = 0; iteration < 12; ++iteration) {
        ImVec2 p, x, y;
        constexpr double step = 0.1;
        if (!Screen (at, p) || !Screen (at + Vec3 { step, 0.0, 0.0 }, x) || !Screen (at + Vec3 { 0.0, step, 0.0 }, y))
            return false;
        const double dx = px - p.x, dy = py - p.y;
        if (std::hypot (dx, dy) < 0.05) {
            out = at;
            return true;
        }
        const double xx = (x.x - p.x) / step, xy = (x.y - p.y) / step, yx = (y.x - p.x) / step, yy = (y.y - p.y) / step;
        const double det = xx * yy - xy * yx;
        if (std::abs (det) < 1.0e-6)
            return false;
        at.x += (dx * yy - dy * yx) / det;
        at.y += (dy * xx - dx * xy) / det;
        if (!Finite (at))
            return false;
    }
    return false;
}

// A pick ray through the pixel, towards the scene.
bool Map::RayAt (float px, float py, double z, Vec3 seed, edit::Ray& ray) const
{
    Vec3 low, high;
    if (!Onto (px, py, z, seed, low))
        return false;
    if (v.planar) {
        ray = { low + Vec3 { 0.0, 0.0, 1000.0 }, { 0.0, 0.0, -1.0 } };
        return true;
    }
    if (!Onto (px, py, z + 1.0, low, high))
        return false;
    Vec3 direction = low - high;
    if (v.hasCameraDirection &&
        Dot (direction, Vec3 { v.cameraDirection[0], v.cameraDirection[1], v.cameraDirection[2] }) < 0.0)
        direction = direction * -1.0;
    if (!Normalize (direction, direction))
        return false;
    ray = { low - direction * 1000.0, direction };
    return true;
}

double Map::MetresPerPixel (float px, float py, double z, Vec3 seed) const
{
    Vec3 a, b;
    if (!Onto (px, py, z, seed, a) || !Onto (px + 1.0f, py, z, a, b))
        return 0.01;
    const double d = Distance (a, b);
    return d > 1.0e-9 && std::isfinite (d) ? d : 0.01;
}

edit::PickView Map::Pick () const
{
    edit::PickView view;
    view.toScreen = [this] (Vec3 p, double& x, double& y) {
        ImVec2 s;
        if (!Screen (p, s))
            return false;
        x = s.x;
        y = s.y;
        return true;
    };
    if (!v.planar && v.hasCameraDirection)
        view.viewDirection = { v.cameraDirection[0], v.cameraDirection[1], v.cameraDirection[2] };
    return view;
}

rc::Anchor At (Vec3 p, ImVec2 s)
{
    rc::Anchor a;
    a.model = { p.x, p.y, p.z };
    a.screen = { s.x, s.y };
    return a;
}

void Line (Session& s, const Map& m, Vec3 a, Vec3 b, ImU32 colour, float width)
{
    ImVec2 pa, pb;
    if (!m.Screen (a, pa) || !m.Screen (b, pb))
        return;
    rc::Scope scope (s.canvas, { At (a, pa), At (b, pb) });
    s.canvas.list->AddLine (pa, pb, colour, width);
}

void Dot (Session& s, const Map& m, Vec3 p, float radius, ImU32 colour, bool filled)
{
    ImVec2 q;
    if (!m.Screen (p, q))
        return;
    rc::Scope scope (s.canvas, { At (p, q) });
    if (filled)
        s.canvas.list->AddCircleFilled (q, radius, colour);
    else
        s.canvas.list->AddCircle (q, radius, colour, 0, 2.0f);
}

void Note (Session& s, const Map& m, Vec3 p, ImVec2 offset, const std::string& text, ImU32 colour)
{
    ImVec2 q;
    if (text.empty () || !m.Screen (p, q))
        return;
    rc::Scope scope (s.canvas, { At (p, q) });
    const ImVec2 at { q.x + offset.x, q.y + offset.y };
    const ImVec2 size = ImGui::CalcTextSize (text.c_str ());
    s.canvas.list->AddRectFilled ({ at.x - 3, at.y - 2 }, { at.x + size.x + 3, at.y + size.y + 2 }, kPaper, 3.0f);
    s.canvas.list->AddText (at, colour, text.c_str ());
}

void FinishDrawing (Session& s)
{
    if (s.drawing.size () < 2) {
        s.drawing.clear ();
        return;
    }
    auto points = s.drawing;
    s.drawing.clear ();
    const bool closed = points.size () > 3 && edit::Distance (points.front (), points.back ()) < 1.0e-6;
    if (closed)
        points.pop_back ();
    edit::Id created = edit::kNoId;
    const Tool tool = s.tool;
    const double width = s.pathWidth;
    const std::string role = s.role;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string&) {
        created =
            tool == Tool::Path ? edit::AddPath (d, points, width, role) : edit::AddPolyline (d, points, closed, role);
        return true;
    });
    if (!ok) {
        s.message = s.edits.Message ();
        return;
    }
    s.selection.Set ({ { created, edit::Part::Entity, 0 } });
    s.message.clear ();
}

} // namespace detail

namespace {

using namespace detail;

using edit::Vec3;
namespace rc = roofcanvas;

constexpr float kPickPixels = 8.0f, kGizmoPixels = 70.0f, kSnapPixels = 10.0f;

// The items a press on `ref` moves: the selection when it holds `ref`, otherwise `ref`.
std::vector<edit::Ref> Moving (const Session& s, const edit::Ref& ref)
{
    return s.selection.Contains (ref) ? s.selection.Items () : std::vector<edit::Ref> { ref };
}

// What a moving item must not snap to: itself and everything that moves with it.
std::vector<edit::Ref> Exclusions (const edit::Document& doc, const std::vector<edit::Ref>& refs)
{
    std::vector<edit::Ref> out;
    for (const auto& r : refs) {
        if (edit::FindPath (doc, r.entity) != nullptr || r.part == edit::Part::Entity || r.part == edit::Part::Face) {
            out.push_back ({ r.entity, edit::Part::Entity, 0 });
            continue;
        }
        const edit::Entity* e = edit::Find (doc, r.entity);
        if (e == nullptr)
            continue;
        if (r.part == edit::Part::Vertex)
            out.push_back (r);
        else if (r.part == edit::Part::Segment) {
            const int i = edit::SegmentIndex (*e, r.item);
            if (i < 0)
                continue;
            size_t from = 0, to = 0;
            edit::SegmentEnds (*e, size_t (i), from, to);
            out.push_back ({ e->id, edit::Part::Vertex, e->vertices[from].id });
            out.push_back ({ e->id, edit::Part::Vertex, e->vertices[to].id });
        }
    }
    return out;
}

bool Centre (const Session& s, Vec3& centre)
{
    const auto& items = s.selection.Items ();
    Vec3 sum;
    size_t n = 0;
    for (const auto& r : items) {
        Vec3 p;
        if (edit::PointOf (Doc (s), r, p))
            sum = sum + p, ++n;
    }
    if (n == 0)
        return false;
    centre = sum * (1.0 / double (n));
    return true;
}

bool GizmoShown (const Session& s)
{
    return s.tool == Tool::Select && !s.selection.Items ().empty () &&
           s.selection.Kind () != edit::SelectionKind::Range && s.selection.Kind () != edit::SelectionKind::Break;
}

// The point the pointer is on, snapped, at height z.
bool Pointer (Session& s, const Map& m, ImVec2 mouse, double z, Vec3 seed, std::optional<Vec3> anchor,
              const std::vector<edit::Ref>& exclude, float scale, Vec3& out)
{
    // A drawing tool snaps to its own unfinished points too, so a line closes on its start.
    edit::SnapContext context = s.context;
    for (const auto& p : s.drawing)
        context.points.push_back ({ p, "this line" });
    Vec3 cursor;
    if (!m.Onto (mouse.x, mouse.y, z, seed, cursor))
        return false;
    edit::SnapQuery q;
    q.cursor = cursor;
    q.tolerance = kSnapPixels * scale * m.MetresPerPixel (mouse.x, mouse.y, z, cursor);
    q.anchor = anchor;
    q.plane.origin = { 0.0, 0.0, z };
    q.exclude = exclude;
    s.snap = edit::Snap (Doc (s), context, q);
    out = s.snap.point;
    return true;
}

void Escape (Session& s)
{
    if (s.tracker.State () == edit::TrackerState::Editing)
        s.tracker.Key ({ edit::TrackerKey::Kind::Escape });
    else if (s.drag != Drag::None) {
        s.edits.Cancel ();
        s.drag = Drag::None;
        s.snap = {};
    }
    else if (!s.drawing.empty ())
        s.drawing.clear ();
    else if (!s.selection.Items ().empty ())
        s.selection.Clear ();
    else
        s.close = true;
}

void TrackerEvent (Session& s, const edit::Tracker::Event& event)
{
    using O = edit::Tracker::Outcome;
    if (event.outcome == O::CancelGesture) {
        Escape (s);
        return;
    }
    if (event.outcome == O::Invalid) {
        s.message = "Type a plain number; Enter applies, Esc cancels.";
        return;
    }
    if (event.outcome != O::Commit)
        return;
    if (!s.drawing.empty ()) {
        // A typed length places the next point along the pointer's current direction.
        const Vec3 last = s.drawing.back ();
        Vec3 dir;
        const Vec3 toward = s.snap.snapped ? s.snap.point : last + Vec3 { 1.0, 0.0, 0.0 };
        if (!edit::Normalize (Vec3 { toward.x - last.x, toward.y - last.y, 0.0 }, dir))
            dir = { 1.0, 0.0, 0.0 };
        if (const auto angle = s.tracker.Locked (edit::Quantity::Angle))
            dir = { std::cos (*angle * 3.14159265358979323846 / 180.0),
                    std::sin (*angle * 3.14159265358979323846 / 180.0), 0.0 };
        if (s.tracker.Items ()[event.item].quantity == edit::Quantity::Length && event.value > 0.0)
            s.drawing.push_back (last + dir * event.value);
        return;
    }
    std::vector<edit::Dimension> dims;
    TrackerFields (s, dims);
    if (event.item >= dims.size ())
        return;
    if (!s.edits.Apply (edit::WriteDimension (dims[event.item], event.value)))
        s.message = s.edits.Message ();
    else
        s.message.clear ();
}

} // namespace

std::vector<edit::TrackerItem> TrackerFields (const Session& s, std::vector<edit::Dimension>& dims)
{
    std::vector<edit::TrackerItem> items;
    dims.clear ();
    if (!s.drawing.empty ()) {
        const Vec3 last = s.drawing.back (), at = s.snap.point;
        items.push_back ({ edit::Quantity::Length, edit::DistanceXY (last, at) });
        items.push_back (
            { edit::Quantity::Angle, std::atan2 (at.y - last.y, at.x - last.x) * 180.0 / 3.14159265358979323846 });
        return items;
    }
    if (s.selection.Items ().empty ())
        return items;
    dims = edit::DimensionsFor (Doc (s), s.selection.Items ().front ());
    for (const auto& d : dims) {
        double value = 0.0;
        edit::ReadDimension (Doc (s), d, value);
        items.push_back ({ d.quantity, value });
    }
    return items;
}

edit::Frame GizmoFrame (const Session& s)
{
    edit::Frame f;
    Centre (s, f.origin);
    const auto& items = s.selection.Items ();
    if (s.pivot && s.pivotFor == items)
        f.origin = *s.pivot;
    if (s.gizmo == edit::GizmoMode::Extrude && items.size () == 1 && items[0].part == edit::Part::Face)
        if (const edit::Entity* e = edit::Find (Doc (s), items[0].entity)) {
            edit::FaceFrame (*e, items[0].item, f);
            return f;
        }
    if (s.localAxes) {
        const edit::Frame local = edit::SelectionFrame (Doc (s), items, f.origin);
        for (int i = 0; i < 3; ++i)
            f.axes[i] = local.axes[i];
    }
    return f;
}

const edit::Path* ProfilePath (const Session& s)
{
    const auto& doc = Doc (s);
    for (const auto& r : s.selection.Items ())
        if (const edit::Path* p = edit::FindPath (doc, r.entity))
            return p;
    if (const edit::Path* p = edit::FindPath (doc, s.profilePath))
        return p;
    return doc.paths.size () == 1 ? &doc.paths.front () : nullptr;
}

void Keys (Session& s, const overlayhud::Input& v, bool textInput)
{
    if (textInput)
        return; // an ImGui text field has the keyboard
    using K = edit::TrackerKey::Kind;
    bool escaped = false;
    const auto escape = [&] {
        if (!escaped)
            Escape (s);
        escaped = true;
    };
    for (const auto action : v.editActions) {
        if (action == overlayhud::Input::EditAction::Cancel)
            escape ();
        else if (action == overlayhud::Input::EditAction::Undo || action == overlayhud::Input::EditAction::Redo)
            History (s, action == overlayhud::Input::EditAction::Undo);
        else
            s.message = "Archicad's delete, cut and paste are off while the virtual editor has the view.";
    }
    const bool typing = s.tracker.State () == edit::TrackerState::Editing;
    for (const auto& key : v.keys) {
        if (!key.down || key.repeat)
            continue;
        switch (key.code) {
            case 0x1B:
                escape ();
                break;
            case 0x0D:
                if (s.tracker.State () == edit::TrackerState::Editing)
                    TrackerEvent (s, s.tracker.Key ({ K::Enter }));
                else if (!s.drawing.empty ())
                    FinishDrawing (s);
                break;
            case 0x09:
                TrackerEvent (s, s.tracker.Key ({ key.shift ? K::BackTab : K::Tab }));
                break;
            case 0x08:
                if (typing)
                    s.tracker.Key ({ K::Backspace });
                else
                    DeleteSelection (s);
                break;
            case 0x2E:
                if (!typing)
                    DeleteSelection (s);
                break;
            default:
                if (key.ctrl && (key.code == 'Z' || key.code == 'Y'))
                    History (s, key.code == 'Z');
                else if (!typing && !key.ctrl && !key.alt) {
                    switch (key.code) {
                        case 'V':
                            s.tool = Tool::Select, s.drawing.clear ();
                            break;
                        case 'L':
                            s.tool = Tool::Polyline;
                            break;
                        case 'P':
                            s.tool = Tool::Path;
                            break;
                        case 'G':
                            s.gizmo = edit::GizmoMode::Move;
                            break;
                        case 'R':
                            s.gizmo = edit::GizmoMode::Rotate;
                            break;
                        case 'S':
                            s.gizmo = edit::GizmoMode::Scale;
                            break;
                        case 'T':
                            s.gizmo = edit::GizmoMode::Stretch;
                            break;
                        case 'E':
                            s.gizmo = edit::GizmoMode::Extrude;
                            break;
                        default:
                            break;
                    }
                }
                break;
        }
    }
    for (const char16_t c : v.characters)
        if ((c >= u'0' && c <= u'9') || c == u'-' || c == u'+' || c == u'.' || c == u',')
            TrackerEvent (s, s.tracker.Key ({ K::Char, char (c) }));
}

void OnView (Session& s, const overlayhud::Input& v, bool hovered, float scale)
{
    const Map m { v };
    auto* draw = ImGui::GetWindowDrawList ();
    s.canvas.list = draw;
    s.canvas.batches.clear ();
    draw->PushClipRect ({ -1e6f, -1e6f }, { 1e6f, 1e6f }, false);
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool shift = ImGui::GetIO ().KeyShift;
    if ((s.drag == Drag::Part || s.drag == Drag::Handle) && s.dragPlanar != v.planar) {
        // A view switch mid-gesture cannot carry the gesture into the other projection.
        s.edits.Cancel ();
        s.drag = Drag::None;
    }
    const edit::PickView pick = m.Pick ();
    const double radius = kPickPixels * scale;
    const bool gizmo = GizmoShown (s);
    const edit::Frame frame = gizmo ? GizmoFrame (s) : edit::Frame {};
    const auto handles = edit::Handles (s.gizmo);
    const double gizmoSize = kGizmoPixels * scale * m.MetresPerPixel (mouse.x, mouse.y, frame.origin.z, frame.origin);

    // Hover: a gizmo handle first, then the document.
    std::vector<edit::PickHit> hits;
    if (s.drag == Drag::None && hovered && s.tool == Tool::Select) {
        s.hoverHandle = gizmo ? edit::PickHandle (frame, handles, gizmoSize, pick, mouse.x, mouse.y, radius) : -1;
        hits = edit::Pick (Doc (s), pick, mouse.x, mouse.y, s.filter, radius);
        s.selection.hover = s.hoverHandle < 0 && !hits.empty () ? hits.front ().ref : edit::Ref {};
    }
    else if (!hovered && s.drag == Drag::None)
        s.selection.hover = {}, s.hoverHandle = -1;

    // Press.
    if (hovered && ImGui::IsMouseClicked (0) && s.drag == Drag::None) {
        if (s.tool != Tool::Select) {
            const double z = s.drawing.empty () ? s.workZ : s.drawing.back ().z;
            Vec3 p;
            std::optional<Vec3> anchor;
            if (!s.drawing.empty ())
                anchor = s.drawing.back ();
            if (Pointer (s, m, mouse, z, s.drawing.empty () ? Vec3 {} : s.drawing.back (), anchor, {}, scale, p)) {
                if (!s.drawing.empty () && edit::Distance (p, s.drawing.back ()) < 1.0e-6)
                    FinishDrawing (s); // a second click on the last point ends the line
                else if (s.drawing.size () >= 3 && edit::Distance (p, s.drawing.front ()) < 1.0e-6) {
                    s.drawing.push_back (p); // back on the start: a closed outline
                    FinishDrawing (s);
                }
                else
                    s.drawing.push_back (p);
            }
            else
                s.message = "This view is edge-on to the working plane. Orbit or draw in the plan.";
        }
        else if (s.hoverHandle >= 0) {
            s.drag = Drag::Handle;
            s.handle = s.hoverHandle;
            s.frame = frame;
            s.dragTransform = {};
            s.dragPlanar = v.planar;
            if (m.RayAt (mouse.x, mouse.y, frame.origin.z, frame.origin, s.startRay) && s.edits.Begin ())
                s.message.clear ();
            else
                s.drag = Drag::None, s.message = "That handle cannot be dragged in this view.";
        }
        else if (!hits.empty ()) {
            s.selection.Click (hits, mouse.x, mouse.y, shift);
            if (!shift && s.selection.Items ().size () == 1) {
                const edit::Ref pressed = s.selection.Items ().front ();
                const edit::Entity* e = edit::Find (Doc (s), pressed.entity);
                if (pressed.part == edit::Part::Face && e != nullptr) {
                    // A face pushes or pulls along its normal: the face handle, taken directly.
                    s.drag = Drag::Handle;
                    s.handle = -2;
                    edit::FaceFrame (*e, pressed.item, s.frame);
                    s.dragPlanar = v.planar;
                    if (!m.RayAt (mouse.x, mouse.y, s.frame.origin.z, s.frame.origin, s.startRay) ||
                        !s.edits.Begin (pressed))
                        s.drag = Drag::None;
                }
                else if (edit::PointOf (Doc (s), pressed, s.anchor)) {
                    s.drag = Drag::Part;
                    s.target = pressed;
                    s.dragPlanar = v.planar;
                    s.startX = mouse.x, s.startY = mouse.y;
                    if (!m.Onto (mouse.x, mouse.y, s.anchor.z, s.anchor, s.startCursor) || !s.edits.Begin (pressed))
                        s.drag = Drag::None;
                }
            }
        }
        else {
            if (!shift)
                s.selection.Clear ();
            s.drag = Drag::Marquee;
            s.startX = mouse.x, s.startY = mouse.y;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked (0) && s.tool != Tool::Select)
        FinishDrawing (s);

    // A right click opens the menu for what is under the pointer, selecting it first -- also
    // while the menu is open elsewhere: the click that closes it opens it again here.
    const bool rightHere = v.pointer && ImGui::IsWindowHovered (ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if ((hovered || (s.menu.open && rightHere)) && ImGui::IsMouseClicked (ImGuiMouseButton_Right) &&
        s.drag == Drag::None) {
        const auto under = edit::Pick (Doc (s), pick, mouse.x, mouse.y, s.filter, radius);
        Vec3 at, on;
        const double z = !s.drawing.empty ()                                                  ? s.drawing.back ().z
                         : !under.empty () && edit::PointOf (Doc (s), under.front ().ref, on) ? on.z
                                                                                              : s.workZ;
        if (!m.Onto (mouse.x, mouse.y, z, on, at))
            at = on;
        if (s.tool != Tool::Select && !s.drawing.empty ())
            OpenMenu (s, MenuTarget::Drawing, {}, at);
        else if (under.empty ())
            OpenMenu (s, MenuTarget::View, {}, at);
        else {
            const edit::Ref ref = under.front ().ref;
            if (!s.selection.Contains (ref))
                s.selection.Set ({ ref });
            const bool path = edit::FindPath (Doc (s), ref.entity) != nullptr;
            MenuTarget target = MenuTarget::Object;
            switch (ref.part) {
                case edit::Part::Vertex:
                    target = MenuTarget::Vertex;
                    break;
                case edit::Part::Segment:
                    target = MenuTarget::Segment;
                    break;
                case edit::Part::PathPoint:
                    target = MenuTarget::PathPoint;
                    break;
                case edit::Part::Break:
                    target = MenuTarget::GradeBreak;
                    break;
                case edit::Part::Range:
                    target = MenuTarget::Range;
                    break;
                default:
                    target = path ? MenuTarget::Path : MenuTarget::Object;
                    break;
            }
            OpenMenu (s, target, ref, at);
        }
    }
    Menu (s);

    // Move.
    if (ImGui::IsMouseDown (0) && s.drag == Drag::Part) {
        const auto moving = Moving (s, s.target);
        Vec3 at;
        if (Pointer (s, m, mouse, s.anchor.z, s.anchor, s.anchor, Exclusions (Doc (s), moving), scale, at)) {
            edit::Operation op;
            const edit::Path* path = edit::FindPath (s.edits.Before (), s.target.entity);
            if (path != nullptr && s.target.part == edit::Part::Range)
                op = [id = path->id, at] (edit::Document& d, std::string& e) {
                    return edit::SetPathWidthFromPoint (d, id, at, e);
                };
            else if (path != nullptr && s.target.part == edit::Part::Break) {
                edit::AlignmentGeometry g;
                std::string why;
                double station = 0.0, offset = 0.0, z = s.anchor.z;
                if (edit::BuildAlignment (path->points, g, why) && edit::StationOf (g, at, station, offset))
                    op = edit::DragBreak (path->id, s.target.item, station, z);
            }
            else {
                // Pressing on an item lifted off its grab point moves by the pointer's travel.
                edit::Transform t;
                t.translate = { at.x - s.anchor.x, at.y - s.anchor.y, 0.0 };
                if (!s.snap.snapped)
                    t.translate = { at.x - s.startCursor.x, at.y - s.startCursor.y, 0.0 };
                op = edit::TransformOperation (moving, t);
            }
            if (op && !s.edits.Move (op))
                s.message = s.edits.Message ();
        }
        else
            s.message = "This view is edge-on to the working plane. Orbit or type a value.";
    }
    if (ImGui::IsMouseDown (0) && s.drag == Drag::Handle) {
        edit::Ray now;
        const edit::Handle h = s.handle == -2 ? edit::Handles (edit::GizmoMode::Extrude).front ()
                                              : edit::Handles (s.gizmo)[size_t (s.handle)];
        if (m.RayAt (mouse.x, mouse.y, s.frame.origin.z, s.frame.origin, now)) {
            const auto r = edit::Drag (s.frame, h, s.startRay, now);
            const auto refs = s.handle == -2 ? std::vector<edit::Ref> { s.edits.Target () } : s.selection.Items ();
            if (!r.ok)
                s.message = r.error;
            else if (!s.edits.Move (edit::TransformOperation (refs, r.transform)))
                s.message = s.edits.Message ();
            else {
                s.dragTransform = r.transform;
                char text[48];
                std::snprintf (text, sizeof text, "%.2f %s", r.value, edit::QuantityUnit (h.quantity));
                s.snap = {};
                s.snap.point = s.frame.origin;
                s.snap.hint = text;
                s.snap.snapped = true;
            }
        }
    }

    // Release.
    if (ImGui::IsMouseReleased (0)) {
        if (s.drag == Drag::Part || s.drag == Drag::Handle) {
            s.edits.End ();
            s.snap = {};
        }
        // A dragged pivot stays where it was put; one already set travels with its selection.
        if (s.drag == Drag::Handle && s.handle >= 0 &&
            (s.dragTransform.relocate || (s.pivot && s.pivotFor == s.selection.Items ()))) {
            s.pivot = edit::Moved (s.frame, s.dragTransform).origin;
            s.pivotFor = s.selection.Items ();
        }
        else if (s.drag == Drag::Marquee && std::hypot (mouse.x - s.startX, mouse.y - s.startY) > 3.0f)
            s.selection.Marquee (Doc (s), pick, s.startX, s.startY, mouse.x, mouse.y, s.filter, shift);
        if (s.drag == Drag::Part || s.drag == Drag::Handle || s.drag == Drag::Marquee)
            s.drag = Drag::None;
    }

    // Drawing tools follow the pointer between clicks.
    if (s.tool != Tool::Select && hovered && s.drag == Drag::None) {
        const double z = s.drawing.empty () ? s.workZ : s.drawing.back ().z;
        Vec3 p;
        std::optional<Vec3> anchor;
        if (!s.drawing.empty ())
            anchor = s.drawing.back ();
        if (Pointer (s, m, mouse, z, s.drawing.empty () ? Vec3 {} : s.drawing.back (), anchor, {}, scale, p)) {
            if (const auto length = s.tracker.Locked (edit::Quantity::Length); length && !s.drawing.empty ()) {
                Vec3 dir;
                if (edit::Normalize (Vec3 { p.x - anchor->x, p.y - anchor->y, 0.0 }, dir))
                    s.snap.point = *anchor + dir * *length;
            }
            for (size_t i = 0; i + 1 < s.drawing.size (); ++i)
                Line (s, m, s.drawing[i], s.drawing[i + 1], kAccent, 2.5f * scale);
            if (!s.drawing.empty ())
                Line (s, m, s.drawing.back (), s.snap.point, kAccent, 1.5f * scale);
            for (const auto& q : s.drawing)
                Dot (s, m, q, 4.0f * scale, kAccent);
        }
    }

    // Hover highlight.
    if (s.drag == Drag::None && s.selection.hover.entity != edit::kNoId) {
        Vec3 p;
        const auto& hover = s.selection.hover;
        const edit::Entity* e = edit::Find (Doc (s), hover.entity);
        if (hover.part == edit::Part::Segment && e != nullptr) {
            const int i = edit::SegmentIndex (*e, hover.item);
            size_t from = 0, to = 0;
            if (i >= 0) {
                edit::SegmentEnds (*e, size_t (i), from, to);
                Line (s, m, e->vertices[from].p, e->vertices[to].p, kHover, 4.0f * scale);
            }
        }
        else if (edit::PointOf (Doc (s), hover, p))
            Dot (s, m, p, 7.0f * scale, kHover, false);
    }

    // Snap marker, its guides and its hint.
    if (s.snap.snapped && (s.drag != Drag::None || s.tool != Tool::Select)) {
        for (const auto& g : s.snap.guides)
            Line (s, m, g.from, g.to, kGuide, 1.25f * scale);
        Dot (s, m, s.snap.point, 5.0f * scale, kSnap);
        Note (s, m, s.snap.point, { 14 * scale, -22 * scale }, s.snap.hint, kInk);
    }

    // The live dimensions, clear of the gizmo and of the tracker beside the pointer.
    {
        std::vector<editlayers::Box> taken;
        ImVec2 o;
        if (gizmo && m.Screen (frame.origin, o)) {
            const float r = (kGizmoPixels + 12.0f) * scale;
            taken.push_back ({ o.x - r, o.y - r, o.x + r, o.y + r });
        }
        if (hovered) {
            const float rows = float (std::max<size_t> (s.tracker.Items ().size (), 1));
            taken.push_back ({ mouse.x + 12 * scale, mouse.y + 14 * scale, mouse.x + 200 * scale,
                               mouse.y + 20 * scale + rows * (ImGui::GetTextLineHeight () + 6) });
        }
        DrawDimensions (s, m, taken, scale);
    }

    // The gizmo.
    if (gizmo && s.drag != Drag::Part) {
        // Through a drag the gumball rides what it moves: carried and turned with it.
        const edit::Frame f = s.drag == Drag::Handle && s.handle >= 0 ? edit::Moved (s.frame, s.dragTransform) : frame;
        for (const auto& h : handles) {
            const bool active = int (h.index) == (s.drag == Drag::Handle ? s.handle : s.hoverHandle);
            const ImU32 colour = active ? kSnap : h.axis >= 0 && h.axis < 3 ? kAxis[h.axis] : kHover;
            for (const auto& shape : edit::HandleGeometry (f, h, gizmoSize)) {
                if (shape.points.size () == 1) {
                    Dot (s, m, shape.points[0], 5.0f * scale, colour);
                    continue;
                }
                const size_t n = shape.points.size (), edges = shape.closed ? n : n - 1;
                for (size_t i = 0; i < edges; ++i)
                    Line (s, m, shape.points[i], shape.points[(i + 1) % n], colour, (active ? 4.0f : 2.5f) * scale);
                if (h.kind == edit::HandleKind::Axis || h.kind == edit::HandleKind::Face)
                    Dot (s, m, shape.points.back (), 4.0f * scale, colour);
            }
        }
    }

    // The marquee, in screen pixels: left to right solid (contained), right to left dashed-thin (touched).
    if (s.drag == Drag::Marquee && ImGui::IsMouseDown (0)) {
        const bool crossing = mouse.x < s.startX;
        draw->AddRectFilled ({ std::min (s.startX, mouse.x), std::min (s.startY, mouse.y) },
                             { std::max (s.startX, mouse.x), std::max (s.startY, mouse.y) },
                             crossing ? IM_COL32 (70, 167, 88, 40) : IM_COL32 (62, 99, 221, 40));
        draw->AddRect ({ std::min (s.startX, mouse.x), std::min (s.startY, mouse.y) },
                       { std::max (s.startX, mouse.x), std::max (s.startY, mouse.y) }, crossing ? kAxis[1] : kAxis[2],
                       0.0f, 0, crossing ? 1.0f : 1.5f);
    }

    // The tracker beside the pointer.
    std::vector<edit::Dimension> dims;
    s.tracker.Show (TrackerFields (s, dims));
    if (hovered && s.tracker.State () != edit::TrackerState::Hidden) {
        float y = mouse.y + 20 * scale;
        for (size_t i = 0; i < s.tracker.Items ().size (); ++i) {
            const auto& item = s.tracker.Items ()[i];
            char text[80];
            const bool editing = s.tracker.State () == edit::TrackerState::Editing && i == s.tracker.Focus ();
            if (editing)
                std::snprintf (text, sizeof text, "%s  %s_", edit::QuantityName (item.quantity),
                               s.tracker.Text ().c_str ());
            else
                std::snprintf (
                    text, sizeof text, "%s  %s%s", edit::QuantityName (item.quantity),
                    editlayers::FormatValue (item.quantity, item.locked ? item.lockedValue : item.value).c_str (),
                    item.locked ? "  (locked)" : "");
            const ImVec2 at { mouse.x + 18 * scale, y };
            const ImVec2 size = ImGui::CalcTextSize (text);
            draw->AddRectFilled ({ at.x - 4, at.y - 2 }, { at.x + size.x + 4, at.y + size.y + 2 },
                                 i == s.tracker.Focus () ? IM_COL32 (255, 236, 179, 240) : kPaper, 3.0f);
            draw->AddText (at, kInk, text);
            y += size.y + 6;
        }
    }
    // One cursor per state, said every frame so nothing of Archicad's shows between them: a
    // crosshair while a drawing tool places points, the move cross on a handle and through any
    // drag, the arrow otherwise.
    if (hovered) {
        const bool dragging = s.drag == Drag::Part || s.drag == Drag::Handle;
        hudshell::OwnCursor (s.tool != Tool::Select && s.drag == Drag::None);
        ImGui::SetMouseCursor (dragging || s.hoverHandle >= 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow);
    }
    draw->PopClipRect ();
}

} // namespace geomsrv::archviz::edithud
