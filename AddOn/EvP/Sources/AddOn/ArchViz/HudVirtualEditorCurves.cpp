#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditShape.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// Curves on the locked view, in a vector editor's manner: the handles of the selected
// anchors (and of a selected segment's ends) drawn as lines with a dot at the end, which
// drags -- Shift in 45 degree steps; a press on a curved segment bends it, the point
// under the pointer following it; and the pen: a click places a corner anchor, a drag
// pulls out the handles of a smooth one, a click on the first anchor closes the shape.
namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;
namespace rc = roofcanvas;

namespace {

constexpr float kHandleDot = 3.5f, kPick = 7.0f, kThreshold = 3.0f;

struct HandleEnd {
    edit::Id entity = edit::kNoId, vertex = edit::kNoId;
    bool out = true;
    Vec3 anchor, end;
    ImVec2 anchorScreen, endScreen;
};

// The handles shown: those of selected anchors, and the ones facing into a selected segment.
std::vector<HandleEnd> Handles (const Session& s, const Map& m)
{
    std::vector<HandleEnd> out;
    const edit::Document& doc = Doc (s);
    const auto add = [&] (const edit::Entity& e, size_t i, bool in, bool outward) {
        const edit::Vertex& v = e.vertices[i];
        for (const bool o : { false, true }) {
            if ((o && !outward) || (!o && !in))
                continue;
            const Vec3 h = o ? v.out : v.in;
            if (std::hypot (h.x, h.y) < 1.0e-12)
                continue;
            HandleEnd x { e.id, v.id, o, v.p, v.p + h, {}, {} };
            if (m.Screen (x.anchor, x.anchorScreen) && m.Screen (x.end, x.endScreen))
                out.push_back (x);
        }
    };
    for (const auto& r : s.selection.Items ()) {
        const edit::Entity* e = edit::Find (doc, r.entity);
        if (e == nullptr)
            continue;
        if (r.part == edit::Part::Vertex) {
            const int i = edit::VertexIndex (*e, r.item);
            if (i >= 0)
                add (*e, size_t (i), true, true);
        }
        else if (r.part == edit::Part::Segment) {
            const int i = edit::SegmentIndex (*e, r.item);
            size_t from = 0, to = 0;
            if (i < 0)
                continue;
            edit::SegmentEnds (*e, size_t (i), from, to);
            add (*e, from, false, true);
            add (*e, to, true, false);
        }
    }
    return out;
}

// Shift: the handle in 45 degree steps about its anchor.
Vec3 Constrain (Vec3 offset, bool shift)
{
    if (!shift)
        return offset;
    const double length = std::hypot (offset.x, offset.y);
    const double step = 3.14159265358979323846 / 4.0;
    const double a = std::round (std::atan2 (offset.y, offset.x) / step) * step;
    return { length * std::cos (a), length * std::sin (a), 0.0 };
}

} // namespace

