#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditShape.hpp"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::edithud {

edit::Frame GizmoFrame (const Session& s)
{
    edit::Frame f;
    const auto& items = s.selection.Items ();
    size_t n = 0;
    for (const auto& r : items) {
        edit::Vec3 p;
        if (edit::PointOf (s.edits.Doc (), r, p))
            f.origin = f.origin + p, ++n;
    }
    if (n != 0)
        f.origin = f.origin * (1.0 / double (n));
    if (s.pivot && s.pivotFor == items)
        f.origin = *s.pivot;
    // A face pushes or pulls along its normal: the gumball's Z arrow, or Extrude's one handle.
    if (items.size () == 1 && items[0].part == edit::Part::Face)
        if (const edit::Entity* e = edit::Find (s.edits.Doc (), items[0].entity))
            if (edit::FaceFrame (*e, items[0].item, f))
                return f;
    if (s.localAxes) {
        const edit::Frame local = edit::SelectionFrame (s.edits.Doc (), items, f.origin);
        for (int i = 0; i < 3; ++i)
            f.axes[i] = local.axes[i];
    }
    // One frame for every mode: the selection's own box centre (an extrusion measured to its
    // top), so the pivot does not jump when the mode changes and a rotate or scale turns about
    // the place the gumball stands. A pivot the user moved stays where they put it, and
    // Stretch's box and grips stay the selection's.
    const bool custom = s.pivot && s.pivotFor == items;
    if (s.gizmo == edit::GizmoMode::Stretch)
        edit::FitBox (s.edits.Doc (), items, f);
    else if (!custom) {
        edit::Frame box = f;
        if (edit::FitBox (s.edits.Doc (), items, box)) {
            f.origin = box.origin;
            for (int i = 0; i < 3; ++i)
                f.half[i] = box.half[i];
        }
    }
    return f;
}

std::vector<edit::Handle> GizmoHandles (const Session& s, edit::GizmoMode mode)
{
    const auto& doc = s.edits.InGesture () ? s.edits.Before () : s.edits.Doc ();
    const auto frame = s.drag == Drag::Handle ? s.frame : s.typedHandle >= 0 ? s.typedFrame : GizmoFrame (s);
    return edit::Handles (mode, doc, s.selection.Items (), frame);
}

bool GizmoAvailable (const Session& s, edit::GizmoMode mode)
{
    const auto handles = GizmoHandles (s, mode);
    return std::any_of (handles.begin (), handles.end (),
                        [] (const edit::Handle& h) { return h.movable && h.kind != edit::HandleKind::Centre; });
}

int GizmoFaceSlot (const std::vector<edit::Handle>& handles)
{
    for (const auto& h : handles)
        if (h.kind == edit::HandleKind::Face)
            return int (h.index);
    return -1;
}

bool GizmoModeLocked (const Session& s)
{
    // A gesture holds a captured control and a typed value owns the number: a mode change under
    // either would re-name what they work on, so it waits for Enter/Esc or the release.
    return s.landscapeCommand.has_value () || s.drag != Drag::None || s.typedHandle >= 0 ||
           s.tracker.State () == edit::TrackerState::Editing || s.planeTyping || s.profileTyping || s.labelOpen;
}

bool SetGizmoMode (Session& s, edit::GizmoMode mode)
{
    if (GizmoModeLocked (s)) {
        s.message = "Finish the drag or the typed value, or Esc it, before switching the control.";
        return false;
    }
    if (!GizmoAvailable (s, mode)) {
        s.message = "That control does not apply to this selection.";
        return false;
    }
    s.gizmo = mode;
    return true;
}

edit::Handle HeldControl (const Session& s)
{
    return s.heldControl;
}

std::vector<edit::Ref> HeldRefs (const Session& s)
{
    return s.facePush ? std::vector<edit::Ref> { s.edits.Target () } : s.selection.Items ();
}

bool BeginFacePush (Session& s, const detail::Map& view, float x, float y, const edit::Ref& face)
{
    // A face pushes or pulls along its normal through the control the mode gives it -- Extrude's
    // own handle, or the gumball's normal arrow. Its slot is resolved once here (never a
    // sentinel) against this face's own selection and frame, so the drag, its drawing and a
    // mid-drag number all name one control; its refs are the pressed face (HeldRefs), and a
    // click without a drag stays a plain select.
    const edit::Entity* e = edit::Find (s.edits.Doc (), face.entity);
    edit::Frame frame;
    s.startX = x, s.startY = y;
    const bool frameOk = e != nullptr && edit::FaceFrame (*e, face.item, frame);
    const auto handles =
        frameOk ? edit::Handles (s.gizmo, s.edits.Doc (), { face }, frame) : std::vector<edit::Handle> {};
    const int slot = GizmoFaceSlot (handles);
    if (slot < 0 || !handles[size_t (slot)].movable || !view.RayAt (x, y, frame.origin.z, frame.origin, s.startRay) ||
        !edit::NeutralDrag (frame, handles[size_t (slot)], s.startRay).ok) {
        s.message = "That face cannot be pushed or pulled in this view; use its height.";
        return false;
    }
    if (!s.edits.Begin (face)) {
        s.message = s.edits.Message ();
        return false;
    }
    s.drag = Drag::Handle;
    s.dragStarted = false;
    s.dragReleased = false;
    s.dragPointerPause.reset ();
    s.dragValue = 0.0;
    s.handle = slot;
    s.heldControl = handles[size_t (slot)];
    s.facePush = true;
    s.frame = frame;
    s.dragTransform = {};
    s.typedHandle = -1;
    s.message.clear ();
    return true;
}

} // namespace geomsrv::archviz::edithud

