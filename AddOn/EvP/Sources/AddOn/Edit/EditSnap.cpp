#include "Edit/EditSnap.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::edit {

const char* SnapKindName (SnapKind kind)
{
    switch (kind) {
        case SnapKind::None:
            return "";
        case SnapKind::Endpoint:
            return "Endpoint";
        case SnapKind::Midpoint:
            return "Midpoint";
        case SnapKind::Intersection:
            return "Intersection";
        case SnapKind::Centre:
            return "Centre";
        case SnapKind::Perpendicular:
            return "Perpendicular";
        case SnapKind::Nearest:
            return "On edge";
        case SnapKind::Axis:
            return "Axis";
        case SnapKind::Parallel:
            return "Parallel";
        case SnapKind::Angle:
            return "Angle";
        case SnapKind::Extension:
            return "Extension";
        case SnapKind::Grid:
            return "Grid";
        case SnapKind::Terrain:
            return "On terrain";
    }
    return "";
}

void PlaneAxes (const Plane& plane, Vec3& u, Vec3& v)
{
    Vec3 n { 0.0, 0.0, 1.0 };
    Normalize (plane.normal, n);
    if (Normalize (plane.u - n * Dot (plane.u, n), u)) {
        v = Cross (n, u);
        return;
    }
    const Vec3 seed = std::abs (n.x) < 0.9 ? Vec3 { 1.0, 0.0, 0.0 } : Vec3 { 0.0, 1.0, 0.0 };
    Normalize (Cross (n, seed), v);
    Normalize (Cross (v, n), u);
}

Vec3 ProjectToPlane (const Plane& plane, Vec3 p)
{
    Vec3 n { 0.0, 0.0, 1.0 };
    Normalize (plane.normal, n);
    return p - n * Dot (p - plane.origin, n);
}

