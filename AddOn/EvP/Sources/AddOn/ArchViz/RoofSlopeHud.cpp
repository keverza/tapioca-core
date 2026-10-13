#include "ArchViz/RoofSlopeHud.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/HudShell.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <charconv>

namespace geomsrv::archviz::roofhud {
namespace rs = roofslope;
namespace rc = roofcanvas;
namespace {
constexpr ImU32 kAccent = IM_COL32 (90, 127, 98, 255), kSecondary = IM_COL32 (131, 153, 135, 255);
constexpr ImU32 kDark = IM_COL32 (55, 82, 61, 255), kSurface = IM_COL32 (240, 245, 241, 255);
constexpr ImU32 kWarning = IM_COL32 (178, 131, 36, 255);
bool Project (const overlayhud::Input& v, const rs::Data& d, rs::Vec p, double z, ImVec2& screen)
{
    p = rs::World (d, p);
    if (v.planar) {
        screen = { float (v.plan[0] * p.x + v.plan[1] * p.y + v.plan[2]),
                   float (v.plan[3] * p.x + v.plan[4] * p.y + v.plan[5]) };
        return std::isfinite (screen.x) && std::isfinite (screen.y);
    }
    return v.project && v.project (p.x, p.y, z, screen.x, screen.y);
}
rc::Anchor At (const rs::Data& d, rs::Vec xy, double z, ImVec2 screen)
{
    const auto p = rs::World (d, xy);
    return { { p.x, p.y, z }, { screen.x, screen.y } };
}
double Distance (ImVec2 a, ImVec2 b)
{
    return std::hypot (a.x - b.x, a.y - b.y);
}
std::string Number (double number, const char* suffix, int decimals = 3)
{
    char text[100];
    std::snprintf (text, sizeof (text), "%.*f%s", decimals, number, suffix);
    return text;
}
void Ask (Session& s, int node, int cell, int side, double value, const std::string& label)
{
    if (!s.number)
        s.number = NumberEdit { s.editor.revision, node, cell, side, value, label, s.token },
        std::snprintf (s.numberText, sizeof (s.numberText), "%.6g", value), s.focusNumber = true;
}
bool Edit (Session& s, rs::Data candidate, const std::string& error)
{
    if (!error.empty ()) {
        s.editor.message = error;
        return false;
    }
    s.editor.Begin ();
    s.editor.Apply (std::move (candidate));
    s.editor.Commit ();
    return true;
}
void Label (ImDrawList* draw, ImVec2 at, const std::string& text, ImU32 color, bool editable = false)
{
    const auto size = ImGui::CalcTextSize (text.c_str ());
    if (editable) {
        draw->AddRectFilled ({ at.x - 4, at.y - 2 }, { at.x + size.x + 4, at.y + size.y + 2 }, kSurface, 2);
        draw->AddRect ({ at.x - 4, at.y - 2 }, { at.x + size.x + 4, at.y + size.y + 2 }, kSecondary, 2);
    }
    draw->AddText (at, color, text.c_str ());
}
struct Hit {
    int kind = 0, cell = -1, node = -1, side = -1, corner = -1, ring = -1, vertex = -1, axis = -1;
    rs::Vec at;
    double z = 0, distance = 12;
};
void ApplyDrag (Session& s, rs::Vec at, double z)
{
    auto& e = s.editor;
    if (at == s.last && s.drag != 4)
        return;
    if (s.drag == 4 && e.selectedNode >= 0 && std::abs (z - e.data.nodes[e.selectedNode].z) < 1e-8)
        return;
    s.last = at;
    rs::Data candidate = e.before;
    std::string error;
    bool valid = false;
    if (s.drag == 1)
        valid = rs::MoveDrain (candidate, e.selectedCell, at, error);
    else if (s.drag == 2) {
        const int sx = (s.corner == 0 || s.corner == 3) ? 0 : 2;
        const int sy = s.corner < 2 ? 1 : 3;
        valid = rs::MoveDivider (candidate, e.selectedCell, sx, at.x, error) &&
                rs::MoveDivider (candidate, e.selectedCell, sy, at.y, error);
    }
    else if (s.drag == 3)
        valid =
            rs::MoveDivider (candidate, e.selectedCell, e.selectedSide, e.selectedSide % 2 == 0 ? at.x : at.y, error);
    else if (s.drag == 4)
        valid = rs::SetElevation (candidate, e.selectedNode, z, error);
    if (valid) {
        e.Apply (std::move (candidate));
        // Divider rebuilding changes node indices. Keep the selected cell, never a
        // dangling former point index, until the next click.
        if (s.drag == 2 || s.drag == 3)
            e.selectedNode = -1;
    }
    else
        e.message = error;
}
bool ElevationAt (const Session& s, const overlayhud::Input& v, ImVec2 mouse, double& z)
{
    ImVec2 p, q;
    const auto& d = s.editor.before;
    if (!Project (v, d, s.anchor, s.startZ, p) || !Project (v, d, s.anchor, s.startZ + 1, q))
        return false;
    const double length = Distance (p, q);
    if (length < 1)
        return false;
    const double dx = (q.x - p.x) / length, dy = (q.y - p.y) / length;
    const double target = (mouse.x - s.startX) * dx + (mouse.y - s.startY) * dy;
    z = s.startZ;
    for (int i = 0; i < 10; ++i) {
        ImVec2 at, next;
        if (!Project (v, d, s.anchor, z, at) || !Project (v, d, s.anchor, z + 0.01, next))
            return false;
        const double residual = (at.x - p.x) * dx + (at.y - p.y) * dy - target;
        if (std::abs (residual) < 0.05)
            return true;
        const double derivative = ((next.x - at.x) * dx + (next.y - at.y) * dy) / 0.01;
        if (std::abs (derivative) < 1e-6)
            return false;
        z -= residual / derivative;
        if (!std::isfinite (z))
            return false;
    }
    return false;
}
void NumberInput (Session& s, const overlayhud::Input& view, float scale)
{
    if (!s.number)
        return;
    const auto edit = *s.number;
    if (edit.revision != s.editor.revision) {
        s.number.reset ();
        s.editor.message = "Design changed while the field was open; value not applied.";
        return;
    }
    const auto& d = s.editor.data;
    rs::Vec xy;
    double z = d.topZ;
    if (edit.node >= 0 && size_t (edit.node) < d.nodes.size ()) {
        xy = d.nodes[edit.node].xy, z = d.nodes[edit.node].z;
    }
    else if (edit.cell >= 0 && size_t (edit.cell) < d.cells.size ()) {
        const auto& c = d.cells[edit.cell];
        xy = d.nodes[c.drain].xy, z = d.nodes[c.drain].z;
        if (edit.side == 2)
            xy = { (c.box[0] + c.box[2]) / 2, c.box[1] };
        if (edit.side == 3)
            xy = { c.box[0], (c.box[1] + c.box[3]) / 2 };
    }
    else {
        s.number.reset ();
        return;
    }
    ImVec2 p;
    if (!Project (view, d, xy, z, p))
        return;
    rc::Scope field (s.canvas, { At (d, xy, z, p) });
    ImGui::SetCursorScreenPos ({ p.x + 8 * scale, p.y + (edit.side == 2 ? -24 : 4) * scale });
    ImGui::SetNextItemWidth (150 * scale);
    if (s.focusNumber)
        ImGui::SetKeyboardFocusHere ();
    s.focusNumber = false;
    const bool enter = ImGui::InputText ("##roofNumber", s.numberText, sizeof (s.numberText),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsKeyPressed (ImGuiKey_Escape)) {
        s.number.reset ();
        return;
    }
    if (enter || ImGui::IsItemDeactivatedAfterEdit ()) {
        std::string text = s.numberText;
        std::replace (text.begin (), text.end (), ',', '.');
        double value = 0;
        const auto parsed = std::from_chars (text.data (), text.data () + text.size (), value);
        if (parsed.ec == std::errc () && parsed.ptr == text.data () + text.size () && std::isfinite (value)) {
            s.number.reset ();
            Answer (s, edit, value);
        }
        else
            s.editor.message = "Enter a finite number; Enter applies and Esc cancels.";
    }
}
} // namespace

bool OntoPlane (const overlayhud::Input& v, const rs::Data& d, float px, float py, double z, rs::Vec seed,
                rs::Vec& local)
{
    local = seed;
    for (int iteration = 0; iteration < 8; ++iteration) {
        ImVec2 p, x, y;
        constexpr double step = 0.1;
        if (!Project (v, d, local, z, p) || !Project (v, d, { local.x + step, local.y }, z, x) ||
            !Project (v, d, { local.x, local.y + step }, z, y))
            return false;
        const double dx = px - p.x, dy = py - p.y;
        if (std::hypot (dx, dy) < 0.05)
            return true;
        const double xx = (x.x - p.x) / step, xy = (x.y - p.y) / step, yx = (y.x - p.x) / step, yy = (y.y - p.y) / step;
        const double determinant = xx * yy - xy * yx;
        if (std::abs (determinant) < 1e-6)
            return false;
        local.x += (dx * yy - dy * yx) / determinant;
        local.y += (dy * xx - dx * xy) / determinant;
        if (!std::isfinite (local.x) || !std::isfinite (local.y))
            return false;
    }
    return false;
}

void OnView (Session& s, const overlayhud::Input& v, bool hovered, float scale)
{
    auto& e = s.editor;
    if (s.drag && s.dragPlanar != v.planar)
        e.Cancel (), s.drag = 0;
    auto& d = e.data;
    auto* draw = ImGui::GetWindowDrawList ();
    s.canvas.list = draw;
    s.canvas.batches.clear ();
    // Retain model furniture even outside this layout's view; the GPU's viewport
    // clips it after reprojection with the actual frame's transform/camera.
    draw->PushClipRect ({ -1e6f, -1e6f }, { 1e6f, 1e6f }, false);
    const ImVec2 mouse = ImGui::GetIO ().MousePos;
    Hit hit;
    bool fieldHovered = false;
    hit.distance *= scale;
    auto offer = [&] (Hit next) {
        ImVec2 p;
        if (Project (v, d, next.at, next.z, p)) {
            const double distance = Distance (p, mouse);
            if (distance < hit.distance)
                next.distance = distance, hit = next;
        }
    };
    // Every rectangle has exactly four editing points and the central X point,
    // irrespective of how many triangles clipping needs at the roof boundary.
    for (int ci = 0; ci < int (d.cells.size ()); ++ci) {
        const auto& c = d.cells[ci];
        for (int j = 0; j < 5; ++j) {
            const int n = j == 4 ? c.drain : c.rim[j];
            const auto& node = d.nodes[n];
            ImVec2 p;
            if (!Project (v, d, node.xy, node.z, p))
                continue;
            const bool selected = e.selectedNode == n;
            const float r = (j == 4 ? 5 : 4) * scale;
            rc::Scope marker (s.canvas, { At (d, node.xy, node.z, p) });
            draw->AddRectFilled ({ p.x - r, p.y - r }, { p.x + r, p.y + r },
                                 j == 4 && !c.drainInside ? kWarning
                                 : selected               ? kDark
                                                          : kAccent,
                                 2 * scale);
            if (hovered && !s.footprint)
                offer ({ j == 4 ? 1 : 2, ci, n, -1, j, -1, -1, -1, node.xy, node.z });
            if (ci == e.selectedCell) {
                const auto text = "Z " + Number (node.z, " m");
                ImVec2 at { p.x + 8 * scale, p.y + 4 * scale };
                if (!s.number || s.number->node != n)
                    Label (draw, at, text, kDark, !node.lockZ);
                const auto size = ImGui::CalcTextSize (text.c_str ());
                fieldHovered = fieldHovered || (hovered && !node.lockZ && mouse.x >= at.x && mouse.y >= at.y &&
                                                mouse.x < at.x + size.x && mouse.y < at.y + size.y);
                if (hovered && !node.lockZ && ImGui::IsMouseClicked (0) && mouse.x >= at.x && mouse.y >= at.y &&
                    mouse.x < at.x + size.x && mouse.y < at.y + size.y)
                    Ask (s, n, ci, -1, node.z, "Point elevation, project-world metres");
            }
        }
        if (ci != e.selectedCell)
            continue;
        ImVec2 points[4];
        bool projected = true;
        for (int j = 0; j < 4; ++j)
            projected = Project (v, d, d.nodes[c.rim[j]].xy, d.nodes[c.rim[j]].z, points[j]) && projected;
        if (!projected)
            continue;
        {
            rc::Scope border (s.canvas, { At (d, d.nodes[c.rim[0]].xy, d.nodes[c.rim[0]].z, points[0]),
                                          At (d, d.nodes[c.rim[1]].xy, d.nodes[c.rim[1]].z, points[1]),
                                          At (d, d.nodes[c.rim[2]].xy, d.nodes[c.rim[2]].z, points[2]),
                                          At (d, d.nodes[c.rim[3]].xy, d.nodes[c.rim[3]].z, points[3]) });
            draw->AddPolyline (points, 4, kAccent, ImDrawFlags_Closed, 2 * scale);
        }
        // Midpoint grips translate the whole aligned divider, never just one edge.
        for (int side = 0; side < 4; ++side) {
            const int a = side == 0 ? 3 : side == 1 ? 0 : side == 2 ? 1 : 2;
            const int b = (a + 1) % 4;
            const auto &na = d.nodes[c.rim[a]], &nb = d.nodes[c.rim[b]];
            rs::Vec mid { (na.xy.x + nb.xy.x) / 2, (na.xy.y + nb.xy.y) / 2 };
            const double z = (na.z + nb.z) / 2;
            ImVec2 p;
            if (Project (v, d, mid, z, p)) {
                rc::Scope grip (s.canvas, { At (d, mid, z, p) });
                draw->AddRectFilled ({ p.x - 4 * scale, p.y - 4 * scale }, { p.x + 4 * scale, p.y + 4 * scale },
                                     kSecondary, 2);
                if (hovered && !s.footprint)
                    offer ({ 3, ci, -1, side, -1, -1, -1, -1, mid, z });
            }
        }
        for (int axis = 0; axis < 2; ++axis) {
            const auto &na = d.nodes[c.rim[0]], &nb = d.nodes[c.rim[axis == 0 ? 1 : 3]];
            const rs::Vec xy { (na.xy.x + nb.xy.x) / 2, (na.xy.y + nb.xy.y) / 2 };
            ImVec2 projectedMid;
            if (!Project (v, d, xy, (na.z + nb.z) / 2, projectedMid))
                continue;
            ImVec2 mid = projectedMid;
            mid.y -= (axis == 0 ? 24 : 0) * scale;
            rc::Scope dimension (s.canvas, { At (d, xy, (na.z + nb.z) / 2, projectedMid) });
            const auto text = Number (c.box[axis + 2] - c.box[axis], " m");
            if (!s.number || s.number->cell != ci || s.number->side != axis + 2)
                Label (draw, mid, text, kDark, true);
            const auto size = ImGui::CalcTextSize (text.c_str ());
            fieldHovered = fieldHovered || (hovered && mouse.x >= mid.x && mouse.y >= mid.y &&
                                            mouse.x < mid.x + size.x && mouse.y < mid.y + size.y);
            if (hovered && ImGui::IsMouseClicked (0) && mouse.x >= mid.x && mouse.y >= mid.y &&
                mouse.x < mid.x + size.x && mouse.y < mid.y + size.y)
                Ask (s, -1, ci, axis + 2, c.box[axis + 2] - c.box[axis],
                     axis == 0 ? "Rectangle width, metres" : "Rectangle height, metres");
        }
        ImVec2 p;
        const auto& drain = d.nodes[c.drain];
        if (Project (v, d, drain.xy, drain.z, p)) {
            rc::Scope area (s.canvas, { At (d, drain.xy, drain.z, p) });
            Label (draw, { p.x + 8 * scale, p.y - 24 * scale }, Number (c.area, " m2", 2), kDark);
        }
    }
    // A selected point's gumball uses the roof's axes, with elevation available in
    // 3D only when Z projects to a stable screen direction. Plan Z uses the text.
    if (e.selectedNode >= 0 && size_t (e.selectedNode) < d.nodes.size () && !s.footprint) {
        const auto& node = d.nodes[e.selectedNode];
        ImVec2 p, x;
        if (Project (v, d, node.xy, node.z, p))
            for (int axis = 0; axis < 3; ++axis) {
                rs::Vec q = node.xy;
                double z = node.z;
                if (axis == 0)
                    q.x += 1;
                if (axis == 1)
                    q.y += 1;
                if (axis == 2)
                    z += 1;
                if (!Project (v, d, q, z, x) || Distance (p, x) < 1)
                    continue;
                const double length = Distance (p, x);
                ImVec2 end { p.x + float ((x.x - p.x) / length) * 48 * scale,
                             p.y + float ((x.y - p.y) / length) * 48 * scale };
                auto anchor = At (d, node.xy, node.z, p);
                const auto start = rs::World (d, node.xy), finish = rs::World (d, q);
                anchor.direction = { finish.x - start.x, finish.y - start.y, z - node.z };
                anchor.screenDirection = { float ((x.x - p.x) / length), float ((x.y - p.y) / length) };
                anchor.turn = true;
                rc::Scope gumball (s.canvas, { anchor });
                draw->AddLine (p, end, axis == 0 ? kAccent : axis == 1 ? kSecondary : kDark, 2 * scale);
                draw->AddCircleFilled (end, 4 * scale, kAccent);
                draw->AddText ({ end.x + 4, end.y }, kDark, axis == 0 ? "U" : axis == 1 ? "V" : "Z");
                if (hovered && Distance (end, mouse) < hit.distance) {
                    hit = { axis == 2                                         ? 4
                            : e.selectedNode == d.cells[e.selectedCell].drain ? 1
                                                                              : 2,
                            e.selectedCell,
                            e.selectedNode,
                            -1,
                            s.corner,
                            -1,
                            -1,
                            axis,
                            node.xy,
                            node.z,
                            Distance (end, mouse) };
                }
            }
    }
    if (s.footprint) {
        for (int r = 0; r < int (d.outline.size ()); ++r)
            for (int i = 0; i < int (d.outline[r].size ()); ++i) {
                const auto a = d.outline[r][i], b = d.outline[r][(i + 1) % d.outline[r].size ()];
                const rs::Vec mid { (a.x + b.x) / 2, (a.y + b.y) / 2 };
                ImVec2 p, q;
                if (Project (v, d, a, d.topZ, p) && Project (v, d, mid, d.topZ, q)) {
                    {
                        rc::Scope vertex (s.canvas, { At (d, a, d.topZ, p) });
                        draw->AddCircleFilled (p, 4 * scale, kAccent);
                    }
                    {
                        rc::Scope edge (s.canvas, { At (d, mid, d.topZ, q) });
                        draw->AddRectFilled ({ q.x - 3 * scale, q.y - 3 * scale }, { q.x + 3 * scale, q.y + 3 * scale },
                                             kSecondary, 2);
                    }
                    if (hovered) {
                        offer ({ 5, -1, -1, -1, -1, r, i, -1, a, d.topZ });
                        offer ({ 6, -1, -1, -1, -1, r, i, -1, mid, d.topZ });
                    }
                }
            }
    }
    if (hovered) {
        hudshell::OwnCursor ();
        ImGui::SetMouseCursor (fieldHovered ? ImGuiMouseCursor_TextInput
                               : hit.kind   ? ImGuiMouseCursor_ResizeAll
                                            : ImGuiMouseCursor_Arrow);
    }
    if (hovered && ImGui::IsMouseClicked (0) && !s.number) {
        if (hit.kind) {
            e.selectedCell = hit.cell, e.selectedNode = hit.node, e.selectedSide = hit.side;
            s.drag = hit.kind, s.corner = hit.corner, s.ring = hit.ring, s.vertex = hit.vertex, s.axis = hit.axis;
            s.dragPlanar = v.planar;
            s.anchor = s.last = hit.at, s.startZ = hit.z, s.startX = mouse.x, s.startY = mouse.y;
            e.Begin ();
        }
        else {
            // Pick a clipped face, not an invisible part of the rectangle envelope.
            for (const auto& t : d.triangles) {
                ImVec2 p[3];
                bool ok = true;
                for (int i = 0; i < 3; ++i)
                    ok = Project (v, d, t.vertices[i].xy, t.vertices[i].z, p[i]) && ok;
                if (!ok)
                    continue;
                auto cross = [] (ImVec2 a, ImVec2 b, ImVec2 c) {
                    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                };
                double a = cross (p[0], p[1], mouse), b = cross (p[1], p[2], mouse), c = cross (p[2], p[0], mouse);
                if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
                    e.selectedCell = t.cell, e.selectedNode = -1, e.selectedSide = -1;
            }
        }
    }
    if (s.drag && ImGui::IsMouseDown (0)) {
        rs::Vec at;
        if (s.drag == 4) {
            double z = 0;
            if (ElevationAt (s, v, mouse, z))
                ApplyDrag (s, s.anchor, z);
            else
                e.message = "Elevation axis is edge-on or unstable. Orbit or click its Z text.";
        }
        else if (OntoPlane (v, e.before, mouse.x, mouse.y, s.startZ, s.anchor, at)) {
            // Axis grips start away from the point. Apply their pointer delta, not
            // the grip tip as an absolute point location.
            rs::Vec start;
            if (s.axis >= 0 && OntoPlane (v, e.before, s.startX, s.startY, s.startZ, s.anchor, start)) {
                at = { s.anchor.x + at.x - start.x, s.anchor.y + at.y - start.y };
                if (s.axis == 0)
                    at.y = s.anchor.y;
                if (s.axis == 1)
                    at.x = s.anchor.x;
            }
            rs::Vec nearby;
            const double tolerance = OntoPlane (v, e.before, mouse.x + 10 * scale, mouse.y, s.startZ, at, nearby)
                                         ? std::hypot (at.x - nearby.x, at.y - nearby.y)
                                         : 0;
            s.snap = rs::Infer (e.before, at, s.anchor, tolerance, e.selectedNode);
            if (!s.snap.label.empty ()) {
                at = s.snap.at;
                Label (draw, { mouse.x + 16 * scale, mouse.y + 16 * scale }, s.snap.label, kDark);
                ImVec2 from, to;
                if (s.snap.guide && Project (v, d, s.snap.guideFrom, s.startZ, from) &&
                    Project (v, d, at, s.startZ, to)) {
                    rc::Scope guide (s.canvas, { At (d, s.snap.guideFrom, s.startZ, from), At (d, at, s.startZ, to) });
                    draw->AddLine (from, to, kSecondary, scale);
                }
            }
            if (s.drag <= 3)
                ApplyDrag (s, at, s.startZ);
            else {
                s.last = at;
                ImVec2 p;
                if (Project (v, d, at, d.topZ, p)) {
                    rc::Scope preview (s.canvas, { At (d, at, d.topZ, p) });
                    draw->AddCircle (p, 8 * scale, kAccent);
                }
            }
        }
        else
            e.message = "This view is edge-on. Orbit the view or use an exact dimension.";
    }
    if (s.drag && ImGui::IsMouseReleased (0)) {
        if (s.drag >= 5 && s.last != s.anchor) {
            rs::Data candidate = e.before;
            std::string error;
            if (s.drag == 5)
                rs::EditOutline (candidate, s.ring, s.vertex, s.last, false, false, error);
            else {
                auto rings = candidate.outline;
                auto& r = rings[s.ring];
                const int next = (s.vertex + 1) % int (r.size ());
                const rs::Vec a = r[s.vertex], b = r[next];
                const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
                const double amount = ((s.last.x - s.anchor.x) * -dy + (s.last.y - s.anchor.y) * dx) / length2;
                r[s.vertex].x -= amount * dy, r[s.vertex].y += amount * dx;
                r[next].x -= amount * dy, r[next].y += amount * dx;
                for (auto& ring : rings)
                    for (auto& p : ring)
                        p = rs::World (candidate, p);
                bool locked = false;
                for (const auto& node : candidate.nodes)
                    locked = locked || node.lockXY || node.lockZ;
                if (locked)
                    error = "Unlock all controls before replanning the virtual footprint.";
                else
                    rs::Plan (rings, candidate.topZ, candidate.targetPercent, candidate.cap, candidate, error);
            }
            if (error.empty ()) {
                e.Apply (std::move (candidate));
                e.message = "Virtual footprint replanned. Drain and height edits reset; source slab unchanged.";
            }
            else
                e.message = error;
        }
        e.Commit ();
        s.drag = 0;
    }
    draw->PopClipRect ();
    NumberInput (s, v, scale);
    if (hovered && ImGui::IsMouseClicked (1)) {
        if (s.drag)
            e.Cancel (), s.drag = 0;
        if (hit.kind) {
            e.selectedCell = hit.cell, e.selectedNode = hit.node, e.selectedSide = hit.side;
            s.ring = hit.ring, s.vertex = hit.vertex, s.last = hit.at;
        }
        hudshell::ClaimRightClick ();
        ImGui::OpenPopup ("##roofSlopeMenu");
    }
    if (ImGui::BeginPopup ("##roofSlopeMenu")) {
        if (e.selectedNode >= 0) {
            const auto& node = d.nodes[e.selectedNode];
            if (ImGui::MenuItem ("Set elevation", nullptr, false, !node.lockZ))
                Ask (s, e.selectedNode, e.selectedCell, -1, node.z, "Point elevation, project-world metres");
            if (ImGui::MenuItem ("Lock XY", nullptr, node.lockXY)) {
                auto copy = d;
                copy.nodes[e.selectedNode].lockXY = !node.lockXY;
                Edit (s, std::move (copy), {});
            }
            if (ImGui::MenuItem ("Lock Z", nullptr, node.lockZ)) {
                auto copy = d;
                copy.nodes[e.selectedNode].lockZ = !node.lockZ;
                Edit (s, std::move (copy), {});
            }
        }
        if (s.footprint && s.ring >= 0) {
            ImGui::TextUnformatted ("Replans virtual roof; slab stays unchanged");
            for (int action = 0; action < 2; ++action)
                if (ImGui::MenuItem (action == 0 ? "Insert vertex here" : "Delete vertex")) {
                    auto copy = d;
                    std::string error;
                    rs::EditOutline (copy, s.ring, s.vertex, s.last, action == 0, action == 1, error);
                    Edit (s, std::move (copy), error);
                }
        }
        if (e.selectedSide >= 0 && ImGui::MenuItem ("Delete virtual divider"))
            DeleteSelection (s);
        if (ImGui::MenuItem ("Undo", nullptr, false, !e.undo.empty ()))
            e.Undo (true);
        if (ImGui::MenuItem ("Redo", nullptr, false, !e.redo.empty ()))
            e.Undo (false);
        if (ImGui::MenuItem ("Finish editing"))
            s.close = true;
        ImGui::EndPopup ();
    }
}

void Panel (Session& s)
{
    auto& e = s.editor;
    const auto& d = e.data;
    ImGui::TextUnformatted ("Roof drainage");
    ImGui::Text ("%zu rectangles   %zu clipped triangles", d.cells.size (), d.triangles.size ());
    ImGui::Text ("Slab top %.3f m   Target %.2f%%", d.topZ, d.targetPercent);
    ImGui::Text ("Cell envelope < %.2f m2", d.cap);
    ImGui::TextWrapped (
        "Four corners + X, clipped to slab. Select a face for live dimensions. Drag a midpoint to resize the grid.");
    if (ImGui::SmallButton ("Undo"))
        e.Undo (true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Redo"))
        e.Undo (false);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Finish"))
        s.close = true;
    ImGui::Checkbox ("Virtual footprint grips", &s.footprint);
    if (s.footprint)
        ImGui::TextWrapped ("Outline edits replan cells and reset manual heights. The source slab is not edited.");
    int outside = 0;
    for (const auto& c : d.cells)
        outside += !c.drainInside;
    if (outside) {
        ImGui::TextWrapped ("Review: %d amber drain centres are on or outside the clipped roof. Move them inside "
                            "before approving drainage.",
                            outside);
    }
    if (e.selectedCell >= 0 && size_t (e.selectedCell) < d.cells.size ()) {
        const auto& c = d.cells[e.selectedCell];
        ImGui::Text ("Rectangle %d%s", e.selectedCell + 1, c.rectangle ? "" : " (clipped)");
        ImGui::Text ("%.3f x %.3f m    Net %.2f m2", c.box[2] - c.box[0], c.box[3] - c.box[1], c.area);
        double low = 1000, high = 0;
        for (const auto& t : d.triangles)
            if (t.cell == e.selectedCell) {
                const double grade = std::hypot (t.gx, t.gy) * 100;
                low = (std::min) (low, grade), high = (std::max) (high, grade);
            }
        ImGui::Text ("Achieved %.2f to %.2f%%", low, high);
        if (ImGui::SmallButton ("Move drain"))
            e.selectedNode = c.drain;
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Push/pull"))
            Ask (s, -1, e.selectedCell, -2, 0, "Face push/pull distance, metres");
        if (ImGui::SmallButton ("Rotate layout"))
            Ask (s, -1, e.selectedCell, -3, d.bearing * 180 / 3.141592653589793,
                 "Layout bearing, degrees. Replans cells and resets manual heights");
    }
    else
        ImGui::TextUnformatted ("No selection. Click a slope face or control point.");
    if (!e.message.empty ()) {
        ImGui::TextWrapped ("Review: %s", e.message.c_str ());
    }
    ImGui::TextWrapped ("Locked: overlay models, Archicad navigates. Wheel zooms; middle drag pans/orbits. Proposed "
                        "drains only. No bake or hydraulic certification.");
}
bool Answer (Session& s, const NumberEdit& edit, double value)
{
    if (s.token != edit.sessionToken || s.editor.revision != edit.revision || !std::isfinite (value)) {
        s.editor.message = "Design changed while the field was open; value not applied.";
        return false;
    }
    auto candidate = s.editor.data;
    std::string error;
    if (edit.node >= 0)
        rs::SetElevation (candidate, edit.node, value, error);
    else if (edit.cell < 0 || size_t (edit.cell) >= candidate.cells.size ())
        error = "Select a roof rectangle before editing its dimensions.";
    else if (edit.side == -2)
        rs::LiftCell (candidate, edit.cell, value, error);
    else if (edit.side == -3)
        rs::Reorient (candidate, value * 3.141592653589793 / 180, error);
    else if ((edit.side == 2 || edit.side == 3) && value > 0) {
        const auto box = candidate.cells[edit.cell].box;
        if (!rs::MoveDivider (candidate, edit.cell, edit.side, box[edit.side - 2] + value, error) &&
            error == "The slab boundary is fixed. Drag an internal divider.") {
            error.clear ();
            rs::MoveDivider (candidate, edit.cell, edit.side - 2, box[edit.side] - value, error);
        }
    }
    else
        error = "Dimensions must be positive.";
    const bool applied = Edit (s, std::move (candidate), error);
    if (applied && edit.side == -3)
        s.editor.selectedCell = s.editor.selectedNode = s.editor.selectedSide = -1, s.ring = s.vertex = -1;
    return applied;
}
bool DeleteSelection (Session& s)
{
    if (s.number)
        return false; // Delete belongs to the active inline field
    if (s.editor.gesture)
        s.editor.Cancel (), s.drag = 0;
    auto candidate = s.editor.data;
    std::string error;
    const bool valid = s.footprint && s.ring >= 0
                           ? rs::EditOutline (candidate, s.ring, s.vertex, s.last, false, true, error)
                           : rs::RemoveDivider (candidate, s.editor.selectedCell, s.editor.selectedSide, error);
    if (!valid) {
        s.editor.message = error;
        return false;
    }
    const bool applied = Edit (s, std::move (candidate), {});
    if (applied)
        s.editor.message = s.footprint && s.ring >= 0
                               ? "Virtual footprint replanned; manual edits reset. Source slab unchanged."
                               : "Virtual rectangles merged; central drain reset. Source slab unchanged.";
    s.editor.selectedCell = s.editor.selectedNode = s.editor.selectedSide = -1;
    s.ring = s.vertex = -1;
    return applied;
}
} // namespace geomsrv::archviz::roofhud
