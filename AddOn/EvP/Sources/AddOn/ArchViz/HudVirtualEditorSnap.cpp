#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// The virtual editor's drafting aids around the kernel's snap: which snaps are on, the
// edges a gesture has touched (parallels and squares run to them), the point a drag was
// pressed (its captured base point), its edges snapping flush to others, and the view
// screening that makes a snap mouse-close, on screen and near in 3D.
namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;

void PrepareSnap (const Session& s, edit::SnapQuery& q)
{
    // Snapping off suspends every mode without clearing the chosen list; the grid uses the
    // session's spacing, so the kernel's last-resort grid follows the field the panel edits.
    q.modes = s.snapEnabled ? s.snapModes : 0u;
    q.grid = s.snapGrid;
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
    // The base point a drag moves from is the exact place it was pressed: preserving the captured
    // press position means a press near a corner keeps its offset and the item never jumps on the
    // first frame. Snapping then pulls this base point onto its target, so a press exactly on a
    // corner still lands that corner on the target.
    (void) s;
    (void) m;
    (void) mouse;
    (void) pressed;
    (void) scale;
    return onPlane;
}

namespace {
constexpr float kSnapScreenPixels = 10.0f;  // the pointer's snap radius, matching Pointer's kSnapPixels
constexpr float kFlushScreenPixels = 30.0f; // 3x: a flush target is past the pointer's own snap radius,
                                            // or the pointer would already have snapped to it

// The screening both snaps share: the supplied view's world-to-pixel mapping, the pointer, the
// viewport, and the session's 3D proximity cap. A plan view has one scale for the whole plane, so
// the pixel test alone is exact and the cap is left off; in 3D a far-in-Z point can project on top
// of the pointer, and only its world point tells it apart.
void SetScreening (const Session& s, const Map& m, ImVec2 mouse, float pixelTolerance, edit::SnapQuery& q)
{
    edit::SnapQuery::Screening screen;
    screen.project = [&m] (const Vec3& p, double& px, double& py) {
        ImVec2 at;
        if (!m.Screen (p, at))
            return false;
        px = at.x, py = at.y;
        return true;
    };
    screen.mouseX = mouse.x, screen.mouseY = mouse.y;
    screen.pixelTolerance = pixelTolerance;
    screen.viewWidth = m.v.width, screen.viewHeight = m.v.height;
    screen.proximity = m.v.planar                                                 ? 0.0
                       : std::isfinite (s.snapProximity) && s.snapProximity > 0.0 ? s.snapProximity
                                                                                  : 1.0;
    q.screen = std::move (screen);
}
} // namespace

void ScreenPointer (const Session& s, const Map& m, ImVec2 mouse, float scale, edit::SnapQuery& q)
{
    SetScreening (s, m, mouse, kSnapScreenPixels * scale, q);
}

void ScreenFlush (const Session& s, const Map& m, ImVec2 mouse, float scale, edit::SnapQuery& q)
{
    SetScreening (s, m, mouse, kFlushScreenPixels * scale, q);
}

bool FlushEdges (Session& s, const Map& m, ImVec2 mouse, const std::vector<edit::Ref>& moving, double z,
                 const std::vector<edit::Ref>& exclude, float scale, edit::Transform& t)
{
    if (!s.edgeSnap || !s.snapEnabled)
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
    q.plane = edit::SnapPlane (edit::WorldXY (z)); // part drags retain their original geometry datum/world axes
    q.exclude = exclude;
    ScreenFlush (s, m, mouse, scale, q);
    edit::SnapResult r;
    if (!edit::SnapEdges (before, s.context, q, edges, t.translate, r))
        return false;
    r.point = at;
    s.snap = std::move (r);
    return true;
}

} // namespace geomsrv::archviz::edithud::detail