namespace {

struct P2 {
    double x = 0.0, y = 0.0;
};

struct Frame {
    Vec3 origin, u, v;
    P2 To (Vec3 p) const
    {
        const Vec3 d = p - origin;
        return { Dot (d, u), Dot (d, v) };
    }
};

double Dist (P2 a, P2 b)
{
    return std::hypot (a.x - b.x, a.y - b.y);
}

// Parameter of the point on line a + t (b - a) nearest to p; NaN for a degenerate line.
double LineParameter (P2 a, P2 b, P2 p)
{
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    if (len2 < 1.0e-18)
        return std::nan ("");
    return ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
}

P2 At (P2 a, P2 b, double t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

// Parameters (t on ab, s on cd) of the crossing of two lines; false when parallel.
bool Cross2 (P2 a, P2 b, P2 c, P2 d, double& t, double& s)
{
    const double rx = b.x - a.x, ry = b.y - a.y, qx = d.x - c.x, qy = d.y - c.y;
    const double den = rx * qy - ry * qx;
    if (std::abs (den) < 1.0e-12 * std::max (1.0, std::hypot (rx, ry) * std::hypot (qx, qy)))
        return false;
    t = ((c.x - a.x) * qy - (c.y - a.y) * qx) / den;
    s = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
    return true;
}

struct Seg {
    Vec3 a, b;
    Ref ref;
    std::string context;
    int run = -1; // a piece of a drawn curve: the curve's number (unique in the gather), -1 straight
};

struct Pt {
    Vec3 p;
    SnapKind kind;
    Ref ref;
    std::string context;
};

// A guide line that the cursor is on: an axis through the anchor, a segment's
// extension, or a segment itself. Crossings between two of them are snap points.
struct Line {
    Vec3 a, b;    // a point on it and a second point defining the direction
    bool bounded; // a real segment: crossings must fall inside it
    Guide guide;  // what to draw when the line takes part in a snap
    Ref ref;
    std::string context;
    int run = -1; // as Seg::run: two pieces of one curve do not cross
};

struct Candidate {
    SnapKind kind = SnapKind::None;
    Vec3 p;
    double distance = 0.0;
    Ref ref;
    std::string context;
    std::vector<Guide> guides;
    std::string hint;
    bool onEdge = false;
    Vec3 edgeFrom, edgeTo;
};

bool Excluded (const std::vector<Ref>& exclude, Id entity, Id vertex)
{
    for (const auto& r : exclude) {
        if (r.entity != entity)
            continue;
        // Only a vertex ref is partial. Every other part (the whole entity, a face, a segment, a
        // path range) drops the whole moving item, so a preview position can never be a target.
        if (r.part != Part::Vertex)
            return true;
        if (vertex != kNoId && r.item == vertex)
            return true;
    }
    return false;
}

void Gather (const Document& doc, const SnapContext& context, const SnapQuery& query, std::vector<Pt>& points,
             std::vector<Seg>& segments)
{
    int runBase = 0; // each entity's curves numbered apart from the next one's
    for (const auto& e : doc.entities) {
        if (Excluded (query.exclude, e.id, kNoId))
            continue;
        const Vec3 up { 0.0, 0.0, e.kind == Kind::Extrusion ? e.height : 0.0 };
        const bool solid = e.kind == Kind::Extrusion;
        for (const auto& v : e.vertices) {
            if (Excluded (query.exclude, e.id, v.id))
                continue;
            points.push_back ({ v.p, SnapKind::Endpoint, { e.id, Part::Vertex, v.id }, {} });
            if (solid) {
                points.push_back ({ v.p + up, SnapKind::Endpoint, { e.id, Part::Vertex, v.id }, {} });
                segments.push_back ({ v.p, v.p + up, { e.id, Part::Vertex, v.id }, {} });
            }
        }
        // ⚠️ A ROUNDED OUTLINE IS SNAPPED AS DRAWN (the user, 2026-10-15: guides caught each piece of
        // a fillet as a straight edge). Its straight runs are edges with their middles, its arcs and
        // curves are curves -- on them, never their pieces' middles, extensions or crossings -- and
        // an arc's centre and tangent points are points. The anchors' chords are not drawn: gone.
        if (Rounded (e)) {
            const auto f = Features (e);
            for (const auto& edge : f.edges) {
                if (edge.segment >= e.segments.size ())
                    continue;
                size_t from = 0, to = 0;
                SegmentEnds (e, edge.segment, from, to);
                if (Excluded (query.exclude, e.id, e.vertices[from].id) ||
                    Excluded (query.exclude, e.id, e.vertices[to].id))
                    continue;
                const Ref ref = edge.run < 0 ? Ref { e.id, Part::Segment, e.segments[edge.segment].id }
                                             : Ref { e.id, Part::Entity, 0 };
                segments.push_back ({ edge.a, edge.b, ref, {}, edge.run < 0 ? -1 : runBase + edge.run });
                if (edge.run < 0 && DistanceXY (edge.a, edge.b) > kMinSegment)
                    points.push_back ({ Lerp (edge.a, edge.b, 0.5), SnapKind::Midpoint, ref, {} });
            }
            for (const auto& c : f.centres)
                points.push_back ({ c, SnapKind::Centre, { e.id, Part::Entity, 0 }, {} });
            for (const auto& t : f.tangentPoints)
                points.push_back ({ t, SnapKind::Endpoint, { e.id, Part::Entity, 0 }, "tangent point" });
            runBase += 1000;
        }
        for (size_t i = 0; i < e.segments.size () && !Rounded (e); ++i) {
            size_t from = 0, to = 0;
            SegmentEnds (e, i, from, to);
            if (Excluded (query.exclude, e.id, e.vertices[from].id) ||
                Excluded (query.exclude, e.id, e.vertices[to].id))
                continue;
            const Ref ref { e.id, Part::Segment, e.segments[i].id };
            const Vec3 a = e.vertices[from].p, b = e.vertices[to].p;
            segments.push_back ({ a, b, ref, {} });
            points.push_back ({ Lerp (a, b, 0.5), SnapKind::Midpoint, ref, {} });
            if (solid) {
                segments.push_back ({ a + up, b + up, { e.id, Part::Face, 1 }, {} });
                points.push_back ({ Lerp (a, b, 0.5) + up, SnapKind::Midpoint, ref, {} });
            }
        }
        if (e.closed) {
            Vec3 centre;
            if (PointOf (doc, { e.id, solid ? Part::Face : Part::Entity, solid ? 1u : 0u }, centre))
                points.push_back (
                    { centre, SnapKind::Centre, { e.id, solid ? Part::Face : Part::Entity, solid ? 1u : 0u }, {} });
        }
    }
    // Paths: control points and grade breaks are endpoints, the centreline and both
    // edges are edges. A path any exclusion names is moving and is skipped whole.
    for (const auto& path : doc.paths) {
        bool moving = false;
        for (const auto& r : query.exclude)
            moving |= r.entity == path.id;
        PathOutline built;
        const PathOutline* found = nullptr;
        std::string error;
        if (moving)
            continue;
        if (query.outlines != nullptr)
            found = query.outlines->Get (path, query.pathStep);
        else if (Outline (path, query.pathStep, built, error))
            found = &built;
        if (found == nullptr)
            continue;
        const PathOutline& o = *found;
        for (size_t i = 0; i < o.points.size (); ++i)
            points.push_back ({ o.points[i], SnapKind::Endpoint, { path.id, Part::PathPoint, path.points[i].id }, {} });
        for (size_t i = 0; i < o.breaks.size (); ++i)
            points.push_back ({ o.breaks[i], SnapKind::Endpoint, { path.id, Part::Break, path.profile[i].id }, {} });
        // Bends are curves: their sampled pieces are snapped along, not as edges of their own.
        const auto centreRuns = CurveRuns (o.centre, false, runBase);
        const auto leftRuns = CurveRuns (o.left, false, runBase + 1000);
        const auto rightRuns = CurveRuns (o.right, false, runBase + 2000);
        runBase += 3000;
        for (size_t i = 0; i + 1 < o.stations.size (); ++i) {
            segments.push_back ({ o.centre[i], o.centre[i + 1], { path.id, Part::Entity, 0 }, {}, centreRuns[i] });
            segments.push_back ({ o.left[i], o.left[i + 1], { path.id, Part::Range, o.range[i] }, {}, leftRuns[i] });
            segments.push_back ({ o.right[i], o.right[i + 1], { path.id, Part::Range, o.range[i] }, {}, rightRuns[i] });
        }
    }
    for (const auto& p : context.points)
        points.push_back ({ p.p, SnapKind::Endpoint, {}, p.source });
    for (const auto& s : context.segments) {
        segments.push_back ({ s.a, s.b, {}, s.source });
        points.push_back ({ Lerp (s.a, s.b, 0.5), SnapKind::Midpoint, {}, s.source });
    }
}

bool Better (const Candidate& a, const Candidate& b)
{
    if (a.kind != b.kind)
        return a.kind < b.kind;
    return a.distance < b.distance;
}

// Optional view screening (SnapQuery::screen). The candidate's world point is tested first, so a
// point far from the gesture is rejected before any projection -- a source far in Z is never
// excused by where it happens to land on screen. A segment is judged by its computed candidate,
// never by blanket-rejecting a source whose endpoints happen to be off-screen.
} // namespace

// The gathered targets, and a uniform grid over them in the working plane's frame: each cell
// lists the points in it and the segments whose bounds touch it, in gather order.
struct SnapTargets::Index {
    uint64_t contextKey = 0;
    std::vector<Entity> entities; // what it was gathered from, compared on every use
    std::vector<Path> paths;
    std::vector<Ref> exclude;
    double pathStep = 0.0;
    Plane plane;
    Frame frame;
    std::vector<Pt> points;
    std::vector<Seg> segments;
    double x0 = 0.0, y0 = 0.0, cell = 1.0;
    int nx = 0, ny = 0;
    std::vector<std::vector<uint32_t>> pointCells, segmentCells;

