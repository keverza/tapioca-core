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

} // namespace

void DrawDimensions (Session& s, const Map& m, std::vector<editlayers::Box>& taken, float scale)
{
    if (s.selection.Items ().empty () || !s.drawing.empty ())
        return;
    const edit::Ref ref = s.selection.Items ().front ();
    ImVec2 centre;
    const bool haveCentre = ShapeCentre (s, m, ref, centre);
    const float pad = 5.0f, lineHeight = ImGui::GetTextLineHeight ();
    ImVec2 outward { 0.0f, -1.0f }; // where labels without a line of their own go
    for (const auto& l : editlayers::LiveDimensions (Doc (s), ref)) {
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
