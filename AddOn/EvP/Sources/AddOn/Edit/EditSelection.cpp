#include "Edit/EditSelection.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace geomsrv::edit {

namespace {

// Pick samples every path's outline at this step; the cache is keyed on it.
constexpr double kOutlineStep = 0.5;

struct S2 {
    double x = 0.0, y = 0.0;
};

double SegmentDistance (S2 a, S2 b, S2 p)
{
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
    t = std::clamp (t, 0.0, 1.0);
    return std::hypot (a.x + dx * t - p.x, a.y + dy * t - p.y);
}

bool Inside (const std::vector<S2>& polygon, S2 p)
{
    bool in = false;
    for (size_t i = 0, j = polygon.size () - 1; i < polygon.size (); j = i++) {
        const S2 a = polygon[i], b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

bool Project (const PickView& view, Vec3 p, S2& out)
{
    return view.toScreen && view.toScreen (p, out.x, out.y) && std::isfinite (out.x) && std::isfinite (out.y);
}

bool ProjectAll (const PickView& view, const std::vector<Vec3>& points, std::vector<S2>& out)
{
    out.clear ();
    for (const auto& p : points) {
        S2 s;
        if (!Project (view, p, s))
            return false;
        out.push_back (s);
    }
    return true;
}

int Rank (Part part)
{
    switch (part) {
        case Part::Vertex:
            return 0;
        case Part::Segment:
            return 1;
        case Part::Face:
        case Part::LandscapeFace:
            return 2;
        case Part::Entity:
            return 3;
        case Part::PathPoint:
        case Part::Break:
            return 0; // points, like vertices
        case Part::Range:
            return 1; // picked by its edges, like segments
    }
    return 4;
}

// Every edge an entity shows: its segments, and for an extrusion the top ring and
// the verticals (reported as the segment or vertex they rise from).
struct Edge {
    Vec3 a, b;
    Ref ref;
};

std::vector<Edge> Edges (const Entity& e)
{
    std::vector<Edge> out;
    const Vec3 up { 0.0, 0.0, e.kind == Kind::Extrusion ? e.height : 0.0 };
    for (size_t i = 0; i < e.segments.size (); ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (e, i, from, to);
        const Ref ref { e.id, Part::Segment, e.segments[i].id };
        const auto points = SegmentPoints (e, i); // a curve, piece by piece
        for (size_t k = 0; k + 1 < points.size (); ++k) {
            out.push_back ({ points[k], points[k + 1], ref });
            if (e.kind == Kind::Extrusion)
                out.push_back ({ points[k] + up, points[k + 1] + up, ref });
        }
    }
    if (e.kind == Kind::Extrusion)
        for (const auto& v : e.vertices)
            out.push_back ({ v.p, v.p + up, { e.id, Part::Vertex, v.id } });
    return out;
}

bool Facing (const Entity& e, size_t face, const PickView& view)
{
    Vec3 n;
    return FaceNormal (e, face, n) && Dot (n, view.viewDirection) < 0.0;
}

} // namespace

std::vector<PickHit> Pick (const Document& doc, const PickView& view, double x, double y, uint32_t filter,
                           double radius, PickGeometryCache* cache)
{
    std::vector<PickHit> hits;
    const S2 p { x, y };
    std::map<Id, const tapioca::geometry::LandscapeSource*> arcSources;
    if (doc.landscape)
        for (const auto& source : doc.landscape->sources)
            if (!source.deleted && source.entityId &&
                std::any_of (source.arcAngles.begin (), source.arcAngles.end (), [] (double a) { return a != 0; }))
                arcSources.emplace (source.entityId, &source);
    for (const auto& e : doc.entities) {
        if (!tapioca::geometry::LandscapeLineVisible (doc, e.id))
            continue;
        std::vector<Vec3> analyticOutline;
        if (const auto source = arcSources.find (e.id); source != arcSources.end ()) {
            bool closed = false;
            std::string error;
            if (!tapioca::geometry::LandscapeSourcePreview (doc, *source->second, analyticOutline, closed, error))
                analyticOutline.clear ();
        }
        const Vec3 up { 0.0, 0.0, e.kind == Kind::Extrusion ? e.height : 0.0 };
        // An edge within the radius hits the entity at its distance; the pointer inside a closed
        // outline with no edge near hits it only after every such edge (PickHit::inside).
        double entityBest = -1.0, edgeNearest = -1.0;
        bool inside = false;
        const auto touch = [&] (double d) {
            if (edgeNearest < 0.0 || d < edgeNearest)
                edgeNearest = d;
            if (d <= radius && (entityBest < 0.0 || d < entityBest))
                entityBest = d;
        };
        if (filter & kPickVertex) {
            for (const auto& v : e.vertices) {
                double best = -1.0;
                for (Vec3 q : { v.p, v.p + up }) {
                    S2 s;
                    if (Project (view, q, s)) {
                        const double d = std::hypot (s.x - p.x, s.y - p.y);
                        if (d <= radius && (best < 0.0 || d < best))
                            best = d;
                    }
                    if (e.kind != Kind::Extrusion)
                        break;
                }
                if (best >= 0.0)
                    hits.push_back ({ { e.id, Part::Vertex, v.id }, best });
            }
        }
        auto edges = analyticOutline.empty () ? Edges (e) : std::vector<Edge> {};
        // Explicit landscape arcs use their displayed geometry, not the hidden control
        // chords. Their whole source is movable; existing authored vertices remain pickable.
        if (!analyticOutline.empty ())
            for (size_t i = 0; i + 1 < analyticOutline.size () + (e.closed ? 1u : 0u); ++i)
                edges.push_back ({ analyticOutline[i],
                                   analyticOutline[(i + 1) % analyticOutline.size ()],
                                   { e.id, Part::Entity, 0 } });
        for (const auto& edge : edges) {
            S2 a, b;
            if (!Project (view, edge.a, a) || !Project (view, edge.b, b))
                continue;
            const double d = SegmentDistance (a, b, p);
            touch (d);
            if ((filter & kPickSegment) && edge.ref.part == Part::Segment && d <= radius) {
                auto it =
                    std::find_if (hits.begin (), hits.end (), [&] (const PickHit& h) { return h.ref == edge.ref; });
                if (it == hits.end ())
                    hits.push_back ({ edge.ref, d });
                else
                    it->distance = std::min (it->distance, d);
            }
        }
        std::vector<S2> outline;
        if (e.kind == Kind::Extrusion) {
            for (size_t f = 0; f < FaceCount (e); ++f) {
                if (!Facing (e, f, view) || !ProjectAll (view, FacePolygon (e, f), outline) || !Inside (outline, p))
                    continue;
                inside = true;
                if (filter & kPickFace)
                    hits.push_back ({ { e.id, Part::Face, f }, 0.0 });
            }
        }
        else if (e.closed) {
            std::vector<Vec3> ring = analyticOutline;
            if (ring.empty ())
                for (const auto& v : e.vertices)
                    ring.push_back (v.p);
            if (ProjectAll (view, ring, outline) && Inside (outline, p))
                inside = true;
        }
        if ((filter & kPickEntity) && entityBest >= 0.0)
            hits.push_back ({ { e.id, Part::Entity, 0 }, entityBest });
        else if ((filter & kPickEntity) && inside)
            hits.push_back ({ { e.id, Part::Entity, 0 }, radius + std::max (edgeNearest, 0.0), true });
    }
    // Paths: their outlines are model-space and do not depend on the camera, the cursor or
    // the selection, so a caller-owned cache reuses a path's tessellation while its own
    // value is unchanged; the projection and the hits are recomputed every pick. The
    // revision reset is a clear, and the value compare keeps a stale entry from surviving a
    // document swap that kept the revision or the ids.
    if (cache != nullptr && (cache->step != kOutlineStep || cache->revision != doc.revision)) {
        cache->entries.clear ();
        cache->step = kOutlineStep;
        cache->revision = doc.revision;
    }
    const uint64_t touched = cache != nullptr ? ++cache->tick : 0;
    for (const auto& path : doc.paths) {
        PathOutline built;
        const PathOutline* resolved = nullptr;
        if (cache != nullptr) {
            size_t at = cache->entries.size ();
            for (size_t i = 0; i < cache->entries.size (); ++i)
                if (cache->entries[i].path == path.id && cache->entries[i].input == path) {
                    at = i;
                    break;
                }
            if (at == cache->entries.size ()) {
                PickGeometryCache::Entry entry;
                entry.path = path.id;
                entry.input = path;
                std::string error;
                if (!Outline (path, kOutlineStep, entry.outline, error))
                    continue; // an outline that cannot be built is not cached
                ++cache->builds;
                cache->entries.push_back (std::move (entry));
                at = cache->entries.size () - 1;
            }
            cache->entries[at].touched = touched;
            resolved = &cache->entries[at].outline;
        }
        else {
            std::string error;
            if (!Outline (path, kOutlineStep, built, error))
                continue;
            resolved = &built;
        }
        const PathOutline& o = *resolved;
        const auto near = [&] (Vec3 q, double& d) {
            S2 s;
            if (!Project (view, q, s))
                return false;
            d = std::hypot (s.x - p.x, s.y - p.y);
            return d <= radius;
        };
        double d = 0.0;
        if (filter & kPickVertex) {
            for (size_t i = 0; i < o.points.size (); ++i)
                if (near (o.points[i], d))
                    hits.push_back ({ { path.id, Part::PathPoint, path.points[i].id }, d });
            for (size_t i = 0; i < o.breaks.size (); ++i)
                if (near (o.breaks[i], d))
                    hits.push_back ({ { path.id, Part::Break, path.profile[i].id }, d });
        }
        double centreBest = -1.0;
        for (size_t i = 0; i + 1 < o.stations.size (); ++i) {
            S2 a, b;
            if ((filter & kPickSegment))
                for (const auto* edge : { &o.left, &o.right })
                    if (Project (view, (*edge)[i], a) && Project (view, (*edge)[i + 1], b)) {
                        const double e = SegmentDistance (a, b, p);
                        const Ref r { path.id, Part::Range, o.range[i] };
                        auto it =
                            std::find_if (hits.begin (), hits.end (), [&] (const PickHit& h) { return h.ref == r; });
                        if (e <= radius && it == hits.end ())
                            hits.push_back ({ r, e });
                        else if (e <= radius)
                            it->distance = std::min (it->distance, e);
                    }
            if (Project (view, o.centre[i], a) && Project (view, o.centre[i + 1], b)) {
                const double c = SegmentDistance (a, b, p);
                if (c <= radius && (centreBest < 0.0 || c < centreBest))
                    centreBest = c;
            }
        }
        if ((filter & kPickEntity) && centreBest >= 0.0)
            hits.push_back ({ { path.id, Part::Entity, 0 }, centreBest });
    }
    if (cache != nullptr)
        cache->entries.erase (
            std::remove_if (cache->entries.begin (), cache->entries.end (),
                            [touched] (const PickGeometryCache::Entry& entry) { return entry.touched != touched; }),
            cache->entries.end ());
    std::stable_sort (hits.begin (), hits.end (), [] (const PickHit& a, const PickHit& b) {
        const int ra = Rank (a.ref.part), rb = Rank (b.ref.part);
        return ra != rb ? ra < rb : a.distance < b.distance;
    });
    return hits;
}

const std::vector<Ref>& Selection::Items () const
{
    return items;
}

bool Selection::Contains (const Ref& ref) const
{
    return std::find (items.begin (), items.end (), ref) != items.end ();
}

SelectionKind Selection::Kind () const
{
    if (items.empty ())
        return SelectionKind::None;
    const Part first = items.front ().part;
    for (const auto& r : items)
        if (r.part != first)
            return SelectionKind::Mixed;
    switch (first) {
        case Part::Vertex:
            return SelectionKind::Vertex;
        case Part::Segment:
            return SelectionKind::Segment;
        case Part::Face:
            return SelectionKind::Face;
        case Part::Entity:
            return SelectionKind::Entity;
        case Part::PathPoint:
            return SelectionKind::PathPoint;
        case Part::Break:
            return SelectionKind::Break;
        case Part::Range:
            return SelectionKind::Range;
        case Part::LandscapeFace:
            return SelectionKind::LandscapeFace;
    }
    return SelectionKind::Mixed;
}

void Selection::Clear ()
{
    items.clear ();
    lastHits.clear ();
    cycle = 0;
}

void Selection::Set (const std::vector<Ref>& refs)
{
    items.clear ();
    for (const auto& r : refs)
        if (!Contains (r))
            items.push_back (r);
}

void Selection::Toggle (const Ref& ref)
{
    auto it = std::find (items.begin (), items.end (), ref);
    if (it == items.end ())
        items.push_back (ref);
    else
        items.erase (it);
}

void Selection::Click (const std::vector<PickHit>& hits, double x, double y, bool additive)
{
    const bool same = !hits.empty () && hits.size () == lastHits.size () && std::hypot (x - lastX, y - lastY) <= 3.0 &&
                      std::equal (hits.begin (), hits.end (), lastHits.begin (),
                                  [] (const PickHit& a, const PickHit& b) { return a.ref == b.ref; });
    cycle = same ? (cycle + 1) % hits.size () : 0;
    lastHits = hits;
    lastX = x;
    lastY = y;
    if (hits.empty ()) {
        if (!additive)
            items.clear ();
        return;
    }
    const Ref pick = hits[cycle].ref;
    if (additive)
        Toggle (pick);
    else
        items = { pick };
}

namespace {
// What a screen region selects: `in` says a projected point lies in it, `touches` that a
// projected segment meets it (only asked when `crossing`). Whole objects only when the filter
// admits them -- a marquee over a drawing means its objects, not every vertex and edge of them.
std::vector<Ref> InRegion (const Document& doc, const PickView& view, const std::function<bool (S2)>& in,
                           const std::function<bool (S2, S2)>& touches, bool crossing, uint32_t filter)
{
    if (filter & kPickEntity)
        filter = kPickEntity;
    const auto chain = [&] (const std::vector<Vec3>& points, bool closed) {
        std::vector<S2> s;
        if (!ProjectAll (view, points, s) || s.empty ())
            return false;
        if (!crossing)
            return std::all_of (s.begin (), s.end (), in);
        if (s.size () == 1)
            return in (s[0]);
        for (size_t i = 0; i + 1 < s.size () + (closed ? 1 : 0); ++i)
            if (touches (s[i], s[(i + 1) % s.size ()]))
                return true;
        return false;
    };
    std::vector<Ref> found;
    for (const auto& e : doc.entities) {
        if (!tapioca::geometry::LandscapeLineVisible (doc, e.id))
            continue;
        const Vec3 up { 0.0, 0.0, e.kind == Kind::Extrusion ? e.height : 0.0 };
        if (filter & kPickVertex)
            for (const auto& v : e.vertices)
                if (chain ({ v.p }, false))
                    found.push_back ({ e.id, Part::Vertex, v.id });
        if (filter & kPickSegment)
            for (size_t i = 0; i < e.segments.size (); ++i) {
                size_t from = 0, to = 0;
                SegmentEnds (e, i, from, to);
                if (chain ({ e.vertices[from].p, e.vertices[to].p }, false))
                    found.push_back ({ e.id, Part::Segment, e.segments[i].id });
            }
        if ((filter & kPickFace) && e.kind == Kind::Extrusion)
            for (size_t f = 0; f < FaceCount (e); ++f)
                if (Facing (e, f, view) && chain (FacePolygon (e, f), true))
                    found.push_back ({ e.id, Part::Face, f });
        if (filter & kPickEntity) {
            std::vector<Vec3> all;
            for (const auto& v : e.vertices)
                all.push_back (v.p);
            if (e.kind == Kind::Extrusion)
                for (const auto& v : e.vertices)
                    all.push_back (v.p + up);
            bool hit = chain (all, false);
            if (crossing && !hit)
                for (const auto& edge : Edges (e))
                    if ((hit = chain ({ edge.a, edge.b }, false)))
                        break;
            if (hit)
                found.push_back ({ e.id, Part::Entity, 0 });
        }
    }
    for (const auto& path : doc.paths) {
        PathOutline o;
        std::string error;
        if (!Outline (path, kOutlineStep, o, error))
            continue;
        if (filter & kPickVertex) {
            for (size_t i = 0; i < o.points.size (); ++i)
                if (chain ({ o.points[i] }, false))
                    found.push_back ({ path.id, Part::PathPoint, path.points[i].id });
            for (size_t i = 0; i < o.breaks.size (); ++i)
                if (chain ({ o.breaks[i] }, false))
                    found.push_back ({ path.id, Part::Break, path.profile[i].id });
        }
        if ((filter & kPickEntity) && chain (o.centre, false))
            found.push_back ({ path.id, Part::Entity, 0 });
    }
    return found;
}
} // namespace

void Selection::Marquee (const Document& doc, const PickView& view, double x0, double y0, double x1, double y1,
                         uint32_t filter, bool additive)
{
    const bool crossing = x1 < x0;
    const double lx = std::min (x0, x1), hx = std::max (x0, x1), ly = std::min (y0, y1), hy = std::max (y0, y1);
    const auto in = [=] (S2 s) { return s.x >= lx && s.x <= hx && s.y >= ly && s.y <= hy; };
    // A segment touches the rectangle if an end is inside or it crosses an edge.
    const auto touches = [=] (S2 a, S2 b) {
        if (in (a) || in (b))
            return true;
        const S2 c[4] = { { lx, ly }, { hx, ly }, { hx, hy }, { lx, hy } };
        for (int i = 0; i < 4; ++i) {
            const S2 p = c[i], q = c[(i + 1) % 4];
            const auto side = [] (S2 o, S2 u, S2 v) { return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x); };
            if (side (a, b, p) * side (a, b, q) < 0.0 && side (p, q, a) * side (p, q, b) < 0.0)
                return true;
        }
        return false;
    };
    const auto found = InRegion (doc, view, in, touches, crossing, filter);
    if (!additive)
        items.clear ();
    for (const auto& r : found)
        if (!Contains (r))
            items.push_back (r);
}

void Selection::Lasso (const Document& doc, const PickView& view, const std::vector<std::pair<double, double>>& outline,
                       uint32_t filter, bool additive)
{
    std::vector<S2> polygon;
    for (const auto& [x, y] : outline)
        polygon.push_back ({ x, y });
    std::vector<Ref> found;
    if (polygon.size () >= 3)
        found = InRegion (
            doc, view, [&] (S2 s) { return Inside (polygon, s); }, [] (S2, S2) { return false; }, false, filter);
    if (!additive)
        items.clear ();
    for (const auto& r : found)
        if (!Contains (r))
            items.push_back (r);
}

void Selection::Prune (const Document& doc)
{
    items.erase (std::remove_if (items.begin (), items.end (), [&] (const Ref& r) { return !Exists (doc, r); }),
                 items.end ());
    if (hover.entity != kNoId && !Exists (doc, hover))
        hover = {};
}

std::vector<const Action*> Applicable (const std::vector<Action>& table, const Selection& selection, ViewKind view)
{
    std::vector<const Action*> out;
    const size_t n = selection.Items ().size ();
    for (const auto& a : table) {
        if ((a.kinds & Bit (selection.Kind ())) == 0 || (a.views & Bit (view)) == 0)
            continue;
        if (n < a.minItems || (a.maxItems != 0 && n > a.maxItems))
            continue;
        out.push_back (&a);
    }
    return out;
}

} // namespace geomsrv::edit