    bool For (const Document& doc, const SnapQuery& q) const
    {
        return exclude == q.exclude && pathStep == q.pathStep && entities == doc.entities && paths == doc.paths &&
               plane.origin == q.plane.origin && plane.normal == q.plane.normal && plane.u == q.plane.u;
    }
    int Column (double x) const
    {
        return std::clamp (int (std::floor ((x - x0) / cell)), 0, nx - 1);
    }
    int Row (double y) const
    {
        return std::clamp (int (std::floor ((y - y0) / cell)), 0, ny - 1);
    }
    // Indices whose cells meet the square of half side `radius` about `c`, ascending: the
    // gather order, so every tie and crossing is met in the order a full scan meets it.
    void Near (const std::vector<std::vector<uint32_t>>& cells, P2 c, double radius, std::vector<uint32_t>& out) const
    {
        out.clear ();
        if (nx == 0 || !(radius >= 0.0) || !std::isfinite (c.x + c.y + radius))
            return;
        const int cx0 = Column (c.x - radius), cx1 = Column (c.x + radius);
        const int cy0 = Row (c.y - radius), cy1 = Row (c.y + radius);
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) {
                const auto& list = cells[size_t (y) * size_t (nx) + size_t (x)];
                out.insert (out.end (), list.begin (), list.end ());
            }
        std::sort (out.begin (), out.end ());
        out.erase (std::unique (out.begin (), out.end ()), out.end ());
    }
};

