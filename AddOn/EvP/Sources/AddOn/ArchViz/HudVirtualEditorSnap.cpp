#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// The virtual editor's drafting aids around the kernel's snap: which snaps are on, the
// edges a gesture has touched (parallels and squares run to them), the point a drag
// grabs an item by, and its edges snapping flush to others.
namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;

void PrepareSnap (const Session& s, edit::SnapQuery& q)
{
    q.modes = s.snapModes;
    q.angleStep = s.angleStep;
    q.references = s.references;
    // A line being drawn is its own first reference: its last leg, to turn square from.
    if (s.drawing.size () >= 2)
        q.references.insert (q.references.begin (), { s.drawing[s.drawing.size () - 2], s.drawing.back () });
}

void Acquire (Session& s, const edit::SnapResult& snap)
{
    if (!snap.onEdge || edit::DistanceXY (snap.edgeFrom, snap.edgeTo) < 1.0e-6)
        return;
    const auto same = [&] (const std::pair<Vec3, Vec3>& e) {
        return edit::Distance (e.first, snap.edgeFrom) < 1.0e-9 && edit::Distance (e.second, snap.edgeTo) < 1.0e-9;
    };
    s.references.erase (std::remove_if (s.references.begin (), s.references.end (), same), s.references.end ());
    s.references.insert (s.references.begin (), { snap.edgeFrom, snap.edgeTo });
    constexpr size_t kKept = 3; // the last few edges touched: more would only add noise
    if (s.references.size () > kKept)
        s.references.resize (kKept);
}

Vec3 GrabPoint (const Session& s, const Map& m, ImVec2 mouse, const edit::Ref& pressed, Vec3 onPlane, float scale)
{
    const edit::Entity* e = edit::Find (Doc (s), pressed.entity);
    if (e == nullptr)
        return onPlane;
    // The item's own vertex under the pointer, top or bottom, is the base point; else the
    // place it was pressed.
    const float reach = 12.0f * scale;
    float best = reach;
    Vec3 grab = onPlane;
    const Vec3 up { 0.0, 0.0, e->kind == edit::Kind::Extrusion ? e->height : 0.0 };
    for (const auto& v : e->vertices)
        for (const Vec3 p : { v.p, v.p + up }) {
            ImVec2 q;
            if (!m.Screen (p, q))
                continue;
            const float d = std::hypot (q.x - mouse.x, q.y - mouse.y);
            if (d < best)
                best = d, grab = p;
        }
    return grab;
}

bool FlushEdges (Session& s, const Map& m, ImVec2 mouse, const std::vector<edit::Ref>& moving, double z,
                 const std::vector<edit::Ref>& exclude, float scale, edit::Transform& t)
{
    if (!s.edgeSnap)
        return false;
    // The moving item's edges, where the move puts them.
    const edit::Document& before = s.edits.Before ();
    std::vector<std::pair<Vec3, Vec3>> edges;
    for (const auto& r : moving) {
        const edit::Entity* e = edit::Find (before, r.entity);
        if (e == nullptr ||
            (r.part != edit::Part::Entity && r.part != edit::Part::Segment && r.part != edit::Part::Face))
            continue;
        for (size_t i = 0; i < edit::SegmentCount (*e); ++i) {
            if (r.part == edit::Part::Segment && e->segments[i].id != r.item)
                continue;
            size_t from = 0, to = 0;
            edit::SegmentEnds (*e, i, from, to);
            edges.push_back ({ e->vertices[from].p + t.translate, e->vertices[to].p + t.translate });
        }
    }
    if (edges.empty ())
        return false;
    edit::SnapQuery q;
    Vec3 at;
    if (!m.Onto (mouse.x, mouse.y, z, {}, at))
        return false;
    q.cursor = at;
    q.tolerance = 10.0 * scale * m.MetresPerPixel (mouse.x, mouse.y, z, at);
    q.plane = edit::SnapPlane (s.capturedPlane.value_or (s.plane));
    q.plane.origin.z = z;
    q.exclude = exclude;
    edit::SnapResult r;
    if (!edit::SnapEdges (before, s.context, q, edges, t.translate, r))
        return false;
    r.point = at;
    s.snap = std::move (r);
    return true;
}

} // namespace geomsrv::archviz::edithud::detail
