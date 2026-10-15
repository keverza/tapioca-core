#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// What the virtual editor draws on the locked view beyond picking feedback: the live
// dimensions of the selection, placed so that no label sits on the gizmo, the tracker or
// another label. Everything is anchored to model points, so the Present reprojects it.
namespace geomsrv::archviz::edithud::detail {

namespace {

using edit::Vec3;
namespace rc = roofcanvas;

ImVec2 Add (ImVec2 a, ImVec2 b, float k = 1.0f)
{
    return { a.x + b.x * k, a.y + b.y * k };
}

// A value on a light pill: dark ink that reads on Archicad's white plan and on 3D alike.
void Pill (Session& s, Vec3 anchor, ImVec2 anchorScreen, const editlayers::Box& box, const std::string& text)
{
    rc::Scope scope (s.canvas, { At (anchor, anchorScreen) });
    s.canvas.list->AddRectFilled ({ box.x0, box.y0 }, { box.x1, box.y1 }, kPaper, 4.0f);
    s.canvas.list->AddRect ({ box.x0, box.y0 }, { box.x1, box.y1 }, IM_COL32 (91, 101, 115, 90), 4.0f, 0, 1.0f);
    s.canvas.list->AddText ({ box.x0 + 5.0f, box.y0 + 2.0f }, kInk, text.c_str ());
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
    for (const auto& h : edit::Handles (mode)) {
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
                    Polygon (list, p, Alpha (colour, on ? 120 : 50), colour, 2.0f * scale);
                    break;
                }
                case K::Side: {
                    rc::Scope scope (s.canvas, { At (shape.points[0], p[0]), At (shape.points[2], p[2]) });
                    Polygon (list, p, colour, on || dragging ? kInk : Lighter (colour), 1.5f * scale);
                    break;
                }
                case K::Ring: {
                    rc::Scope scope (s.canvas, { At (f.origin, o) });
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
    if (s.selection.Items ().empty () || !s.drawing.empty ())
        return;
    const edit::Ref ref = s.selection.Items ().front ();
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
                        const auto box = editlayers::PlaceLabel (
                            q.x, q.y, size.x + 10.0f, ImGui::GetTextLineHeight () + 4.0f, 0.0f, 0.0f, 0.0f, taken);
                        Pill (s, l.fixed, a, box, l.text);
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
            const auto box = editlayers::PlaceLabel (mid.x, mid.y, w, h, n.x, n.y, 0.0f, taken);
            Pill (s, l.at, at, box, l.text);
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
        Pill (s, l.at, at, box, l.text);
    }
}

} // namespace geomsrv::archviz::edithud::detail