namespace {

bool Screened (const SnapQuery& query, const Vec3& p)
{
    if (!query.screen)
        return true;
    const SnapQuery::Screening& v = *query.screen;
    if (v.proximity > 0.0 && Distance (p, query.cursor) > v.proximity)
        return false;
    if (!v.project)
        return true;
    double px = 0.0, py = 0.0;
    if (!v.project (p, px, py) || !std::isfinite (px) || !std::isfinite (py))
        return false; // behind the camera: nothing to snap to
    if (v.viewWidth > 0.0 && v.viewHeight > 0.0 && (px < 0.0 || py < 0.0 || px > v.viewWidth || py > v.viewHeight))
        return false;
    if (v.pixelTolerance > 0.0 && std::hypot (px - v.mouseX, py - v.mouseY) > v.pixelTolerance)
        return false;
    return true;
}

} // namespace

SnapTargets::SnapTargets () = default;
SnapTargets::~SnapTargets () = default;
SnapTargets::SnapTargets (SnapTargets&&) noexcept = default;
SnapTargets& SnapTargets::operator= (SnapTargets&&) noexcept = default;

void SnapTargets::Reset ()
{
    index_.reset ();
}

size_t SnapTargets::Points () const
{
    return index_ ? index_->points.size () : 0;
}

size_t SnapTargets::Segments () const
{
    return index_ ? index_->segments.size () : 0;
}

bool SnapTargets::Update (const Document& doc, const SnapContext& context, uint64_t contextKey, const SnapQuery& query)
{
    if (index_ && index_->contextKey == contextKey && index_->For (doc, query))
        return false;
    auto index = std::make_unique<Index> ();
    index->entities = doc.entities;
    index->paths = doc.paths;
    index->contextKey = contextKey;
    index->exclude = query.exclude;
    index->pathStep = query.pathStep;
    index->plane = query.plane;
    index->frame.origin = query.plane.origin;
    PlaneAxes (query.plane, index->frame.u, index->frame.v);
    Gather (doc, context, query, index->points, index->segments);
    // The grid's extent and cell: about one target per cell, at most 512 cells a side.
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    const auto grow = [&] (Vec3 p) {
        const P2 q = index->frame.To (p);
        if (!std::isfinite (q.x) || !std::isfinite (q.y))
            return;
        x0 = std::min (x0, q.x), x1 = std::max (x1, q.x);
        y0 = std::min (y0, q.y), y1 = std::max (y1, q.y);
    };
    for (const auto& p : index->points)
        grow (p.p);
    for (const auto& s : index->segments)
        grow (s.a), grow (s.b);
    if (std::isfinite (x0)) {
        const double w = std::max (x1 - x0, 1.0e-6), h = std::max (y1 - y0, 1.0e-6);
        const double n = double (std::max<size_t> (1, index->points.size () + index->segments.size ()));
        index->cell = std::max ({ std::sqrt (w * h / n), w / 512.0, h / 512.0, 1.0e-3 });
        index->x0 = x0, index->y0 = y0;
        index->nx = int (std::floor (w / index->cell)) + 1;
        index->ny = int (std::floor (h / index->cell)) + 1;
        index->pointCells.assign (size_t (index->nx) * size_t (index->ny), {});
        index->segmentCells.assign (index->pointCells.size (), {});
        for (uint32_t i = 0; i < index->points.size (); ++i) {
            const P2 q = index->frame.To (index->points[i].p);
            if (std::isfinite (q.x) && std::isfinite (q.y))
                index->pointCells[size_t (index->Row (q.y)) * size_t (index->nx) + size_t (index->Column (q.x))]
                    .push_back (i);
        }
        for (uint32_t i = 0; i < index->segments.size (); ++i) {
            const P2 a = index->frame.To (index->segments[i].a), b = index->frame.To (index->segments[i].b);
            if (!std::isfinite (a.x + a.y + b.x + b.y))
                continue;
            for (int y = index->Row (std::min (a.y, b.y)); y <= index->Row (std::max (a.y, b.y)); ++y)
                for (int x = index->Column (std::min (a.x, b.x)); x <= index->Column (std::max (a.x, b.x)); ++x)
                    index->segmentCells[size_t (y) * size_t (index->nx) + size_t (x)].push_back (i);
        }
    }
    index_ = std::move (index);
    ++builds;
    return true;
}

