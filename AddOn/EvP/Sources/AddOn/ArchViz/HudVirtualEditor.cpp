#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/OverlayHitMap.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace geomsrv::archviz::edithud {

namespace detail {

using edit::Vec3;
namespace rc = roofcanvas;

const edit::Document& Doc (const Session& s)
{
    return s.edits.Doc ();
}

} // namespace detail

namespace {

using namespace detail;

using edit::Vec3;
namespace rc = roofcanvas;

constexpr float kPickPixels = 8.0f, kGizmoPixels = 70.0f, kSnapPixels = 10.0f, kDragPixels = 3.0f;

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

bool GizmoShown (const Session& s)
{
    return s.tool == Tool::Select && !s.selection.Items ().empty () &&
           s.selection.Kind () != edit::SelectionKind::Range && s.selection.Kind () != edit::SelectionKind::Break;
}

// The point the pointer is on, snapped, at height z.
bool Pointer (Session& s, const Map& m, ImVec2 mouse, double z, Vec3 seed, std::optional<Vec3> anchor,
              const std::vector<edit::Ref>& exclude, float scale, Vec3& out)
{
    // Only drawing needs an augmented context for its unfinished points. Ordinary drags borrow
    // the immutable host snapshot instead of copying all its points, edges and source strings.
    edit::SnapContext context;
    if (!s.drawing.empty ()) {
        context = s.context;
        for (const auto& p : s.drawing)
            context.points.push_back ({ p, "this line" });
    }
    Vec3 cursor;
    const auto plane = s.tool == Tool::Select ? edit::WorldXY (z) : s.capturedPlane.value_or (s.plane);
    double pixelScale = 0;
    if (!m.Onto (mouse.x, mouse.y, plane, seed, cursor) ||
        !m.MetresPerPixel (mouse.x, mouse.y, plane, cursor, pixelScale))
        return false;
    if (s.drag == Drag::Part)
        cursor = cursor + s.navigation.offset;
    edit::SnapQuery q;
    q.cursor = cursor;
    q.tolerance = kSnapPixels * scale * pixelScale;
    q.anchor = anchor;
    q.plane = edit::SnapPlane (plane);
    q.exclude = exclude;
    PrepareSnap (s, q);
    ScreenPointer (s, m, mouse, scale, q);
    s.snap = edit::Snap (Doc (s), s.drawing.empty () ? s.context : context, q);
    Acquire (s, s.snap);
    out = s.snap.point;
    return true;
}

void Escape (Session& s)
{
    if (CancelLandscapeCommand (s) || CancelGumballReposition (s))
        return;
    if (s.tracker.State () == edit::TrackerState::Editing) {
        s.tracker.Key ({ edit::TrackerKey::Kind::Escape });
        if (s.planeTyping || s.profileTyping)
            s.message.clear ();
        s.planeTyping = false;
        s.planeEntryPath = edit::kNoId;
        s.planeEntryRevision = 0;
        s.profileTyping = false;
    }
    else if (s.planeTyping)
        s.planeTyping = false, s.planeEntryPath = edit::kNoId, s.planeEntryRevision = 0, s.message.clear ();
    else if (s.profileTyping)
        s.profileTyping = false, s.message.clear ();
    else if (s.typedHandle >= 0)
        s.typedHandle = -1, s.message.clear ();
    else if (s.drag != Drag::None) {
        s.edits.Cancel ();
        s.drag = Drag::None;
        s.lassoPoints.clear (), s.marqueeLasso = false, s.pressHits.clear ();
        s.dragReleased = false;
        s.dragPointerPause.reset ();
        s.snap = {};
        s.message.clear ();
    }
    else if (!s.drawing.empty ())
        s.drawing.clear (), s.references.clear ();
    else if (!s.selection.Items ().empty ())
        s.selection.Clear ();
    else
        s.close = true;
}

void CheckView (Session& s, const overlayhud::Input& v)
{
    CheckPlane (s);
    if (s.viewPlanar && *s.viewPlanar != v.planar)
        CancelViewEdit (s, "The view changed between plan and 3D; the edit in progress was cancelled.");
    if (s.viewIdentity != 0 && v.viewIdentity != 0 && s.viewIdentity != v.viewIdentity)
        CancelViewEdit (s, "The host window changed; the edit in progress was cancelled.");
    s.viewPlanar = v.planar;
    s.viewIdentity = v.viewIdentity;
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
    if (event.outcome != O::Commit && event.outcome != O::Locked)
        return;
    if (s.planeTyping) {
        if (event.outcome == O::Locked)
            s.message = "Enter sets the working plane; Tab does not lock this setting.";
        else if (s.planeEntryPath != edit::kNoId) {
            if (s.edits.Doc ().revision != s.planeEntryRevision) {
                s.message = "The path document changed; Esc this entry and choose the path again.";
                return;
            }
            s.planeTyping = false;
            if (UsePathPlane (s, s.planeEntryPath, event.value))
                s.planeEntryPath = edit::kNoId, s.planeEntryRevision = 0;
            else
                s.planeTyping = true;
        }
        else {
            auto plane = edit::WorldXY (event.value);
            plane.locked = s.plane.locked;
            s.planeTyping = false;
            if (!SetWorkingPlane (s, plane))
                s.planeTyping = true;
        }
        return;
    }
    if (s.profileTyping) {
        // A profile label's value: Enter sets it, Esc gives it up, Tab locks nothing here. The
        // binding is only as good as the revision it was made at -- an edit since then means
        // the value it would write is not the one the click read, so it is refused unapplied.
        if (event.outcome == O::Locked)
            s.message = "Enter sets the value; Tab does not lock this one.";
        else if (s.edits.Doc ().revision != s.profileRevision)
            s.message = "The path changed while the value was typed; click its label again.";
        else {
            s.profileTyping = false;
            if (!s.edits.Apply (edit::WriteDimension (s.profileTarget, event.value))) {
                s.profileTyping = true; // a refusal keeps the field, with what was typed, to correct
                s.message = s.edits.Message ();
            }
            else
                s.message.clear ();
        }
        return;
    }
    if (const auto op = RadiusDragOperation (s, event.value)) {
        if (!s.edits.Move (op))
            s.message = s.edits.Message ();
        else {
            s.dragValue = event.value;
            if (s.drag == Drag::Kerb)
                s.kerbPicked = true;
            if (event.outcome == O::Commit)
                EndDrag (s);
            s.message.clear ();
        }
        return;
    }
    if (event.outcome != O::Commit)
        return;
    if (s.drag == Drag::Handle && s.dragStarted) {
        // A number typed through a gumball drag: the drag's own transform at that value, from
        // where the gesture began, and the gesture ends there -- once, whatever the button does.
        const auto r = edit::FromValue (s.frame, HeldControl (s), event.value);
        if (!r.ok)
            s.message = r.error;
        else if (!s.edits.Move (edit::TransformOperation (HeldRefs (s), r.transform)))
            s.message = s.edits.Message ();
        else {
            s.dragTransform = r.transform;
            EndHandleDrag (s);
            s.message.clear ();
        }
        return;
    }
    if (s.drag == Drag::Part && s.dragStarted && s.dragTypable) {
        // A distance typed through a part drag: the items moved that far the way the pointer has
        // taken them, from where the gesture began, and the gesture ends there once.
        if (!s.dragDirection) {
            s.message = "Move the pointer the way the distance should go, then type it.";
            return;
        }
        edit::Transform t;
        t.translate = *s.dragDirection * event.value;
        if (!s.edits.Move (edit::TransformOperation (Moving (s, s.target), t))) {
            s.message = s.edits.Message ();
            return;
        }
        s.edits.End ();
        s.drag = Drag::None;
        s.dragReleased = false;
        s.snap = {};
        s.message.clear ();
        return;
    }
    if (s.typedHandle >= 0) {
        // The clicked handle's number, on the control the click captured.
        const auto r = edit::FromValue (s.typedFrame, s.typedControl, event.value);
        s.typedHandle = -1;
        if (!r.ok)
            s.message = r.error;
        else if (!s.edits.Apply (edit::TransformOperation (s.selection.Items (), r.transform)))
            s.message = s.edits.Message ();
        else
            s.message.clear ();
        return;
    }
    if (!s.drawing.empty ()) {
        // A typed length places the next point along the pointer's current direction.
        const Vec3 last = s.drawing.back ();
        const auto plane = s.capturedPlane.value_or (s.plane);
        Vec3 dir;
        const Vec3 toward = s.snap.snapped ? s.snap.point : last + plane.u;
        const Vec3 delta = toward - last;
        if (!edit::Normalize (delta - plane.normal * edit::Dot (delta, plane.normal), dir))
            dir = plane.u;
        if (const auto angle = s.tracker.Locked (edit::Quantity::Angle))
            dir = plane.u * std::cos (*angle * 3.14159265358979323846 / 180.0) +
                  plane.v * std::sin (*angle * 3.14159265358979323846 / 180.0);
        if (s.tracker.Items ()[event.item].quantity == edit::Quantity::Length && event.value > 0.0)
            s.drawing.push_back (last + dir * event.value);
        return;
    }
    if (TypeKerb (s, event.value)) // a picked kerb corner's radius
        return;
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
    if (s.planeTyping && s.planeEntryPath != edit::kNoId)
        return { { edit::Quantity::Distance,
                   s.plane.source == edit::PlaneSource::PathPlan && s.plane.sourceRef.entity == s.planeEntryPath
                       ? s.plane.sourceStation
                       : 0 } };
    if (s.planeTyping)
        return { { edit::Quantity::Elevation, s.plane.origin.z } };
    if (s.profileTyping) {
        // The profile canvas' binding: the exact dimension a clicked label read, whatever the
        // selection has become, so nothing else typed through the tracker can shadow it.
        double value = 0.0;
        edit::ReadDimension (s.edits.Doc (), s.profileTarget, value);
        return { { s.profileTarget.quantity, value } };
    }
    if (!s.drawing.empty ()) {
        const auto plane = s.capturedPlane.value_or (s.plane);
        const Vec3 delta = s.snap.point - s.drawing.back ();
        const double u = edit::Dot (delta, plane.u), v = edit::Dot (delta, plane.v);
        items.push_back ({ edit::Quantity::Length, std::hypot (u, v) });
        items.push_back ({ edit::Quantity::Angle, std::atan2 (v, u) * 180.0 / 3.14159265358979323846 });
        return items;
    }
    if ((s.drag == Drag::Corner || s.drag == Drag::Kerb) && s.dragStarted) {
        items.push_back ({ edit::Quantity::Radius, s.dragValue });
        return items;
    }
    if (s.drag == Drag::Part && s.dragStarted && s.dragTypable) {
        items.push_back ({ edit::Quantity::Distance, s.dragValue }); // how far the items have moved
        return items;
    }
    if (s.drag == Drag::Handle && s.dragStarted) {
        // The held handle's quantity, what the drag has made of it so far; a handle that takes no
        // number (a plane, the pivot) offers none.
        const edit::Handle h = HeldControl (s);
        if (h.focusable)
            items.push_back ({ h.quantity, s.dragValue });
        return items;
    }
    if (s.selection.Items ().empty ())
        return items;
    if (double r = 0.0; KerbField (s, r)) {
        items.push_back ({ edit::Quantity::Radius, r });
        return items;
    }
    if (s.typedHandle >= 0) {
        const edit::Handle h = s.typedControl;
        const int a = h.axis >= 0 && h.axis < 3 ? h.axis : 2;
        const double now = h.quantity == edit::Quantity::Factor ? 1.0
                           : h.kind == edit::HandleKind::Side   ? 2.0 * s.typedFrame.half[a]
                                                                : 0.0;
        items.push_back ({ h.quantity, now });
        return items;
    }
    dims = edit::DimensionsFor (Doc (s), s.selection.Items ().front ());
    for (auto& d : dims)
        d.fromEnd = s.fromEnd;
    for (const auto& d : dims) {
        double value = 0.0;
        edit::ReadDimension (Doc (s), d, value);
        items.push_back ({ d.quantity, value });
    }
    return items;
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
    if (!v.viewActive)
        return;
    CheckView (s, v); // stale numeric input cannot commit before the next view layout
    if (textInput || LabelKeys (s, v))
        return; // an ImGui text field, or a label open for typing, has the keyboard
    if (LandscapeCommandKeys (s, v))
        return;
    if (GumballRepositioning (s)) {
        for (const auto action : v.editActions)
            if (action == overlayhud::Input::EditAction::Cancel)
                CancelGumballReposition (s);
        for (const auto& key : v.keys)
            if (key.down && !key.repeat && key.code == 0x1B)
                CancelGumballReposition (s);
        return;
    }
    using K = edit::TrackerKey::Kind;
    bool escaped = false;
    const auto escape = [&] {
        if (!escaped) {
            const bool field = s.tracker.State () == edit::TrackerState::Editing;
            Escape (s);
            if (field && s.drag != Drag::None)
                s.dragPointerPause = { v.x, v.y };
        }
        escaped = true;
    };
    // Undo and redo are the document's, never the number being typed's: a chord pressed while
    // a value is typed does nothing to the history (Enter applies it, Esc gives it up first).
    const auto history = [&] (bool undo) {
        if (s.tracker.State () == edit::TrackerState::Editing || s.planeTyping || s.profileTyping)
            s.message = "Apply the typed value with Enter or give it up with Esc before undoing.";
        else
            History (s, undo);
    };
    // A mode key shows that transform's handles alone; pressed again, the gumball comes back.
    const auto mode = [&s] (edit::GizmoMode m) { SetGizmoMode (s, s.gizmo == m ? edit::GizmoMode::Gumball : m); };
    const auto trackerKey = [&s] (edit::TrackerKey key) {
        const edit::Tracker entry = s.tracker;
        const int handle = s.typedHandle;
        const auto event = s.tracker.Key (key);
        const bool applying =
            event.outcome == edit::Tracker::Outcome::Commit || event.outcome == edit::Tracker::Outcome::Locked;
        if (applying)
            s.message.clear ();
        TrackerEvent (s, event);
        if (applying && !s.message.empty ()) {
            s.tracker = entry;
            s.typedHandle = handle;
        }
    };
    for (const auto action : v.editActions) {
        if (action == overlayhud::Input::EditAction::Cancel)
            escape ();
        else if (action == overlayhud::Input::EditAction::Undo || action == overlayhud::Input::EditAction::Redo)
            history (action == overlayhud::Input::EditAction::Undo);
        else
            s.message = "Archicad's delete, cut and paste are off while the virtual editor has the view.";
    }
    // A resting profile binding is numeric input too: it owns the view's keys until Enter
    // applies it, Esc gives it up, or a digit opens its field (HudVirtualEditorPanel.cpp).
    const bool typing = s.tracker.State () == edit::TrackerState::Editing || s.planeTyping || s.profileTyping;
    for (const auto& key : v.keys) {
        if (!key.down || key.repeat)
            continue;
        switch (key.code) {
            case 0x1B:
                escape ();
                break;
            case 0x0D:
                if (s.tracker.State () == edit::TrackerState::Editing)
                    trackerKey ({ K::Enter });
                else if ((s.dragReleased || s.navigation.blocked) && s.drag == Drag::Marquee) {
                    CommitMarquee (s, Map { v }.Pick (), v.x, v.y);
                    s.dragReleased = false;
                    s.message.clear ();
                }
                else if ((s.dragReleased || s.navigation.blocked) && s.edits.InGesture ())
                    EndDrag (s);
                else if (!s.drawing.empty ())
                    FinishDrawing (s);
                break;
            case 0x09:
                trackerKey ({ key.shift ? K::BackTab : K::Tab });
                break;
            case 0x08:
                if (typing)
                    s.tracker.Key ({ K::Backspace });
                else if (!s.drawing.empty ()) {
                    // A line being drawn takes back its last point, and the pen that point's
                    // handle -- never the selection's geometry, which is not what is being drawn.
                    s.drawing.pop_back ();
                    if (s.drawHandles.size () > s.drawing.size ())
                        s.drawHandles.resize (s.drawing.size ());
                }
                else
                    DeleteSelection (s);
                break;
            case 0x2E:
                if (!typing)
                    DeleteSelection (s);
                break;
            default:
                if (key.ctrl && (key.code == 'Z' || key.code == 'Y'))
                    history (key.code == 'Z');
                else if (!typing && !key.ctrl && !key.alt && (s.drag != Drag::None || s.planeTyping)) {
                    // No tool or gumball mode changes under a gesture: the handle held is one of
                    // this mode's, and the panel's tools are off until it ends too.
                    if (key.code < 0x80 && key.code != 0 && std::strchr ("VLPBGRSTFEQNO", int (key.code)) != nullptr)
                        s.message = "Finish the drag, or Esc it, before switching tool or mode.";
                }
                else if (!typing && !key.ctrl && !key.alt) {
                    switch (key.code) {
                        case 'G':
                            mode (edit::GizmoMode::Move);
                            break;
                        case 'R':
                            mode (edit::GizmoMode::Rotate);
                            break;
                        case 'S':
                            mode (edit::GizmoMode::Scale);
                            break;
                        case 'T':
                            mode (edit::GizmoMode::Stretch);
                            break;
                        case 'F':
                            s.fromEnd = !s.fromEnd; // the other end of the segment stays
                            break;
                        case 'E':
                            mode (edit::GizmoMode::Extrude);
                            break;
                        default:
                            ToolKey (s, key.code); // the drawing tools' letters, a polygon's sides
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
    if (!v.viewActive)
        return;
    const Map m { v };
    auto* draw = ImGui::GetWindowDrawList ();
    s.canvas.list = draw;
    s.canvas.batches.clear ();
    draw->PushClipRect ({ -1e6f, -1e6f }, { 1e6f, 1e6f }, false);
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    const bool shift = ImGui::GetIO ().KeyShift;
    if (!s.gumballRepositionFor.empty () && !GumballRepositioning (s))
        CancelGumballReposition (s);
    CheckView (s, v);
    UpdatePathDetail (s, m, scale);
    Navigate (s, m, mouse);
    // A resting profile binding (the panel's field) withholds the pointer from the view, as a
    // plane entry does: it owns the number until Enter, Esc or a digit takes it.
    hovered = hovered && !s.navigation.blocked && !s.planeTyping && !s.profileTyping;
    if (LandscapeView (s, m, mouse, hovered, scale)) {
        draw->PopClipRect ();
        return;
    }
    // A selection that no longer supports the mode it was left in falls back to the gumball,
    // which shows whatever it does support. Said once, when the mode actually changes.
    if (s.drag == Drag::None && s.typedHandle < 0 && !s.selection.Items ().empty () && !GizmoAvailable (s, s.gizmo)) {
        if (s.gizmo != edit::GizmoMode::Gumball)
            s.message = "That control does not apply to this selection: back to the gumball.";
        s.gizmo = edit::GizmoMode::Gumball;
    }
    const edit::PickView pick = m.Pick ();
    const double radius = kPickPixels * scale;
    const bool gizmo = GizmoShown (s);
    const edit::Frame frame = gizmo ? GizmoFrame (s) : edit::Frame {};
    const auto handles = GizmoHandles (s, s.gizmo);
    ImVec2 pivotPixel;
    const double gizmoSize =
        m.Screen (frame.origin, pivotPixel)
            ? kGizmoPixels * scale * m.MetresPerPixel (pivotPixel.x, pivotPixel.y, frame.origin.z, frame.origin)
            : 0;

    // Hover: a gizmo handle first, then the document.
    std::vector<edit::PickHit> hits;
    if (s.drag == Drag::None && hovered && s.tool == Tool::Select) {
        s.hoverHandle = gizmo ? edit::PickHandle (frame, handles, gizmoSize, pick, mouse.x, mouse.y, radius) : -1;
        hits = edit::Pick (Doc (s), pick, mouse.x, mouse.y, s.filter, radius, &s.pickCache);
        s.selection.hover = s.hoverHandle < 0 && !hits.empty () ? hits.front ().ref : edit::Ref {};
    }
    else if (!hovered && s.drag == Drag::None)
        s.selection.hover = {}, s.hoverHandle = -1;

    // Live-corner grips first: they sit over the geometry and the gizmo.
    bool gripTook = CornerGrips (s, m, mouse, hovered && s.tool == Tool::Select, scale);
    gripTook = KerbGrips (s, m, mouse, hovered && !gripTook && s.tool == Tool::Select, scale) || gripTook;
    gripTook = LabelClick (s, mouse, hovered && !gripTook) || gripTook;
    gripTook = CurveHandles (s, m, mouse, hovered && !gripTook, pick, scale) || gripTook;

    // Press. A handle waiting for its number gives way to whatever is pressed next.
    if (s.typedHandle >= 0 && s.typedMode != s.gizmo)
        s.typedHandle = -1;
    if (!gripTook && hovered && ImGui::IsMouseClicked (0) && s.drag == Drag::None) {
        s.typedHandle = -1;
        if (s.tool != Tool::Select) {
            const double z = s.drawing.empty () ? s.plane.origin.z : s.drawing.back ().z;
            Vec3 p;
            std::optional<Vec3> anchor;
            if (!s.drawing.empty ())
                anchor = s.drawing.back ();
            if (Pointer (s, m, mouse, z, s.drawing.empty () ? Vec3 {} : s.drawing.back (), anchor, {}, scale, p)) {
                if (ShapeTool (s.tool)) {
                    // A shape is two clicks: the corner or centre, then the size.
                    if (s.drawing.empty ())
                        s.drawing.push_back (p);
                    else
                        FinishShape (s, p, shift);
                }
                else if (!s.drawing.empty () && edit::Distance (p, s.drawing.back ()) < 1.0e-6)
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
            s.dragStarted = false;
            s.dragReleased = false;
            s.dragPointerPause.reset ();
            s.dragValue = 0.0;
            s.handle = s.hoverHandle;
            s.heldControl = handles[size_t (s.hoverHandle)];
            s.facePush = false;
            s.frame = frame;
            s.dragTransform = {};
            s.typedHandle = -1;
            s.startX = mouse.x, s.startY = mouse.y;
            if (!m.RayAt (mouse.x, mouse.y, frame.origin.z, frame.origin, s.startRay) || !s.edits.Begin ())
                s.drag = Drag::None, s.message = "That handle cannot be dragged in this view.";
            else if (!edit::NeutralDrag (s.frame, s.heldControl, s.startRay).ok)
                // It can still be clicked for its number, but not dragged from here: the line,
                // plane or ring it runs on is edge-on. The first real move says so again.
                s.message = "This view is edge-on to that control; click it to type a value, or orbit.";
            else
                s.message.clear ();
        }
        else if (!hits.empty () && hits.front ().inside && !s.selection.Contains (hits.front ().ref)) {
            // Inside a region that is not selected: a drag from here selects by marquee, a click
            // selects the region (CommitMarquee). Nothing moves until it is selected.
            BeginMarquee (s, mouse, shift, v.ctrl, hits);
        }
        else if (!hits.empty ()) {
            // An object already selected whole is dragged whole from wherever it is pressed --
            // its own corner included, which becomes the base point. A click there without a
            // drag still picks the corner (at release).
            const edit::Ref hit = hits.front ().ref;
            const edit::Ref whole { hit.entity, edit::Part::Entity, 0 };
            s.wholeGrab = !shift && hit.part != edit::Part::Entity && edit::Find (Doc (s), hit.entity) != nullptr &&
                          s.selection.Contains (whole);
            s.pressHits = hits;
            if (!s.wholeGrab)
                s.selection.Click (hits, mouse.x, mouse.y, shift);
            if (!shift && (s.wholeGrab || s.selection.Items ().size () == 1)) {
                const edit::Ref pressed = s.wholeGrab ? whole : s.selection.Items ().front ();
                const edit::Entity* e = edit::Find (Doc (s), pressed.entity);
                if (pressed.part == edit::Part::Face && e != nullptr)
                    BeginFacePush (s, m, mouse.x, mouse.y, pressed);
                else if (edit::PointOf (Doc (s), pressed, s.anchor)) {
                    s.drag = Drag::Part;
                    s.dragStarted = false;
                    s.dragReleased = false;
                    s.dragPointerPause.reset ();
                    s.dragValue = 0.0;
                    s.dragDirection.reset ();
                    s.target = pressed;
                    s.startX = mouse.x, s.startY = mouse.y;
                    if (!m.Onto (mouse.x, mouse.y, s.anchor.z, s.anchor, s.startCursor) || !s.edits.Begin (pressed))
                        s.drag = Drag::None;
                    // The press point is the drag base: the item moves by the pointer's travel and keeps
                    // its captured offset exactly, so a press near a corner never jumps; a press exactly
                    // on a corner still puts that corner on a target.
                    if (s.drag == Drag::Part)
                        s.navigation.point = s.startCursor;
                }
            }
        }
        else {
            if (!shift)
                s.selection.Clear ();
            BeginMarquee (s, mouse, shift, v.ctrl, {});
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked (0) && s.tool != Tool::Select)
        FinishDrawing (s);
    if (hovered)
        PenDrag (s, m, mouse); // the pen's press past the threshold pulls out handles

    // A right click opens the menu for what is under the pointer, selecting it first -- also
    // while the menu is open elsewhere: the click that closes it opens it again here.
    const bool rightHere = v.pointer && ImGui::IsWindowHovered (ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if ((hovered || (s.menu.open && rightHere)) && ImGui::IsMouseClicked (ImGuiMouseButton_Right) &&
        s.drag == Drag::None) {
        const auto under = edit::Pick (Doc (s), pick, mouse.x, mouse.y, s.filter, radius, &s.pickCache);
        Vec3 at, on;
        const double z = !s.drawing.empty ()                                                  ? s.drawing.back ().z
                         : !under.empty () && edit::PointOf (Doc (s), under.front ().ref, on) ? on.z
                                                                                              : s.plane.origin.z;
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
    // ⚠️ NOTHING MOVES UNTIL THE POINTER HAS (the user, 2026-10-15: just clicking an edge moved it --
    // the press snapped the grab point onto something nearby and the item jumped there). A drag
    // starts 3 px from its press; a press released inside that is a click. A gumball handle and
    // a pushed face wait the same: a hand's tremor on a click is not a move.
    // A short native drag can arrive in one layout: its release still owns its final position.
    const bool pointerHeld = ImGui::IsMouseDown (0) || ImGui::IsMouseReleased (0);
    if (!s.navigation.blocked && pointerHeld && (s.drag == Drag::Part || s.drag == Drag::Handle) && !s.dragStarted)
        s.dragStarted = std::hypot (mouse.x - s.startX, mouse.y - s.startY) >= kDragPixels;
    // While a number is typed through a drag the number has it: the preview stays as it was
    // when typing began, and the pointer moves nothing until Esc gives the number up.
    const bool typing = s.tracker.State () == edit::TrackerState::Editing;
    if (s.dragPointerPause && (mouse.x != s.dragPointerPause->first || mouse.y != s.dragPointerPause->second))
        s.dragPointerPause.reset ();
    const bool pointerMoves = DragPointerMoves (s, mouse);
    if (pointerHeld && s.drag == Drag::Part && s.dragStarted && pointerMoves) {
        const auto moving = Moving (s, s.target);
        Vec3 at;
        if (Pointer (s, m, mouse, s.startCursor.z, s.startCursor, s.startCursor, Exclusions (Doc (s), moving), scale,
                     at)) {
            edit::Operation op;
            const edit::Path* path = edit::FindPath (s.edits.Before (), s.target.entity);
            s.dragTypable =
                !(path != nullptr && (s.target.part == edit::Part::Range || s.target.part == edit::Part::Break));
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
                // Pressing on an item moves it by the pointer's travel from the captured press point,
                // snapped or not: the item keeps its offset under the pointer and only the pointer's own
                // snap pulls it, so engaging or giving up a snap never steps the item.
                edit::Transform t;
                t.translate = SnappedTranslation (s, at);
                if (!s.snap.snapped)
                    FlushEdges (s, m, mouse, moving, s.startCursor.z, Exclusions (Doc (s), moving), scale, t);
                op = edit::TransformOperation (moving, t);
                s.dragValue = std::hypot (t.translate.x, t.translate.y);
                if (s.dragValue > 1.0e-9)
                    s.dragDirection = t.translate * (1.0 / s.dragValue);
            }
            if (op) {
                if (!s.edits.Move (op))
                    s.message = s.edits.Message ();
                else
                    s.navigation.point = at;
            }
        }
        else
            s.message = "This view is edge-on to the working plane. Orbit or type a value.";
    }
    if (pointerHeld && s.drag == Drag::Handle && s.dragStarted && pointerMoves) {
        edit::Ray now;
        const edit::Handle h = HeldControl (s);
        if (m.RayAt (mouse.x, mouse.y, s.frame.origin.z, s.frame.origin, now)) {
            const auto r = s.navigation.handleRebased
                               ? edit::ContinueDrag (s.frame, h, s.navigation.handle, s.navigation.ray, now, shift)
                               : edit::Drag (s.frame, h, s.startRay, now, shift);
            if (!r.ok)
                s.message = r.error;
            else if (!s.edits.Move (edit::TransformOperation (HeldRefs (s), r.transform)))
                s.message = s.edits.Message ();
            else {
                s.dragTransform = r.transform;
                s.dragValue = r.value;
                char text[48];
                const bool everyAxis = shift && h.kind == edit::HandleKind::ScaleAxis;
                std::snprintf (text, sizeof text, "%.2f %s%s", r.value, edit::QuantityUnit (h.quantity),
                               everyAxis ? " uniform" : "");
                s.snap = {};
                s.snap.point = s.frame.origin;
                s.snap.hint = text;
                s.snap.snapped = true;
            }
        }
    }

    // Release. A handle clicked without a drag asks for its number instead; a face click is
    // only ever a select (its height has its own field, and a number typed mid-drag works).
    if (ImGui::IsMouseReleased (0) && !s.dragReleased && !s.navigation.blocked && s.drag == Drag::Handle &&
        !s.facePush && !s.dragStarted && HeldControl (s).focusable) {
        s.edits.Cancel ();
        s.typedHandle = s.handle;
        s.typedControl = s.heldControl;
        s.typedMode = s.gizmo;
        s.typedFrame = s.frame;
        s.dragTransform = {};
        s.drag = Drag::None;
        s.snap = {};
        s.message = "Type the value, Enter applies, Esc cancels.";
    }
    if (ImGui::IsMouseReleased (0) && !s.dragReleased && !s.navigation.blocked && s.drag == Drag::Part && s.wholeGrab &&
        std::hypot (mouse.x - s.startX, mouse.y - s.startY) < 3.0f) {
        s.edits.Cancel (); // a click on a selected object's corner picks the corner
        s.selection.Click (s.pressHits, s.startX, s.startY, false);
        s.drag = Drag::None;
    }
    if (ImGui::IsMouseReleased (0))
        s.wholeGrab = false;
    if (ImGui::IsMouseReleased (0) && !s.dragReleased && !s.navigation.blocked) {
        if ((s.drag == Drag::Handle || (s.drag == Drag::Part && s.dragStarted)) && typing) {
            // The typed number has the gesture: Enter ends it, this release does not.
            s.dragReleased = true;
            s.message = "Enter applies the typed value; Esc gives it up and keeps the drag as it was.";
        }
        else if (s.drag == Drag::Handle)
            EndHandleDrag (s);
        else {
            if (s.drag == Drag::Part) {
                s.edits.End ();
                s.message = s.edits.Message ();
                s.selection.Prune (Doc (s));
                s.snap = {};
            }
            else if (s.drag == Drag::Marquee)
                CommitMarquee (s, pick, mouse.x, mouse.y);
            if (s.drag == Drag::Part)
                s.drag = Drag::None;
        }
    }
    // Giving up a field never commits its underlying drag. After release the preview waits
    // visibly for an explicit finish, another number, or cancellation -- no hidden capture.
    if (s.dragReleased && s.drag != Drag::None && !typing)
        s.message = "Drag paused: Enter finishes this preview; type a value, or Esc cancels the drag.";

    // Drawing tools follow the pointer between clicks.
    if (s.tool != Tool::Select && (hovered || s.navigation.blocked) && s.drag == Drag::None) {
        const double z = s.drawing.empty () ? s.plane.origin.z : s.drawing.back ().z;
        Vec3 p;
        std::optional<Vec3> anchor;
        if (!s.drawing.empty ())
            anchor = s.drawing.back ();
        if (s.navigation.blocked ||
            Pointer (s, m, mouse, z, s.drawing.empty () ? Vec3 {} : s.drawing.back (), anchor, {}, scale, p)) {
            if (s.navigation.blocked)
                p = s.snap.point;
            if (const auto length = s.tracker.Locked (edit::Quantity::Length); length && !s.drawing.empty ()) {
                Vec3 dir;
                if (edit::Normalize (Vec3 { p.x - anchor->x, p.y - anchor->y, 0.0 }, dir))
                    s.snap.point = *anchor + dir * *length;
            }
            DrawingPreview (s, m, shift, scale);
        }
    }

    DrawHover (s, m, scale);

    // The live dimensions and path labels, clear of the gizmo and of each other. Nothing that
    // follows the pointer moves them: the snap hint and the typed value draw over them.
    {
        std::vector<editlayers::Box> taken;
        ImVec2 o;
        // A gizmo with nothing movable draws nothing, so it reserves no space either.
        const bool drawn =
            std::any_of (handles.begin (), handles.end (), [] (const edit::Handle& h) { return h.movable; });
        if (gizmo && drawn && m.Screen (frame.origin, o)) {
            const float r = (kGizmoPixels + 12.0f) * scale;
            taken.push_back ({ o.x - r, o.y - r, o.x + r, o.y + r });
        }
        DrawDimensions (s, m, taken, scale);
        DrawPathLabels (s, m, taken, scale);
        DrawContourLabels (s, m, taken, scale);
    }

    // Snap marker, its guides and its hint: guides are the quietest thing drawn -- thin,
    // dashed, translucent -- under the geometry they help place, and not while the pen pulls a
    // handle (the handle is the gesture then).
    if (s.snap.snapped && (s.drag != Drag::None || s.tool != Tool::Select) && !(s.tool == Tool::Curve && s.penDown)) {
        for (const auto& g : s.snap.guides)
            DashedLine (s, m, g.from, g.to, IM_COL32 (155, 93, 229, 150), 1.0f * scale);
        Dot (s, m, s.snap.point, 5.0f * scale, kSnap);
        Note (s, m, s.snap.point, { 14 * scale, -22 * scale }, s.snap.hint, kInk);
    }

    // A face push/pull rides its face: the drawn control stands at the face's current normal
    // (GizmoFrame), so it follows the extrusion instead of the press-time frame.
    if (gizmo && s.drag != Drag::Part) {
        // Through a drag the gumball rides what it moves: carried and turned with it.
        const bool dragging = s.drag == Drag::Handle;
        const edit::Frame f = dragging && !s.facePush ? edit::Moved (s.frame, s.dragTransform) : frame;
        const int lit = dragging ? s.handle : s.typedHandle >= 0 ? s.typedHandle : s.hoverHandle;
        DrawGizmo (s, m, s.typedHandle >= 0 ? s.typedFrame : f, s.gizmo, gizmoSize, lit, dragging, scale);
    }

    DrawMarquee (s, draw, mouse);

    // The tracker: its values are the panel's; beside the pointer only while one is typed.
    std::vector<edit::Dimension> dims;
    s.tracker.Show (TrackerFields (s, dims));
    // A gumball handle clicked, or dragged, offers its number at once: a field beside the handle
    // (or the pointer through a drag), the value selected so the first key typed replaces it.
    DrawTracker (s, mouse, hovered, scale);
    // ⚠️ THE POINTER'S PLACE LICENSES THE CURSOR, NOT IMGUI'S HOVER (2026-10-18). `hovered` above
    // is a PICKING predicate and it is deliberately narrow: `ImGui::IsWindowHovered()` is false
    // while any HUD popup is open -- this view's own right-click menu included -- and while any
    // item elsewhere holds ImGui's ActiveId, and `navigation.blocked` is true through a native
    // navigation and its rebase as well. Each of those used to withdraw the claim for the layouts
    // it lasted and handed the arrow back, which is the flicker the user saw between the tool's
    // cursor and the standard one. The cursor is owed wherever the pointer actually is, which the
    // input layer has already answered (`Input::pointer` with `Input::overHud`): on the view the
    // tool's cursor stands, over a panel, a legend or an open menu the HUD's own does (`overHud`).
    // One cursor per state, said every frame so nothing of Archicad's shows between them: a
    // crosshair while a drawing tool places points, the move cross on a handle and through any
    // drag, the arrow otherwise.
    if (v.pointer && !v.overHud) {
        const bool dragging = s.drag != Drag::None && s.drag != Drag::Marquee; // the profile's are the panel's
        hudshell::OwnCursor (s.tool != Tool::Select && s.drag == Drag::None);
        if (s.tool == Tool::Curve && s.drag == Drag::None) {
            // The pen; over the first anchor of a shape that can close, the pen that closes.
            ImVec2 first;
            const bool closes = s.drawing.size () >= 2 && m.Screen (s.drawing.front (), first) &&
                                std::hypot (first.x - mouse.x, first.y - mouse.y) < kSnapPixels * scale;
            hudshell::OwnCursorAs (uint8_t (closes ? overlayinput::Cursor::PenClose : overlayinput::Cursor::Pen));
        }
        ImGui::SetMouseCursor (dragging || s.hoverHandle >= 0 || s.hoverCorner >= 0 || s.hoverKerb >= 0 ||
                                       s.hoverBezier >= 0
                                   ? ImGuiMouseCursor_ResizeAll
                                   : ImGuiMouseCursor_Arrow);
    }
    if (!s.navigation.projection && (s.drag != Drag::None || s.penDown))
        Navigate (s, m, mouse); // capture the pre-navigation mapping on the Begin frame
    CapturePlane (s);
    draw->PopClipRect ();
}

} // namespace geomsrv::archviz::edithud
