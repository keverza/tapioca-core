#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/OverlayHud.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// The virtual editor's keyboard: Esc's ladder, the tracker's typed values, the tools' and
// modes' letters, history and the clipboard. The view's pointer work is HudVirtualEditor.cpp.
namespace geomsrv::archviz::edithud {

namespace {

using namespace detail;
using Vec3 = edit::Vec3;

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
    else if (s.tool == Tool::Mirror)
        s.tool = Tool::Select; // the selection it was about to mirror stays
    else if (!s.selection.Items ().empty ())
        s.selection.Clear ();
    else if (s.enteredGroup != edit::kNoId)
        s.enteredGroup = edit::kNoId, s.message = "Out of the group: a click picks it whole again.";
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
                else if (key.ctrl && !key.alt && !typing && key.code == 'G')
                    GroupSelection (s, !key.shift);
                else if (key.ctrl && !key.alt && !typing &&
                         (key.code == 'C' || key.code == 'X' || key.code == 'V' || key.code == 'D')) {
                    // The editor's own clipboard: a paste lands centred under the pointer.
                    if (key.code == 'V') {
                        edit::Vec3 at;
                        const bool here = Map { v }.Onto (v.x, v.y, s.plane.origin.z, s.plane.origin, at);
                        PasteClipboard (s, here ? std::optional<edit::Vec3> (at) : std::nullopt);
                    }
                    else if (key.code == 'D')
                        DuplicateSelection (s);
                    else
                        CopySelection (s, key.code == 'X');
                }
                else if (!typing && !key.ctrl && !key.alt && (s.drag != Drag::None || s.planeTyping)) {
                    // No tool or gumball mode changes under a gesture: the handle held is one of
                    // this mode's, and the panel's tools are off until it ends too.
                    if (key.code < 0x80 && key.code != 0 && std::strchr ("VLPBGRSTFEQNOM", int (key.code)) != nullptr)
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

} // namespace geomsrv::archviz::edithud