SnapResult Snap (const Document& doc, const SnapContext& context, const SnapQuery& query)
{
    SnapResult result;
    result.point = query.cursor;
    Frame frame;
    frame.origin = query.plane.origin;
    PlaneAxes (query.plane, frame.u, frame.v);
    const P2 cursor = frame.To (query.cursor);
    const double tol = query.tolerance;
    const bool on = std::isfinite (tol) && tol > 0.0;
    const auto has = [&] (uint32_t mode) { return (query.modes & mode) != 0; };

    std::vector<Pt> gatheredPoints;
    std::vector<Seg> gatheredSegments;
    const std::vector<Pt>* points = &gatheredPoints;
    const std::vector<Seg>* segments = &gatheredSegments;
    // With an index, only the targets that can be offered: points within the tolerance, and
    // segments within it or -- for an extension -- with an end within the extension's reach.
    // Visited in gather order, the result is the full scan's.
    const SnapTargets::Index* index =
        query.targets != nullptr && query.targets->Built () != nullptr && query.targets->Built ()->For (doc, query)
            ? query.targets->Built ()
            : nullptr;
    std::vector<uint32_t> nearPoints, nearSegments;
    constexpr uint32_t sceneModes = kSnapEndpoint | kSnapMidpoint | kSnapIntersection | kSnapCentre |
                                    kSnapPerpendicular | kSnapNearest | kSnapExtension;
    if (on && (query.modes & sceneModes) != 0) {
        if (index != nullptr) {
            points = &index->points;
            segments = &index->segments;
            const P2 c = index->frame.To (query.cursor);
            const double reach =
                has (kSnapExtension) ? (query.extensionReach > 0.0 ? query.extensionReach : 25.0 * tol) : 0.0;
            index->Near (index->pointCells, c, tol, nearPoints);
            index->Near (index->segmentCells, c, std::max (tol, reach), nearSegments);
        }
        else
            Gather (doc, context, query, gatheredPoints, gatheredSegments);
    }
    // Every gathered target, or only the indexed ones near the cursor, in gather order.
    const auto visit = [&] (const auto& all, const std::vector<uint32_t>& near, const auto& body) {
        if (index == nullptr)
            for (const auto& item : all)
                body (item);
        else
            for (uint32_t i : near)
                body (all[i]);
    };

    std::optional<Candidate> best;
    const auto offer = [&] (Candidate c) {
        if (!Screened (query, c.p))
            return;
        if (c.distance <= tol && (!best || Better (c, *best)))
            best = std::move (c);
    };

    // Notable points. A middle is offered with its edge, below, so it can name the edge.
    visit (*points, nearPoints, [&] (const Pt& p) {
        if (p.kind == SnapKind::Midpoint)
            return;
        const uint32_t mode = p.kind == SnapKind::Endpoint   ? kSnapEndpoint
                              : p.kind == SnapKind::Midpoint ? kSnapMidpoint
                                                             : kSnapCentre;
        if (has (mode))
            offer ({ p.kind, p.p, Dist (frame.To (p.p), cursor), p.ref, p.context, {}, SnapKindName (p.kind) });
    });

    // Segments near the cursor: nearest point, perpendicular foot from the anchor.
    std::vector<Line> lines;
    visit (*segments, nearSegments, [&] (const Seg& s) {
        const P2 a = frame.To (s.a), b = frame.To (s.b);
        const double t = LineParameter (a, b, cursor);
        if (std::isnan (t))
            return; // seen edge-on: it is a point in this projection
        const double clamped = std::clamp (t, 0.0, 1.0);
        const double near = Dist (At (a, b, clamped), cursor);
        // On a curve, the edge it reports is the tangent there, a few tolerances long.
        Vec3 ea = s.a, eb = s.b;
        if (s.run >= 0) {
            Vec3 d;
            if (Normalize (s.b - s.a, d)) {
                const Vec3 at = Lerp (s.a, s.b, clamped);
                ea = at - d * (5.0 * tol), eb = at + d * (5.0 * tol);
            }
        }
        if (near <= tol) {
            lines.push_back ({ s.a, s.b, true, { s.a, s.b, SnapKind::Nearest }, s.ref, s.context, s.run });
            if (has (kSnapNearest))
                offer ({ SnapKind::Nearest,
                         Lerp (s.a, s.b, clamped),
                         near,
                         s.ref,
                         s.context,
                         {},
                         SnapKindName (SnapKind::Nearest),
                         true,
                         ea,
                         eb });
            // Its middle, when that is the snap, names the edge it halves too -- a straight
            // edge's: a curve's pieces have no middles worth a snap.
            if (has (kSnapMidpoint) && s.run < 0) {
                const Vec3 mid = Lerp (s.a, s.b, 0.5);
                offer ({ SnapKind::Midpoint,
                         mid,
                         Dist (frame.To (mid), cursor),
                         s.ref,
                         s.context,
                         {},
                         SnapKindName (SnapKind::Midpoint),
                         true,
                         s.a,
                         s.b });
            }
        }
        if (query.anchor && has (kSnapPerpendicular)) {
            const double f = LineParameter (a, b, frame.To (*query.anchor));
            if (f >= 0.0 && f <= 1.0) {
                const Vec3 foot = Lerp (s.a, s.b, f);
                offer ({ SnapKind::Perpendicular,
                         foot,
                         Dist (frame.To (foot), cursor),
                         s.ref,
                         s.context,
                         { { *query.anchor, foot, SnapKind::Perpendicular } },
                         SnapKindName (SnapKind::Perpendicular),
                         true,
                         s.a,
                         s.b });
            }
        }
        // The segment's extension past the end nearer the cursor -- a straight segment's.
        if (has (kSnapExtension) && s.run < 0) {
            const double reach = query.extensionReach > 0.0 ? query.extensionReach : 25.0 * tol;
            const bool pastEnd = t > 1.0 && Dist (b, cursor) <= reach;
            const bool pastStart = t < 0.0 && Dist (a, cursor) <= reach;
            if (pastEnd || pastStart) {
                const Vec3 p = Lerp (s.a, s.b, t);
                const double d = Dist (At (a, b, t), cursor);
                const Guide g { pastEnd ? s.b : s.a, p, SnapKind::Extension };
                if (d <= tol)
                    lines.push_back ({ s.a, s.b, false, g, s.ref, s.context });
                offer ({ SnapKind::Extension,
                         p,
                         d,
                         s.ref,
                         s.context,
                         { g },
                         SnapKindName (SnapKind::Extension),
                         true,
                         s.a,
                         s.b });
            }
        }
    });

    // Axis guides through the anchor.
    if (query.anchor && has (kSnapAxis)) {
        const bool level = std::abs (frame.u.z) < 1.0e-9 && std::abs (frame.v.z) < 1.0e-9;
        const Vec3 axes[2] = { frame.u, frame.v };
        const char* names[2] = { level ? "X axis" : "Plane axis", level ? "Y axis" : "Plane axis" };
        for (int i = 0; i < 2; ++i) {
            const Vec3 a = *query.anchor, b = a + axes[i];
            const double t = LineParameter (frame.To (a), frame.To (b), cursor);
            const Vec3 p = Lerp (a, b, t);
            const double d = Dist (frame.To (p), cursor);
            const Guide g { a, p, SnapKind::Axis };
            if (d <= tol)
                lines.push_back ({ a, b, false, g, {}, {} });
            offer ({ SnapKind::Axis, p, d, {}, {}, { g }, names[i] });
        }
    }

    // Parallel and square to the edges the gesture touched, through the anchor; each one's
    // edge is drawn with it, so it is plain what the line runs parallel to.
    if (query.anchor && has (kSnapParallel)) {
        const Vec3 a = *query.anchor;
        for (const auto& [ra, rb] : query.references) {
            const P2 p0 = frame.To (ra), p1 = frame.To (rb);
            const double dx = p1.x - p0.x, dy = p1.y - p0.y, len = std::hypot (dx, dy);
            if (len < 1.0e-9)
                continue;
            for (const bool square : { false, true }) {
                const Vec3 dir =
                    square ? frame.u * (-dy / len) + frame.v * (dx / len) : frame.u * (dx / len) + frame.v * (dy / len);
                const Vec3 b = a + dir;
                const double t = LineParameter (frame.To (a), frame.To (b), cursor);
                if (std::isnan (t))
                    continue;
                const Vec3 p = Lerp (a, b, t);
                const double d = Dist (frame.To (p), cursor);
                const Guide g { a, p, SnapKind::Parallel };
                if (d <= tol)
                    lines.push_back ({ a, b, false, g, {}, {} });
                offer ({ SnapKind::Parallel,
                         p,
                         d,
                         {},
                         {},
                         { g, { ra, rb, SnapKind::Parallel } },
                         square ? "Square to edge" : "Parallel to edge" });
            }
        }
    }

    // Construction angles through the anchor, beyond the axes; no crossings of their own.
    if (query.anchor && has (kSnapAngle) && query.angleStep > 0.0 && query.angleStep < 90.0) {
        const Vec3 a = *query.anchor;
        for (double deg = query.angleStep; deg < 180.0 - 1.0e-9; deg += query.angleStep) {
            if (std::abs (std::remainder (deg, 90.0)) < 1.0e-9)
                continue; // the axes are the axis guides'
            const double r = deg * 3.14159265358979323846 / 180.0;
            const Vec3 b = a + frame.u * std::cos (r) + frame.v * std::sin (r);
            const double t = LineParameter (frame.To (a), frame.To (b), cursor);
            const Vec3 p = Lerp (a, b, t);
            char hint[16];
            std::snprintf (hint, sizeof hint, "%.0f\xC2\xB0", t >= 0.0 ? deg : deg - 180.0);
            offer ({ SnapKind::Angle, p, Dist (frame.To (p), cursor), {}, {}, { { a, p, SnapKind::Angle } }, hint });
        }
    }

    // Crossings of two lines the cursor is on: real intersections, and guide
    // crossings (axis x edge, extension x axis, ...).
    if (has (kSnapIntersection)) {
        for (size_t i = 0; i < lines.size (); ++i)
            for (size_t j = i + 1; j < lines.size (); ++j) {
                const Line &l = lines[i], &m = lines[j];
                if (l.run >= 0 && l.run == m.run)
                    continue; // two pieces of one curve: where they meet is just how it is drawn
                double t = 0.0, s = 0.0;
                if (!Cross2 (frame.To (l.a), frame.To (l.b), frame.To (m.a), frame.To (m.b), t, s))
                    continue;
                if ((l.bounded && (t < 0.0 || t > 1.0)) || (m.bounded && (s < 0.0 || s > 1.0)))
                    continue;
                const Vec3 p = Lerp (l.a, l.b, t);
                // A projected crossing needs both source points in reach, not just the first
                // line's: a distant second line must not manufacture a local intersection.
                if (!Screened (query, Lerp (m.a, m.b, s)))
                    continue;
                Candidate c { SnapKind::Intersection, p, Dist (frame.To (p), cursor), l.ref, l.context, {},
                              "Intersection" };
                for (const Line* g : { &l, &m })
                    if (!g->bounded)
                        c.guides.push_back ({ g->guide.from, p, g->guide.kind });
                offer (std::move (c));
            }
    }

    if (best) {
        result.snapped = true;
        result.kind = best->kind;
        result.point = best->p;
        result.source = best->ref;
        result.contextSource = best->context;
        result.guides = std::move (best->guides);
        result.hint = best->hint;
        result.onEdge = best->onEdge;
        result.edgeFrom = best->edgeFrom;
        result.edgeTo = best->edgeTo;
        if (!best->context.empty ())
            result.hint += " \xC2\xB7 " + best->context;
    }
    else if (on && has (kSnapGrid) && query.grid > 0.0) {
        const double gu = std::round (cursor.x / query.grid) * query.grid;
        const double gv = std::round (cursor.y / query.grid) * query.grid;
        const Vec3 grid = frame.origin + frame.u * gu + frame.v * gv;
        if (Screened (query, grid)) {
            result.snapped = true;
            result.kind = SnapKind::Grid;
            result.point = grid;
            result.hint = SnapKindName (SnapKind::Grid);
        }
    }
    if (query.keepPlane)
        result.point = ProjectToPlane (query.plane, result.point);

    // Terrain gives Z to a plan position; it never moves the position itself.
    const bool level = std::abs (frame.u.z) < 1.0e-9 && std::abs (frame.v.z) < 1.0e-9;
    double z = 0.0;
    if (has (kSnapTerrain) && level && query.keepPlane && context.height && Screened (query, result.point) &&
        context.height (result.point.x, result.point.y, z) && std::isfinite (z) &&
        Screened (query, { result.point.x, result.point.y, z })) {
        result.point.z = z;
        if (!result.snapped) {
            result.snapped = true;
            result.kind = SnapKind::Terrain;
            result.hint = SnapKindName (SnapKind::Terrain);
        }
    }
    return result;
}

