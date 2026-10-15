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
        if (r.part == Part::Entity)
            return true;
        if (vertex != kNoId && r.part == Part::Vertex && r.item == vertex)
            return true;
    }
    return false;
}

void Gather (const Document& doc, const SnapContext& context, const SnapQuery& query, std::vector<Pt>& points,
             std::vector<Seg>& segments)
{
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
        for (size_t i = 0; i < e.segments.size (); ++i) {
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
        // A rounded outline: its arcs and the straight runs between them are its edges.
        if (std::any_of (e.vertices.begin (), e.vertices.end (), [] (const Vertex& v) { return v.radius > 0.0; }) &&
            !Excluded (query.exclude, e.id, kNoId)) {
            const auto shape = Shape (e);
            for (size_t i = 0; i + 1 < shape.size () + (e.closed ? 1 : 0); ++i)
                segments.push_back ({ shape[i], shape[(i + 1) % shape.size ()], { e.id, Part::Entity, 0 }, {} });
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
        PathOutline o;
        std::string error;
        if (moving || !Outline (path, query.pathStep, o, error))
            continue;
        for (size_t i = 0; i < o.points.size (); ++i)
            points.push_back ({ o.points[i], SnapKind::Endpoint, { path.id, Part::PathPoint, path.points[i].id }, {} });
        for (size_t i = 0; i < o.breaks.size (); ++i)
            points.push_back ({ o.breaks[i], SnapKind::Endpoint, { path.id, Part::Break, path.profile[i].id }, {} });
        for (size_t i = 0; i + 1 < o.stations.size (); ++i) {
            segments.push_back ({ o.centre[i], o.centre[i + 1], { path.id, Part::Entity, 0 }, {} });
            segments.push_back ({ o.left[i], o.left[i + 1], { path.id, Part::Range, o.range[i] }, {} });
            segments.push_back ({ o.right[i], o.right[i + 1], { path.id, Part::Range, o.range[i] }, {} });
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

} // namespace

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

    std::vector<Pt> points;
    std::vector<Seg> segments;
    if (on)
        Gather (doc, context, query, points, segments);

    std::optional<Candidate> best;
    const auto offer = [&] (Candidate c) {
        if (c.distance <= tol && (!best || Better (c, *best)))
            best = std::move (c);
    };

    // Notable points. A middle is offered with its edge, below, so it can name the edge.
    for (const auto& p : points) {
        if (p.kind == SnapKind::Midpoint)
            continue;
        const uint32_t mode = p.kind == SnapKind::Endpoint   ? kSnapEndpoint
                              : p.kind == SnapKind::Midpoint ? kSnapMidpoint
                                                             : kSnapCentre;
        if (has (mode))
            offer ({ p.kind, p.p, Dist (frame.To (p.p), cursor), p.ref, p.context, {}, SnapKindName (p.kind) });
    }

    // Segments near the cursor: nearest point, perpendicular foot from the anchor.
    std::vector<Line> lines;
    for (const auto& s : segments) {
        const P2 a = frame.To (s.a), b = frame.To (s.b);
        const double t = LineParameter (a, b, cursor);
        if (std::isnan (t))
            continue; // seen edge-on: it is a point in this projection
        const double clamped = std::clamp (t, 0.0, 1.0);
        const double near = Dist (At (a, b, clamped), cursor);
        if (near <= tol) {
            lines.push_back ({ s.a, s.b, true, { s.a, s.b, SnapKind::Nearest }, s.ref, s.context });
            if (has (kSnapNearest))
                offer ({ SnapKind::Nearest,
                         Lerp (s.a, s.b, clamped),
                         near,
                         s.ref,
                         s.context,
                         {},
                         SnapKindName (SnapKind::Nearest),
                         true,
                         s.a,
                         s.b });
            // Its middle, when that is the snap, names the edge it halves too.
            if (has (kSnapMidpoint)) {
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
        // The segment's extension past the end nearer the cursor.
        if (has (kSnapExtension)) {
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
    }

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
                double t = 0.0, s = 0.0;
                if (!Cross2 (frame.To (l.a), frame.To (l.b), frame.To (m.a), frame.To (m.b), t, s))
                    continue;
                if ((l.bounded && (t < 0.0 || t > 1.0)) || (m.bounded && (s < 0.0 || s > 1.0)))
                    continue;
                const Vec3 p = Lerp (l.a, l.b, t);
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
        result.snapped = true;
        result.kind = SnapKind::Grid;
        result.point = frame.origin + frame.u * gu + frame.v * gv;
        result.hint = SnapKindName (SnapKind::Grid);
    }
    if (query.keepPlane)
        result.point = ProjectToPlane (query.plane, result.point);

    // Terrain gives Z to a plan position; it never moves the position itself.
    const bool level = std::abs (frame.u.z) < 1.0e-9 && std::abs (frame.v.z) < 1.0e-9;
    double z = 0.0;
    if (has (kSnapTerrain) && level && query.keepPlane && context.height &&
        context.height (result.point.x, result.point.y, z) && std::isfinite (z)) {
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
    double best = tol;
    bool found = false;
    for (const auto& [ma, mb] : edges) {
        const P2 a = frame.To (ma), b = frame.To (mb);
        const double mx = b.x - a.x, my = b.y - a.y, ml = std::hypot (mx, my);
        if (ml < 1.0e-9)
            continue;
        const double nx = -my / ml, ny = mx / ml; // the moving edge's normal in the plane
        for (const auto& t : targets) {
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
            if (std::abs (gap) < best) {
                best = std::abs (gap);
                found = true;
                translate = translate + frame.u * (gap * nx) + frame.v * (gap * ny);
                result = {};
                result.snapped = true;
                result.kind = SnapKind::Nearest;
                result.source = t.ref;
                result.contextSource = t.context;
                result.guides = { { t.a, t.b, SnapKind::Nearest } };
                result.hint = t.context.empty () ? "Edge to edge" : "Edge to edge \xC2\xB7 " + t.context;
                result.onEdge = true;
                result.edgeFrom = t.a;
                result.edgeTo = t.b;
            }
        }
        if (found)
            break; // one edge flush is the snap; the correction moved every edge alike
    }
    return found;
}

} // namespace geomsrv::edit