// What the virtual editor draws on the locked view beyond picking feedback: the live
// dimensions of the selection and the paths' labels, placed so that no label sits on the
// gizmo or another label. A label is secondary: plain ink, no pill, reading along what it
// measures. Everything is anchored to model points, so the Present reprojects it. The view's
// mapping between model metres and pixels (Map) and the anchored primitives live here too.
namespace geomsrv::archviz::edithud::detail {

void DrawTracker (Session& s, ImVec2 mouse, bool hovered, float scale)
{
    const bool offered = !s.tracker.Items ().empty () &&
                         (s.typedHandle >= 0 || (s.drag == Drag::Handle && s.dragStarted && s.handle >= 0) ||
                          (s.drag == Drag::Part && s.dragStarted && s.dragTypable) ||
                          ((s.drag == Drag::Corner || s.drag == Drag::Kerb) && s.dragStarted));
    if (!hovered || (s.tracker.State () != edit::TrackerState::Editing && !offered))
        return;
    auto* draw = s.canvas.list;
    const ImVec2 base = s.typedHandle >= 0 ? ImVec2 { s.startX, s.startY } : mouse;
    float y = base.y + 20 * scale;
    for (size_t i = 0; i < s.tracker.Items ().size (); ++i) {
        const auto& item = s.tracker.Items ()[i];
        const bool focused = i == s.tracker.Focus ();
        const bool editing = s.tracker.State () == edit::TrackerState::Editing && focused;
        const std::string name = std::string (edit::QuantityName (item.quantity)) + "  ";
        const std::string value =
            editing ? s.tracker.Text () + "_"
                    : editlayers::FormatValue (item.quantity, item.locked ? item.lockedValue : item.value) +
                          (item.locked ? "  (locked)" : "");
        const std::string text = name + value;
        const ImVec2 at { base.x + 18 * scale, y };
        const ImVec2 size = ImGui::CalcTextSize (text.c_str ());
        const ImVec2 lo { at.x - 4 * scale, at.y - 2 * scale },
            hi { at.x + size.x + 4 * scale, at.y + size.y + 2 * scale };
        draw->AddRectFilled (lo, hi, kPaper, 2.0f * scale);
        if (focused) {
            draw->AddRect (lo, hi, kHover, 2.0f * scale, 0, 1.0f * scale); // the field typing goes to
            if (!editing && offered) {
                // Selected, as a field opened for typing is: the first key replaces it.
                const float from = at.x + ImGui::CalcTextSize (name.c_str ()).x;
                draw->AddRectFilled ({ from - 1 * scale, at.y }, { at.x + size.x + 1 * scale, at.y + size.y },
                                     (kHover & 0x00FFFFFFu) | 0x40000000u);
            }
        }
        draw->AddText (at, kInk, text.c_str ());
        y += size.y + 6 * scale;
    }
}

namespace {

using edit::Vec3;
namespace rc = roofcanvas;

ImVec2 Add (ImVec2 a, ImVec2 b, float k = 1.0f)
{
    return { a.x + b.x * k, a.y + b.y * k };
}

// Turns what was drawn from vertex `first` on about the box's centre, to read along it.
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

// A value as plain ink in its box, turned as the box is: secondary to the design, so no pill.
// One that typing can set is kept for a click (`target`); open for typing, it stands on a
// pill with its typed text -- selected, or with a caret after it.
void Ink (Session& s, Vec3 anchor, ImVec2 anchorScreen, const editlayers::Box& box, const std::string& text,
          ImU32 ink = kInk, const editlayers::MarkLabel* target = nullptr)
{
    if (target != nullptr && target->typed)
        s.placed.push_back ({ box, *target });
    rc::Scope scope (s.canvas, { At (anchor, anchorScreen) });
    auto* list = s.canvas.list;
    const int first = list->VtxBuffer.Size;
    if (target == nullptr || !s.labelOpen || !SameLabel (*target, s.labelTarget)) {
        list->AddText ({ box.x0 + 5.0f, box.y0 + 2.0f }, ink, text.c_str ());
        Turn (list, first, box);
        return;
    }
    const float tw = ImGui::CalcTextSize (s.labelText.c_str ()).x, x0 = box.x0 + 5.0f;
    editlayers::Box pill = box;
    pill.x1 = std::max (box.x1, x0 + tw + 8.0f);
    list->AddRectFilled ({ pill.x0, pill.y0 }, { pill.x1, pill.y1 }, kPaper, 3.0f);
    list->AddRect ({ pill.x0, pill.y0 }, { pill.x1, pill.y1 }, kAccent, 3.0f, 0, 1.5f);
    if (s.labelSelected && tw > 0.0f)
        list->AddRectFilled ({ x0 - 1.0f, pill.y0 + 2.0f }, { x0 + tw + 1.0f, pill.y1 - 2.0f },
                             IM_COL32 (179, 212, 252, 255));
    list->AddText ({ x0, box.y0 + 2.0f }, kInk, s.labelText.c_str ());
    if (!s.labelSelected) // the caret, after what is typed
        list->AddLine ({ x0 + tw + 1.5f, pill.y0 + 2.5f }, { x0 + tw + 1.5f, pill.y1 - 2.5f }, kInk, 1.5f);
    Turn (list, first, box);
}

// A label of the view as what typing over it sets: a live dimension's.
editlayers::MarkLabel Typed (const edit::Dimension& value, const std::string& text)
{
    editlayers::MarkLabel t;
    t.typed = true;
    t.value = value;
    t.text = text;
    return t;
}

// The layer's 0xRRGGBBAA as ImGui's colour.
ImU32 FromRgba (uint32_t c)
{
    return IM_COL32 ((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// The selection's centre on screen: dimension lines stand outside the shape.
bool ShapeCentre (const Session& s, const Map& m, const edit::Ref& ref, ImVec2& out)
{
    const edit::Entity* e = edit::Find (Doc (s), ref.entity);
    if (e == nullptr || e->vertices.empty ())
        return false;
    float x = 0.0f, y = 0.0f;
    size_t n = 0;
    for (const auto& v : e->vertices) {
        ImVec2 q;
        if (m.Screen (v.p, q))
            x += q.x, y += q.y, ++n;
    }
    if (n == 0)
        return false;
    out = { x / float (n), y / float (n) };
    return true;
}

ImU32 Lighter (ImU32 c)
{
    const auto mix = [] (unsigned v) { return unsigned (v + (255u - v) * 0.4f); };
    return IM_COL32 (mix (c & 0xFF), mix ((c >> 8) & 0xFF), mix ((c >> 16) & 0xFF), (c >> 24) & 0xFF);
}

ImU32 Alpha (ImU32 c, unsigned a)
{
    return (c & 0x00FFFFFFu) | (a << 24);
}

void Dashed (ImDrawList* list, ImVec2 a, ImVec2 b, ImU32 colour, float width, float dash, float gap)
{
    const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot (dx, dy);
    if (length < 1.0f)
        return;
    for (float t = 0.0f; t < length; t += dash + gap) {
        const float e = std::min (t + dash, length);
        list->AddLine ({ a.x + dx * t / length, a.y + dy * t / length },
                       { a.x + dx * e / length, a.y + dy * e / length }, colour, width);
    }
}

// A stroke with a soft dark shadow under it, so it reads on white and on dark alike.
void Stroke (ImDrawList* list, ImVec2 a, ImVec2 b, ImU32 colour, float width)
{
    list->AddLine (a, b, kShadow, width + 2.0f);
    list->AddLine (a, b, colour, width);
}

void Polygon (ImDrawList* list, const std::vector<ImVec2>& p, ImU32 fill, ImU32 outline, float width)
{
    if (p.size () < 3)
        return;
    list->AddConvexPolyFilled (p.data (), int (p.size ()), fill);
    list->AddPolyline (p.data (), int (p.size ()), kShadow, ImDrawFlags_Closed, width + 1.5f);
    list->AddPolyline (p.data (), int (p.size ()), outline, ImDrawFlags_Closed, width);
}

} // namespace

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
    s.canvas.list->AddText (at, colour, text.c_str ()); // plain ink, as every label
}

void DashedLine (Session& s, const Map& m, Vec3 a, Vec3 b, ImU32 colour, float width)
{
    ImVec2 pa, pb;
    if (!m.Screen (a, pa) || !m.Screen (b, pb))
        return;
    const float dx = pb.x - pa.x, dy = pb.y - pa.y, length = std::hypot (dx, dy);
    if (length < 1.0f)
        return;
    rc::Scope scope (s.canvas, { At (a, pa), At (b, pb) });
    const float dash = 6.0f * width, gap = 4.0f * width;
    for (float t = 0.0f; t < length; t += dash + gap) {
        const float e = std::min (t + dash, length);
        s.canvas.list->AddLine ({ pa.x + dx * t / length, pa.y + dy * t / length },
                                { pa.x + dx * e / length, pa.y + dy * e / length }, colour, width);
    }
}

void DrawGizmo (Session& s, const Map& m, const edit::Frame& f, edit::GizmoMode mode, double size, int active,
                bool dragging, float scale)
{
    using K = edit::HandleKind;
    auto* list = s.canvas.list;
    ImVec2 o;
    if (!m.Screen (f.origin, o))
        return;
    // Stretch: the box its grips stand on, dashed, under them.
    if (mode == edit::GizmoMode::Stretch && !dragging) {
        Vec3 corner[4];
        for (int k = 0; k < 4; ++k)
            corner[k] = f.origin + f.axes[0] * (f.half[0] * ((k == 1 || k == 2) ? 1.0 : -1.0)) +
                        f.axes[1] * (f.half[1] * (k >= 2 ? 1.0 : -1.0));
        for (int k = 0; k < 4; ++k) {
            ImVec2 a, b;
            if (!m.Screen (corner[k], a) || !m.Screen (corner[(k + 1) % 4], b))
                continue;
            rc::Scope scope (s.canvas, { At (corner[k], a), At (corner[(k + 1) % 4], b) });
            Dashed (list, a, b, kRule, 1.0f * scale, 5.0f * scale, 4.0f * scale);
        }
    }
    // Two passes: the gumball's scale boxes sit on its arrows' shafts and are drawn over them.
    const auto handles = GizmoHandles (s, mode);
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& h : handles) {
            if (!h.movable || (h.compact && h.kind == K::ScaleAxis) != (pass == 1))
                continue;
            const bool on = int (h.index) == active;
            if (dragging && !on)
                continue; // a held handle alone, as the move is read off it
            const bool axial = h.axis >= 0 && h.axis < 3 && h.kind != K::Centre && h.kind != K::ScaleUniform;
            const ImU32 base = axial ? kAxis[h.axis] : kRule;
            const ImU32 colour = dragging ? kAccent : on ? Lighter (base) : base;
            const float width = (on ? 3.5f : 2.5f) * scale;
            const Vec3 axis = h.axis >= 0 && h.axis < 3 ? f.axes[h.axis] : f.axes[2];
            // Held, a line handle shows the line it runs on.
            if (dragging && (h.kind == K::Axis || h.kind == K::ScaleAxis || h.kind == K::Side || h.kind == K::Face)) {
                const Vec3 a3 = f.origin - axis * (size * 12.0), b3 = f.origin + axis * (size * 12.0);
                ImVec2 a, b;
                if (m.Screen (a3, a) && m.Screen (b3, b)) {
                    rc::Scope scope (s.canvas, { At (a3, a), At (b3, b) });
                    Dashed (list, a, b, Alpha (base, 150), 1.25f * scale, 8.0f * scale, 5.0f * scale);
                }
            }
            for (const auto& shape : edit::HandleGeometry (f, h, size)) {
                std::vector<ImVec2> p;
                std::vector<std::pair<double, double>> screen;
                for (const auto& q : shape.points) {
                    ImVec2 at;
                    if (!m.Screen (q, at))
                        break;
                    p.push_back (at);
                    screen.push_back ({ at.x, at.y });
                }
                if (p.size () != shape.points.size () || !edit::FacesTheView (screen, shape.closed))
                    continue; // seen edge-on: neither drawn nor picked
                switch (h.kind) {
                    case K::Centre: {
                        rc::Scope scope (s.canvas, { At (shape.points[0], p[0]) });
                        const float r = (on ? 8.0f : 6.5f) * scale;
                        list->AddCircleFilled (p[0], r + 1.5f, kShadow);
                        list->AddCircleFilled (p[0], r, on ? Lighter (kAccent) : kPaper);
                        list->AddCircle (p[0], r, dragging ? kAccent : kRule, 0, 2.0f * scale);
                        break;
                    }
                    case K::Axis:
                    case K::Face: {
                        // A shaft clear of the centre and a filled head, as an arrow reads.
                        const Vec3 a3 = f.origin + axis * (size * 0.2), t3 = f.origin + axis * (size * 0.8);
                        const Vec3 e3 = shape.points.back ();
                        ImVec2 a, t, e;
                        if (!m.Screen (a3, a) || !m.Screen (t3, t) || !m.Screen (e3, e))
                            break;
                        const float dx = e.x - t.x, dy = e.y - t.y, len = std::hypot (dx, dy);
                        if (len < 1.0f)
                            break;
                        const float half = std::max (4.5f * scale, len * 0.38f);
                        const ImVec2 n { -dy / len * half, dx / len * half };
                        rc::Scope scope (s.canvas, { At (a3, a), At (t3, t), At (e3, e) });
                        Stroke (list, a, t, colour, width);
                        Polygon (list, { e, { t.x + n.x, t.y + n.y }, { t.x - n.x, t.y - n.y } }, colour, colour, 1.0f);
                        break;
                    }
                    case K::ScaleAxis:
                        if (shape.points.size () == 2) {
                            const Vec3 a3 = f.origin + axis * (size * 0.2);
                            ImVec2 a;
                            if (!m.Screen (a3, a))
                                break;
                            rc::Scope scope (s.canvas, { At (a3, a), At (shape.points[1], p[1]) });
                            Stroke (list, a, p[1], colour, width);
                        }
                        else {
                            rc::Scope scope (s.canvas, { At (shape.points[0], p[0]) });
                            Polygon (list, p, colour, Lighter (colour), 1.0f); // the cube at its end
                        }
                        break;
                    case K::Plane: {
                        rc::Scope scope (s.canvas, { At (shape.points[0], p[0]), At (shape.points[2], p[2]) });
                        Polygon (list, p, Alpha (colour, on ? 150 : 80), colour, 1.5f * scale);
                        break;
                    }
                    case K::ScaleUniform: {
                        rc::Scope scope (s.canvas, { At (f.origin, o) });
                        // The gumball's is an outline: the pivot dot inside it stays clear.
                        Polygon (list, p, Alpha (colour, on ? 120 : h.compact ? 0 : 50), colour, 2.0f * scale);
                        break;
                    }
                    case K::Side: {
                        rc::Scope scope (s.canvas, { At (shape.points[0], p[0]), At (shape.points[2], p[2]) });
                        Polygon (list, p, colour, on || dragging ? kInk : Lighter (colour), 1.5f * scale);
                        break;
                    }
                    case K::Ring: {
                        rc::Scope scope (s.canvas, { At (f.origin, o) });
                        if (h.compact && p.size () > 2) {
                            // The gumball's arc: its points after the centre the wedge starts from.
                            list->AddPolyline (p.data () + 1, int (p.size ()) - 1, kShadow, ImDrawFlags_None,
                                               width + 2.0f);
                            list->AddPolyline (p.data () + 1, int (p.size ()) - 1, colour, ImDrawFlags_None, width);
                            break;
                        }
                        list->AddPolyline (p.data (), int (p.size ()), kShadow, ImDrawFlags_Closed, width + 2.0f);
                        list->AddPolyline (p.data (), int (p.size ()), colour, ImDrawFlags_Closed, width);
                        break;
                    }
                }
            }
        }
}

void DrawDimensions (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale)
{
    s.placed.clear (); // this frame's, for a click to open
    if (s.selection.Items ().empty () || !s.drawing.empty ())
        return;
    const edit::Ref ref = s.selection.Items ().front ();
    if (QuietPath (s, ref.entity))
        return;
    ImVec2 centre;
    const bool haveCentre = ShapeCentre (s, m, ref, centre);
    const float pad = 5.0f, lineHeight = ImGui::GetTextLineHeight ();
    ImVec2 outward { 0.0f, -1.0f }; // where labels without a line of their own go
    bool endsDrawn = false;
    for (const auto& l : editlayers::LiveDimensions (Doc (s), ref, s.fromEnd)) {
        // Which end a typed length or angle keeps: a pin there, an arrow at the end it moves.
        if (l.ends && !endsDrawn) {
            endsDrawn = true;
            ImVec2 a, b;
            if (m.Screen (l.fixed, a) && m.Screen (l.moving, b)) {
                const float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot (dx, dy);
                rc::Scope scope (s.canvas, { At (l.fixed, a), At (l.moving, b) });
                auto* list = s.canvas.list;
                const float r = 5.0f * scale;
                list->AddRectFilled ({ a.x - r - 1.5f, a.y - r - 1.5f }, { a.x + r + 1.5f, a.y + r + 1.5f }, kPaper,
                                     2.0f);
                list->AddRectFilled ({ a.x - r, a.y - r }, { a.x + r, a.y + r }, kInk, 1.5f);
                if (len > 1.0f) {
                    const ImVec2 u { dx / len, dy / len }, n { -u.y, u.x };
                    const float head = 11.0f * scale, half = 5.0f * scale;
                    const ImVec2 tip { b.x + u.x * (head + 3.0f * scale), b.y + u.y * (head + 3.0f * scale) };
                    const ImVec2 base { b.x + u.x * 3.0f * scale, b.y + u.y * 3.0f * scale };
                    list->AddTriangleFilled (tip, { base.x + n.x * half, base.y + n.y * half },
                                             { base.x - n.x * half, base.y - n.y * half }, kAccent);
                }
            }
        }
        // The angle as an arc about the fixed end, from a dashed reference due east.
        if (l.quantity == edit::Quantity::Angle && l.ends) {
            ImVec2 a;
            if (m.Screen (l.fixed, a)) {
                const double mpp = m.MetresPerPixel (a.x, a.y, l.fixed.z, l.fixed);
                const double radius = 30.0 * scale * mpp;
                const double to = std::atan2 (l.moving.y - l.fixed.y, l.moving.x - l.fixed.x);
                std::vector<ImVec2> arc;
                const int steps = 24;
                for (int k = 0; k <= steps; ++k) {
                    const double t = to * k / steps;
                    ImVec2 q;
                    if (m.Screen (l.fixed + Vec3 { radius * std::cos (t), radius * std::sin (t), 0.0 }, q))
                        arc.push_back (q);
                }
                ImVec2 east;
                if (arc.size () > 1 && m.Screen (l.fixed + Vec3 { radius * 1.4, 0.0, 0.0 }, east)) {
                    rc::Scope scope (s.canvas, { At (l.fixed, a) });
                    s.canvas.list->AddLine (a, east, Alpha (kRule, 150), 1.0f * scale);
                    s.canvas.list->AddPolyline (arc.data (), int (arc.size ()), kAccent, 0, 1.75f * scale);
                    const Vec3 mid = l.fixed + Vec3 { radius * 1.45 * std::cos (to * 0.5),
                                                      radius * 1.45 * std::sin (to * 0.5), 0.0 };
                    ImVec2 q;
                    if (m.Screen (mid, q)) {
                        const ImVec2 size = ImGui::CalcTextSize (l.text.c_str ());
                        float ux = a.y - q.y, uy = q.x - a.x; // along the arc there
                        editlayers::Upright (ux, uy);
                        const auto box =
                            editlayers::PlaceAlong (q.x, q.y, size.x + 10.0f, ImGui::GetTextLineHeight () + 4.0f, ux,
                                                    uy, 0.0f, 0.0f, 0.0f, taken);
                        const auto target = Typed (l.value, l.text);
                        Ink (s, l.fixed, a, box, l.text, kInk, &target);
                    }
                    continue;
                }
            }
        }
        const ImVec2 size = ImGui::CalcTextSize (l.text.c_str ());
        const float w = size.x + 2.0f * pad, h = lineHeight + 4.0f;
        ImVec2 at;
        if (!m.Screen (l.at, at))
            continue;
        if (l.line) {
            ImVec2 a, b;
            if (!m.Screen (l.from, a) || !m.Screen (l.to, b))
                continue;
            const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot (dx, dy);
            if (length < 1.0f)
                continue;
            ImVec2 n { -dy / length, dx / length };
            if (haveCentre && (at.x - centre.x) * n.x + (at.y - centre.y) * n.y < 0.0f)
                n = { -n.x, -n.y };
            outward = n;
            // Outside the shape: extension lines, the dimension line and its ticks, in slate.
            const float offset = 24.0f * scale, gap = 4.0f * scale, tick = 4.0f * scale;
            const ImVec2 a2 = Add (a, n, offset), b2 = Add (b, n, offset);
            const ImVec2 slash { (n.x + dx / length) * 0.7071f * tick, (n.y + dy / length) * 0.7071f * tick };
            if (length > 2.0f * tick) {
                rc::Scope scope (s.canvas, { At (l.from, a), At (l.to, b) });
                auto* list = s.canvas.list;
                list->AddLine (Add (a, n, gap), Add (a2, n, gap), kRule, 1.0f * scale);
                list->AddLine (Add (b, n, gap), Add (b2, n, gap), kRule, 1.0f * scale);
                list->AddLine (a2, b2, kRule, 1.25f * scale);
                list->AddLine (Add (a2, slash, -1.0f), Add (a2, slash), kRule, 1.5f * scale);
                list->AddLine (Add (b2, slash, -1.0f), Add (b2, slash), kRule, 1.5f * scale);
            }
            const ImVec2 mid { (a2.x + b2.x) * 0.5f, (a2.y + b2.y) * 0.5f };
            float ux = dx, uy = dy;
            editlayers::Upright (ux, uy);
            const auto box = editlayers::PlaceAlong (mid.x, mid.y, w, h, ux, uy, n.x, n.y, h * 0.5f, taken);
            const auto target = Typed (l.value, l.text);
            Ink (s, l.at, at, box, l.text, kInk, &target);
            continue;
        }
        // A value of its own: beside its point, outside the shape, clear of the rest.
        ImVec2 dir = outward;
        if (haveCentre && l.quantity != edit::Quantity::Slope) {
            const float ox = at.x - centre.x, oy = at.y - centre.y, d = std::hypot (ox, oy);
            if (d > 1.0f)
                dir = { ox / d, oy / d };
        }
        const float away = (l.quantity == edit::Quantity::Slope ? 24.0f + h : 16.0f) * scale;
        const auto box = editlayers::PlaceLabel (at.x, at.y, w, h, dir.x, dir.y, away + w * 0.25f, taken);
        const auto target = Typed (l.value, l.text);
        Ink (s, l.at, at, box, l.text, kInk, &target);
    }
}

namespace {

// A path dimension's line, thin and in its colour: ticks at both ends of a width, an arrow at
// the arc of a radius.
void DimensionLine (Session& s, const editlayers::MarkLabel& l, ImVec2 a, ImVec2 b, float scale)
{
    const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot (dx, dy);
    if (length < 1.0f)
        return;
    const ImVec2 u { dx / length, dy / length }, n { -u.y, u.x };
    const ImU32 colour = FromRgba (l.rgba);
    rc::Scope scope (s.canvas, { At (l.from, a), At (l.to, b) });
    auto* list = s.canvas.list;
    list->AddLine (a, b, colour, 1.0f * scale);
    if (l.radial) {
        const float head = 8.0f * scale, half = 3.0f * scale;
        const ImVec2 base { b.x - u.x * head, b.y - u.y * head };
        list->AddTriangleFilled (b, { base.x + n.x * half, base.y + n.y * half },
                                 { base.x - n.x * half, base.y - n.y * half }, colour);
        return;
    }
    const float tick = 4.0f * scale;
    const ImVec2 slash { (n.x + u.x) * 0.7071f * tick, (n.y + u.y) * 0.7071f * tick };
    list->AddLine (Add (a, slash, -1.0f), Add (a, slash), colour, 1.5f * scale);
    list->AddLine (Add (b, slash, -1.0f), Add (b, slash), colour, 1.5f * scale);
}

} // namespace

void DrawPathLabels (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale)
{
    const auto& doc = Doc (s);
    if (s.marksRevision != doc.revision) {
        s.marks = editlayers::PathLabels (doc);
        s.marksRevision = doc.revision;
    }
    const float h = ImGui::GetTextLineHeight () + 4.0f;
    for (const auto& l : s.marks) {
        if (QuietPath (s, l.kerbPath != edit::kNoId ? l.kerbPath : l.value.ref.entity))
            continue;
        ImVec2 at, toward, ahead;
        if (!m.Screen (l.at, at))
            continue;
        // A dimension with a line of its own: only where that line is longer than its label,
        // so it shows as the view comes close enough to read it.
        ImVec2 a, b;
        if (l.line && (!m.Screen (l.from, a) || !m.Screen (l.to, b) ||
                       std::hypot (b.x - a.x, b.y - a.y) < ImGui::CalcTextSize (l.text.c_str ()).x + 24.0f * scale))
            continue;
        // Off towards its side on screen, a metre's worth of direction.
        ImVec2 dir { 0.0f, l.dy < 0.0f ? -1.0f : 1.0f };
        if (m.Screen (l.at + l.side, toward)) {
            const float dx = toward.x - at.x, dy = toward.y - at.y, d = std::hypot (dx, dy);
            if (d > 1.0e-3f)
                dir = { dx / d, dy / d };
        }
        // Reading the way what it measures runs.
        float ux = 1.0f, uy = 0.0f;
        if (m.Screen (l.at + l.along, ahead))
            ux = ahead.x - at.x, uy = ahead.y - at.y;
        editlayers::Upright (ux, uy);
        const float w = ImGui::CalcTextSize (l.text.c_str ()).x + 10.0f;
        const float across = std::abs (dir.x * ux + dir.y * uy) * w + std::abs (-dir.x * uy + dir.y * ux) * h;
        bool found = false;
        const float away = l.line ? 0.5f * h + 1.0f * scale : 10.0f * scale + 0.5f * across; // beside its line
        const auto box = editlayers::PlaceAlong (at.x, at.y, w, h, ux, uy, dir.x, dir.y, away, taken, &found);
        if (!found)
            continue;
        if (l.line)
            DimensionLine (s, l, a, b, scale);
        Ink (s, l.at, at, box, l.text, FromRgba (l.textRgba != 0 ? l.textRgba : l.rgba), &l);
        if (l.limitedRadius && s.drag == Drag::None && LabelContains (box, ImGui::GetIO ().MousePos)) {
            char text[128];
            std::snprintf (text, sizeof text, "Fitted radius %.2f m. Requested %.2f m.", l.extent, l.kerb.radius);
            hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Above, text);
        }
    }
}

void DrawHover (Session& s, const Map& m, float scale)
{
    const auto& hover = s.selection.hover;
    if (s.drag != Drag::None || hover.entity == edit::kNoId)
        return;
    const auto run = [&] (const std::vector<Vec3>& p, bool closed, float width) {
        for (size_t k = 0; k + 1 < p.size (); ++k)
            Line (s, m, p[k], p[k + 1], kHover, width * scale);
        if (closed && p.size () > 2)
            Line (s, m, p.back (), p.front (), kHover, width * scale);
    };
    if (const edit::Path* path = edit::FindPath (Doc (s), hover.entity)) {
        if (QuietPath (s, path->id))
            return;
        edit::PathOutline o;
        std::string error;
        if (!edit::Outline (*path, 0.5, o, error))
            return;
        if (hover.part == edit::Part::Range) {
            std::vector<Vec3> left, right;
            for (size_t i = 0; i < o.stations.size (); ++i)
                if (o.range[i] == hover.item)
                    left.push_back (o.left[i]), right.push_back (o.right[i]);
            run (left, false, 3.5f);
            run (right, false, 3.5f);
            return;
        }
        const bool point = hover.part == edit::Part::PathPoint || hover.part == edit::Part::Break;
        run (o.centre, false, point ? 2.0f : 3.5f); // the line it is on, quieter under a point
        Vec3 p;
        if (point && edit::PointOf (Doc (s), hover, p))
            Dot (s, m, p, 7.0f * scale, kHover, false);
        return;
    }
    const edit::Entity* e = edit::Find (Doc (s), hover.entity);
    if (e == nullptr)
        return;
    switch (hover.part) {
        case edit::Part::Segment:
            if (const int i = edit::SegmentIndex (*e, hover.item); i >= 0) // as drawn: a curve along its curve
                run (edit::SegmentPoints (*e, size_t (i)), false, 4.0f);
            return;
        case edit::Part::Entity:
            run (edit::Shape (*e), e->closed, 3.0f);
            return;
        case edit::Part::Face:
            run (edit::FacePolygon (*e, hover.item), true, 3.0f);
            return;
        default: {
            Vec3 p;
            if (edit::PointOf (Doc (s), hover, p))
                Dot (s, m, p, 7.0f * scale, kHover, false);
        }
    }
}

const std::vector<edit::Contour>& Contours (Session& s)
{
    const auto& doc = Doc (s);
    if (!s.contours) {
        s.contourLines.clear ();
        s.contoursRevision = UINT64_MAX;
        return s.contourLines;
    }
    if (s.contoursRevision == doc.revision && s.contoursAsked == s.contourStep && s.contoursPathStep == SurfaceStep (s))
        return s.contourLines;
    s.contoursRevision = doc.revision;
    s.contoursAsked = s.contourStep;
    s.contoursPathStep = SurfaceStep (s);
    // Asked for none: about three divisions over the heights there are, never finer than 5 cm --
    // a crown's few centimetres are not grading.
    double interval = s.contourStep, lo = 0.0, hi = 0.0;
    if (!(interval > 0.0))
        interval = edit::PathHeightRange (doc.paths, lo, hi) ? std::max (edit::NiceStep (hi - lo, 3), 0.05) : 0.0;
    s.contoursInterval = interval;
    s.contourLines =
        edit::SurfaceContours (PavedSurface (s), interval, doc.paths.size () == 1 ? doc.paths[0].id : edit::kNoId);
    return s.contourLines;
}

void DrawContourLabels (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale)
{
    if (!s.contours)
        return;
    if (!Doc (s).paths.empty () && s.quietPaths.size () == Doc (s).paths.size ())
        return;
    ImFont* font = ImGui::GetFont ();
    const float size = ImGui::GetFontSize () * 0.85f, h = size + 2.0f;
    const int decimals = edit::StepDecimals (s.contoursInterval);
    const ImU32 ink = FromRgba (editlayers::Style {}.contourText);
    std::vector<ImVec2> q;
    for (const auto& c : Contours (s)) {
        q.clear ();
        for (const Vec3& p : c.points) {
            ImVec2 v;
            if (!m.Screen (p, v))
                break;
            q.push_back (v);
        }
        if (q.size () != c.points.size () || q.size () < 2)
            continue;
        float total = 0.0f;
        for (size_t k = 0; k + 1 < q.size (); ++k)
            total += std::hypot (q[k + 1].x - q[k].x, q[k + 1].y - q[k].y);
        char text[32];
        std::snprintf (text, sizeof text, "%.*f", decimals, c.z);
        const float w = font->CalcTextSizeA (size, FLT_MAX, 0.0f, text).x + 6.0f;
        if (total < w + 16.0f * scale)
            continue; // its line too short on screen to carry it
        // At the line's middle, reading along it, just beside it.
        float left = total * 0.5f;
        size_t k = 0;
        for (; k + 2 < q.size (); ++k) {
            const float l = std::hypot (q[k + 1].x - q[k].x, q[k + 1].y - q[k].y);
            if (left <= l)
                break;
            left -= l;
        }
        float ux = q[k + 1].x - q[k].x, uy = q[k + 1].y - q[k].y;
        const float l = std::hypot (ux, uy);
        if (l < 1.0e-3f)
            continue;
        const float t = std::clamp (left / l, 0.0f, 1.0f);
        const ImVec2 at { q[k].x + ux * t, q[k].y + uy * t };
        const Vec3 anchor = edit::Lerp (c.points[k], c.points[k + 1], t);
        ux /= l, uy /= l;
        editlayers::Upright (ux, uy);
        const float away = 0.5f * h + 1.0f * scale;
        bool placed = false;
        editlayers::Box box;
        for (const float side : { -1.0f, 1.0f }) {
            const float cx = at.x - uy * away * side, cy = at.y + ux * away * side;
            box = { cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, ux, uy };
            placed = std::none_of (taken.begin (), taken.end (),
                                   [&] (const editlayers::Box& b) { return editlayers::Overlaps (box, b); });
            if (placed)
                break;
        }
        if (!placed)
            continue;
        taken.push_back (box);
        rc::Scope scope (s.canvas, { At (anchor, at) });
        const int first = s.canvas.list->VtxBuffer.Size;
        s.canvas.list->AddText (font, size, { box.x0 + 3.0f, box.y0 + 1.0f }, ink, text);
        Turn (s.canvas.list, first, box);
    }
}

} // namespace geomsrv::archviz::edithud::detail