bool SnapEdges (const Document& doc, const SnapContext& context, const SnapQuery& query,
                const std::vector<std::pair<Vec3, Vec3>>& edges, Vec3& translate, SnapResult& result)
{
    const double tol = query.tolerance;
    if (!(tol > 0.0) || edges.empty ())
        return false;
    Frame frame;
    frame.origin = query.plane.origin;
    PlaneAxes (query.plane, frame.u, frame.v);
    std::vector<Pt> points;
    std::vector<Seg> targets;
    Gather (doc, context, query, points, targets);
    constexpr double kParallel = 0.017; // about a degree
    const bool gated = query.screen.has_value ();
    const P2 cursor = frame.To (query.cursor);
    // The point of a plane edge nearest the pointer: the moving edge and the target are both
    // judged by it, so a flush follows the grab the user holds, never the far side of the item.
    const auto near = [&] (const Vec3& ea, const Vec3& eb) {
        const double t = std::clamp (LineParameter (frame.To (ea), frame.To (eb), cursor), 0.0, 1.0);
        return Lerp (ea, eb, t);
    };
    double bestGap = tol;
    bool found = false;
    Vec3 bestMoveA, bestMoveB;
    SnapResult bestResult;
    for (const auto& [ma, mb] : edges) {
        if (gated && !Screened (query, near (ma, mb)))
            continue;
        const P2 a = frame.To (ma), b = frame.To (mb);
        const double mx = b.x - a.x, my = b.y - a.y, ml = std::hypot (mx, my);
        if (ml < 1.0e-9)
            continue;
        const double nx = -my / ml, ny = mx / ml; // the moving edge's normal in the plane
        for (const auto& t : targets) {
            if (gated && !Screened (query, near (t.a, t.b)))
                continue;
            const P2 c = frame.To (t.a), d = frame.To (t.b);
            const double tx = d.x - c.x, ty = d.y - c.y, tl = std::hypot (tx, ty);
            if (tl < 1.0e-9 || std::abs (mx * ty - my * tx) / (ml * tl) > kParallel)
                continue;
            // Signed distance between the two lines along the normal, and some overlap along them.
            const double gap = (c.x - a.x) * nx + (c.y - a.y) * ny;
            const double ux = mx / ml, uy = my / ml;
            const double s0 = (c.x - a.x) * ux + (c.y - a.y) * uy, s1 = (d.x - a.x) * ux + (d.y - a.y) * uy;
            if (std::max (s0, s1) < -tol || std::min (s0, s1) > ml + tol)
                continue; // side by side, not along each other
            // The globally closest pair wins, not the first moving edge that happens to reach one;
            // a strict comparison keeps the result deterministic for a fixed edge order.
            if (std::abs (gap) < bestGap) {
                bestGap = std::abs (gap);
                found = true;
                bestMoveA = ma, bestMoveB = mb;
                bestResult = {};
                bestResult.snapped = true;
                bestResult.kind = SnapKind::Nearest;
                bestResult.source = t.ref;
                bestResult.contextSource = t.context;
                bestResult.guides = { { t.a, t.b, SnapKind::Nearest } };
                bestResult.hint = t.context.empty () ? "Edge to edge" : "Edge to edge \xC2\xB7 " + t.context;
                bestResult.onEdge = true;
                bestResult.edgeFrom = t.a;
                bestResult.edgeTo = t.b;
            }
        }
        if (found && !gated)
            break; // one edge flush is the snap; the old behaviour when no view screening is attached
    }
    if (!found)
        return false;
    // One correction from the pair already chosen: an earlier candidate never moved the item.
    {
        const P2 a = frame.To (bestMoveA), b = frame.To (bestMoveB);
        const double mx = b.x - a.x, my = b.y - a.y, ml = std::hypot (mx, my);
        const double nx = -my / ml, ny = mx / ml;
        const P2 c = frame.To (bestResult.edgeFrom);
        const double gap = (c.x - a.x) * nx + (c.y - a.y) * ny;
        translate = translate + frame.u * (gap * nx) + frame.v * (gap * ny);
    }
    result = std::move (bestResult);
    return true;
}

} // namespace geomsrv::edit