bool CurveHandles (Session& s, const Map& m, ImVec2 mouse, bool hovered, const edit::PickView& pick, float scale)
{
    if (s.tool != Tool::Select)
        return false;
    const auto handles = Handles (s, m);
    const bool shift = ImGui::GetIO ().KeyShift;
    if (s.drag == Drag::None) {
        s.hoverBezier = -1;
        float best = kPick * scale;
        for (size_t k = 0; hovered && k < handles.size (); ++k) {
            const float d = std::hypot (handles[k].endScreen.x - mouse.x, handles[k].endScreen.y - mouse.y);
            if (d < best)
                best = d, s.hoverBezier = int (k);
        }
        if (s.hoverBezier >= 0)
            s.hoverHandle = -1, s.hoverCorner = -1, s.selection.hover = {};
    }
    bool took = false;
    if (hovered && s.drag == Drag::None && ImGui::IsMouseClicked (0)) {
        if (s.hoverBezier >= 0 && size_t (s.hoverBezier) < handles.size () && s.edits.Begin ()) {
            const HandleEnd& h = handles[size_t (s.hoverBezier)];
            s.drag = Drag::Bezier;
            s.bezierEntity = h.entity, s.bezierVertex = h.vertex, s.bezierOut = h.out;
            took = true;
        }
        else if (!shift) {
            // A press on a curved segment bends it.
            const auto hits = edit::Pick (Doc (s), pick, mouse.x, mouse.y, s.filter, 8.0 * scale);
            if (!hits.empty () && hits.front ().ref.part == edit::Part::Segment) {
                const edit::Ref r = hits.front ().ref;
                const edit::Entity* e = edit::Find (Doc (s), r.entity);
                const int i = e != nullptr ? edit::SegmentIndex (*e, r.item) : -1;
                if (i >= 0 && edit::SegmentCurved (*e, size_t (i))) {
                    Vec3 at;
                    const Vec3 on = edit::SegmentPoint (*e, size_t (i), 0.5);
                    if (m.Onto (mouse.x, mouse.y, on.z, on, at) && s.edits.Begin (r)) {
                        s.selection.Set ({ r });
                        s.drag = Drag::Bend;
                        s.bendEntity = r.entity, s.bendSegment = r.item;
                        s.bendT = edit::NearestOnSegment (*e, size_t (i), at);
                        s.bendStart = at;
                        s.startX = mouse.x, s.startY = mouse.y;
                        took = true;
                    }
                }
            }
        }
    }
    if (ImGui::IsMouseDown (0) && (s.drag == Drag::Bezier || s.drag == Drag::Bend)) {
        took = true;
        edit::Operation op;
        if (s.drag == Drag::Bezier) {
            const edit::Entity* e = edit::Find (s.edits.Before (), s.bezierEntity);
            const int i = e != nullptr ? edit::VertexIndex (*e, s.bezierVertex) : -1;
            Vec3 at;
            if (i >= 0 && m.Onto (mouse.x, mouse.y, e->vertices[size_t (i)].p.z, e->vertices[size_t (i)].p, at)) {
                const Vec3 anchor = e->vertices[size_t (i)].p;
                const Vec3 offset = Constrain ({ at.x - anchor.x, at.y - anchor.y, 0.0 }, shift);
                op = [id = s.bezierEntity, v = s.bezierVertex, out = s.bezierOut, offset] (edit::Document& d,
                                                                                           std::string& error) {
                    return edit::SetHandle (d, id, v, out, offset, false, error);
                };
            }
        }
        else if (std::hypot (mouse.x - s.startX, mouse.y - s.startY) >= kThreshold) { // a click does not bend
            Vec3 at;
            if (m.Onto (mouse.x, mouse.y, s.bendStart.z, s.bendStart, at))
                op = [id = s.bendEntity, seg = s.bendSegment, t = s.bendT,
                      move = Vec3 { at.x - s.bendStart.x, at.y - s.bendStart.y, 0.0 }] (edit::Document& d,
                                                                                        std::string& error) {
                    return edit::BendSegment (d, id, seg, t, move, error);
                };
        }
        if (op && !s.edits.Move (op))
            s.message = s.edits.Message ();
    }
    if (ImGui::IsMouseReleased (0) && (s.drag == Drag::Bezier || s.drag == Drag::Bend)) {
        s.edits.End ();
        s.drag = Drag::None;
        took = true;
    }
    // The handles: a thin line from the anchor and a dot at the end.
    auto* list = s.canvas.list;
    for (size_t k = 0; k < handles.size (); ++k) {
        const HandleEnd& h = handles[k];
        const bool lit =
            int (k) == s.hoverBezier || (s.drag == Drag::Bezier && h.vertex == s.bezierVertex && h.out == s.bezierOut);
        rc::Scope scope (s.canvas, { At (h.anchor, h.anchorScreen), At (h.end, h.endScreen) });
        list->AddLine (h.anchorScreen, h.endScreen, kRule, 1.0f * scale);
        const float r = (lit ? kHandleDot + 1.5f : kHandleDot) * scale;
        list->AddCircleFilled (h.endScreen, r + 1.0f, kPaper);
        list->AddCircleFilled (h.endScreen, r, lit ? kAccent : kHover);
    }
    return took;
}

void PenDrag (Session& s, const Map& m, ImVec2 mouse)
{
    s.drawHandles.resize (s.drawing.size ());
    if (s.tool != Tool::Curve || s.drawing.empty ())
        return;
    if (ImGui::IsMouseClicked (0))
        s.penX = mouse.x, s.penY = mouse.y, s.penDown = true;
    if (ImGui::IsMouseReleased (0))
        s.penDown = false;
    // Past the threshold, the press pulls the new anchor's handles out (smooth: the arriving
    // one mirrors the leaving one).
    if (s.penDown && ImGui::IsMouseDown (0) && std::hypot (mouse.x - s.penX, mouse.y - s.penY) >= kThreshold) {
        const Vec3 anchor = s.drawing.back ();
        Vec3 at;
        if (m.Onto (mouse.x, mouse.y, anchor.z, anchor, at))
            s.drawHandles.back () = Constrain ({ at.x - anchor.x, at.y - anchor.y, 0.0 }, ImGui::GetIO ().KeyShift);
    }
}

void PenPreview (Session& s, const Map& m, Vec3 hover, float scale)
{
    s.drawHandles.resize (s.drawing.size ());
    const auto curve = [&] (Vec3 a, Vec3 ha, Vec3 hb, Vec3 b, ImU32 colour, float width) {
        std::vector<ImVec2> points;
        for (int k = 0; k <= 24; ++k) {
            const double t = k / 24.0, u = 1.0 - t;
            const Vec3 p = a * (u * u * u) + (a + ha) * (3 * u * u * t) + (b + hb) * (3 * u * t * t) + b * (t * t * t);
            ImVec2 q;
            if (m.Screen (p, q))
                points.push_back (q);
        }
        ImVec2 qa;
        if (points.size () > 1 && m.Screen (a, qa)) {
            rc::Scope scope (s.canvas, { At (a, qa) });
            s.canvas.list->AddPolyline (points.data (), int (points.size ()), colour, 0, width);
        }
    };
    for (size_t i = 0; i + 1 < s.drawing.size (); ++i)
        curve (s.drawing[i], s.drawHandles[i], s.drawHandles[i + 1] * -1.0, s.drawing[i + 1], kAccent, 2.5f * scale);
    if (!s.drawing.empty () && !s.penDown)
        curve (s.drawing.back (), s.drawHandles.back (), {}, hover, IM_COL32 (245, 158, 11, 150), 1.0f * scale);
    // The working anchor's handles only: the one being placed, or the last one placed.
    if (!s.drawing.empty ()) {
        const Vec3 p = s.drawing.back (), h = s.drawHandles.back ();
        if (std::hypot (h.x, h.y) > 1.0e-12) {
            Line (s, m, p - h, p + h, kRule, 1.0f * scale);
            Dot (s, m, p + h, kHandleDot * scale, kHover);
            Dot (s, m, p - h, kHandleDot * scale, kHover);
        }
    }
    // Anchors: squares at corners, circles at smooth points; the last one filled.
    for (size_t i = 0; i < s.drawing.size (); ++i) {
        ImVec2 q;
        if (!m.Screen (s.drawing[i], q))
            continue;
        const bool smooth = std::hypot (s.drawHandles[i].x, s.drawHandles[i].y) > 1.0e-12;
        const bool last = i + 1 == s.drawing.size ();
        const float r = 3.5f * scale;
        rc::Scope scope (s.canvas, { At (s.drawing[i], q) });
        auto* list = s.canvas.list;
        if (smooth) {
            list->AddCircleFilled (q, r + 1.0f, kPaper);
            if (last)
                list->AddCircleFilled (q, r, kAccent);
            list->AddCircle (q, r, kAccent, 0, 1.5f * scale);
        }
        else {
            list->AddRectFilled ({ q.x - r - 1, q.y - r - 1 }, { q.x + r + 1, q.y + r + 1 }, kPaper);
            if (last)
                list->AddRectFilled ({ q.x - r, q.y - r }, { q.x + r, q.y + r }, kAccent);
            list->AddRect ({ q.x - r, q.y - r }, { q.x + r, q.y + r }, kAccent, 0.0f, 0, 1.5f * scale);
        }
    }
}

void ApplyPenHandles (edit::Document& d, edit::Id created, const std::vector<Vec3>& handles)
{
    edit::Entity* e = edit::Find (d, created);
    if (e == nullptr)
        return;
    for (size_t i = 0; i < e->vertices.size () && i < handles.size (); ++i) {
        const Vec3 h = handles[i];
        if (std::hypot (h.x, h.y) < 1.0e-12)
            continue;
        e->vertices[i].out = h;
        e->vertices[i].in = h * -1.0;
        e->vertices[i].smooth = true;
    }
}

} // namespace geomsrv::archviz::edithud::detail
