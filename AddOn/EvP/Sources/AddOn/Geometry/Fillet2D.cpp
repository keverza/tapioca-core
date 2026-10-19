#include "Geometry/Fillet2D.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace geomsrv::fillet2d {
namespace {
constexpr double kPi = 3.14159265358979323846;
Point operator+ (Point a, Point b)
{
    return { a.x + b.x, a.y + b.y };
}
Point operator- (Point a, Point b)
{
    return { a.x - b.x, a.y - b.y };
}
Point operator* (Point a, double t)
{
    return { a.x * t, a.y * t };
}
double Dot (Point a, Point b)
{
    return a.x * b.x + a.y * b.y;
}
double Cross (Point a, Point b)
{
    return a.x * b.y - a.y * b.x;
}
double Length (Point a)
{
    return std::hypot (a.x, a.y);
}
Point Left (Point a)
{
    return { -a.y, a.x };
}
bool Fail (std::string& error, const char* why)
{
    error = why;
    return false;
}
double PositiveAngle (double a)
{
    a = std::fmod (a, 2 * kPi);
    return a < 0 ? a + 2 * kPi : a;
}
bool Valid (const Settings& s)
{
    return std::isfinite (s.lengthTolerance) && s.lengthTolerance > 0 && std::isfinite (s.angleTolerance) &&
           s.angleTolerance > 0 && std::isfinite (s.edgeClearance) && s.edgeClearance > s.lengthTolerance &&
           std::isfinite (s.chordError) && s.chordError > s.lengthTolerance && s.maxArcSegments > 0 &&
           s.maxIntersectionPairs > 0 && s.maxFitPasses > 0;
}
bool OnArc (const Edge& e, Point p, const Settings& s, double* parameter = nullptr)
{
    const double angle = std::atan2 (p.y - e.centre.y, p.x - e.centre.x);
    double d = PositiveAngle (e.sweep > 0 ? angle - e.startAngle : e.startAngle - angle);
    const double tolerance = s.angleTolerance + s.lengthTolerance / e.radius;
    if (2 * kPi - d <= tolerance)
        d = 0;
    if (d > std::abs (e.sweep) + tolerance)
        return false;
    if (parameter)
        *parameter = std::clamp (d / std::abs (e.sweep), 0.0, 1.0);
    return true;
}
struct Hits {
    bool overlap = false;
    std::vector<Point> points;
};
Hits LineLine (const Edge& a, const Edge& b, const Settings& s)
{
    Hits out;
    const Point u = a.to - a.from, v = b.to - b.from, d = b.from - a.from;
    const double lu = Length (u), lv = Length (v), det = Cross (u, v);
    if (std::abs (det) <= s.angleTolerance * lu * lv) {
        if (std::abs (Cross (d, u)) / lu > s.lengthTolerance)
            return out;
        double t0 = Dot (d, u) / Dot (u, u), t1 = Dot (b.to - a.from, u) / Dot (u, u);
        if (t0 > t1)
            std::swap (t0, t1);
        const double lo = std::max (0.0, t0), hi = std::min (1.0, t1);
        if ((hi - lo) * lu > s.lengthTolerance)
            out.overlap = true;
        else if ((lo - hi) * lu <= s.lengthTolerance)
            out.points.push_back (a.from + u * ((lo + hi) * .5));
        return out;
    }
    const double t = Cross (d, v) / det, q = Cross (d, u) / det;
    if (t >= -s.lengthTolerance / lu && t <= 1 + s.lengthTolerance / lu && q >= -s.lengthTolerance / lv &&
        q <= 1 + s.lengthTolerance / lv)
        out.points.push_back (a.from + u * t);
    return out;
}
Hits LineArc (const Edge& line, const Edge& arc, const Settings& s)
{
    Hits out;
    const Point u = line.to - line.from, d = line.from - arc.centre;
    const double length2 = Dot (u, u), t = -Dot (d, u) / length2;
    const Point nearest = d + u * t;
    const double h2 = arc.radius * arc.radius - Dot (nearest, nearest);
    if (h2 < -s.lengthTolerance * (2 * arc.radius + s.lengthTolerance))
        return out;
    const double height2 = std::abs (h2) <= s.lengthTolerance * (2 * arc.radius + s.lengthTolerance) ? 0 : h2;
    const double delta = std::sqrt (std::max (0.0, height2) / length2),
                 tolerance = s.lengthTolerance / std::sqrt (length2);
    for (double q : { t - delta, t + delta }) {
        const Point p = line.from + u * q;
        if (q >= -tolerance && q <= 1 + tolerance && OnArc (arc, p, s))
            if (out.points.empty () || Length (p - out.points.back ()) > s.lengthTolerance)
                out.points.push_back (p);
    }
    return out;
}
Hits ArcArc (const Edge& a, const Edge& b, const Settings& s)
{
    Hits out;
    const Point d = b.centre - a.centre;
    const double length = Length (d);
    if (length <= s.lengthTolerance && std::abs (a.radius - b.radius) <= s.lengthTolerance) {
        for (const auto* edge : { &a, &b }) {
            const Edge& other = edge == &a ? b : a;
            double t = 0;
            if (OnArc (other, At (*edge, .5), s, &t) && t > s.angleTolerance && t < 1 - s.angleTolerance)
                out.overlap = true;
            for (Point p : { edge->from, edge->to })
                if (OnArc (other, p, s))
                    out.points.push_back (p);
        }
        return out;
    }
    if (length <= s.lengthTolerance || length > a.radius + b.radius + s.lengthTolerance ||
        length < std::abs (a.radius - b.radius) - s.lengthTolerance)
        return out;
    const double x = (a.radius * a.radius - b.radius * b.radius + length * length) / (2 * length);
    const double y = std::sqrt (std::max (0.0, a.radius * a.radius - x * x));
    const Point u = d * (1 / length), base = a.centre + u * x;
    for (double sign : { -1., 1. }) {
        const Point p = base + Left (u) * (sign * y);
        if (OnArc (a, p, s) && OnArc (b, p, s))
            out.points.push_back (p);
    }
    return out;
}
Hits Intersect (const Edge& a, const Edge& b, const Settings& s)
{
    if (a.kind == Kind::Line)
        return b.kind == Kind::Line ? LineLine (a, b, s) : LineArc (a, b, s);
    return b.kind == Kind::Line ? LineArc (b, a, s) : ArcArc (a, b, s);
}
bool Contains (const Ring& ring, Point p, const Settings& s)
{
    bool inside = false;
    for (const auto& e : ring.edges) {
        if (e.kind == Kind::Line) {
            if ((e.from.y > p.y) != (e.to.y > p.y) &&
                e.from.x + (p.y - e.from.y) * (e.to.x - e.from.x) / (e.to.y - e.from.y) > p.x)
                inside = !inside;
        }
        else {
            const double y = (p.y - e.centre.y) / e.radius;
            if (std::abs (y) >= 1)
                continue; // a horizontal tangency never crosses the ray
            const double x = e.radius * std::sqrt (1 - y * y);
            for (double sign : { -1., 1. }) {
                const Point q { e.centre.x + sign * x, p.y };
                double t = 0;
                if (q.x <= p.x || !OnArc (e, q, s, &t))
                    continue;
                const double dy = Tangent (e, t).y;
                if ((t > s.angleTolerance && t < 1 - s.angleTolerance) || (t <= s.angleTolerance && dy > 0) ||
                    (t >= 1 - s.angleTolerance && dy < 0))
                    inside = !inside;
            }
        }
    }
    return inside;
}
struct Box {
    double x0, y0, x1, y1;
};
Box Bounds (const Edge& e, const Settings& s)
{
    Box box { std::min (e.from.x, e.to.x), std::min (e.from.y, e.to.y), std::max (e.from.x, e.to.x),
              std::max (e.from.y, e.to.y) };
    if (e.kind == Kind::Arc)
        for (double angle : { 0., kPi * .5, kPi, kPi * 1.5 }) {
            const Point p = e.centre + Point { std::cos (angle), std::sin (angle) } * e.radius;
            if (OnArc (e, p, s)) {
                box.x0 = std::min (box.x0, p.x);
                box.x1 = std::max (box.x1, p.x);
                box.y0 = std::min (box.y0, p.y);
                box.y1 = std::max (box.y1, p.y);
            }
        }
    return box;
}
struct Collision {
    size_t ringA = 0, edgeA = 0, ringB = 0, edgeB = 0;
};
// Bounds sweep avoids comparing every pair in long sampled path boundaries.
bool FindCollision (const Region& region, const Settings& s, Collision& hit, bool& budget)
{
    struct Item {
        Box box;
        size_t ring, edge;
    };
    std::vector<Item> items;
    for (size_t r = 0; r < region.rings.size (); ++r)
        for (size_t e = 0; e < region.rings[r].edges.size (); ++e)
            items.push_back ({ Bounds (region.rings[r].edges[e], s), r, e });
    std::stable_sort (items.begin (), items.end (), [] (const Item& a, const Item& b) { return a.box.x0 < b.box.x0; });
    size_t pairs = 0;
    for (size_t i = 0; i < items.size (); ++i)
        for (size_t j = i + 1; j < items.size () && items[j].box.x0 <= items[i].box.x1 + s.lengthTolerance; ++j) {
            const Item &a = items[i], &b = items[j];
            if (++pairs > s.maxIntersectionPairs) {
                budget = true;
                return false;
            }
            if (b.box.y0 > a.box.y1 + s.lengthTolerance || a.box.y0 > b.box.y1 + s.lengthTolerance)
                continue;
            const Edge &ea = region.rings[a.ring].edges[a.edge], &eb = region.rings[b.ring].edges[b.edge];
            const Hits hits = Intersect (ea, eb, s);
            bool collision = hits.overlap;
            for (const Point p : hits.points) {
                const size_t count = region.rings[a.ring].edges.size ();
                const bool adjacent =
                    a.ring == b.ring && ((a.edge + 1) % count == b.edge || (b.edge + 1) % count == a.edge);
                const bool shared =
                    adjacent &&
                    (Length (p - ea.from) <= s.lengthTolerance || Length (p - ea.to) <= s.lengthTolerance) &&
                    (Length (p - eb.from) <= s.lengthTolerance || Length (p - eb.to) <= s.lengthTolerance);
                collision |= !shared;
            }
            if (collision) {
                hit = { a.ring, a.edge, b.ring, b.edge };
                return true;
            }
        }
    return false;
}
struct Candidate {
    Point point, in, out;
    size_t source = 0;
    double turn = 0, factor = 0, requested = 0, limit = 0, radius = 0;
    bool collisionLimited = false;
};
using Candidates = std::vector<std::vector<Candidate>>;
Region Rebuild (const Candidates& rings)
{
    Region result;
    for (const auto& candidates : rings) {
        Ring ring;
        std::vector<Point> from, to;
        for (const auto& c : candidates) {
            const double t = c.radius * c.factor;
            from.push_back (c.point - c.in * t);
            to.push_back (c.point + c.out * t);
        }
        for (size_t i = 0; i < candidates.size (); ++i) {
            const auto& c = candidates[i];
            const size_t next = (i + 1) % candidates.size ();
            if (c.radius > 0) {
                Edge arc;
                arc.kind = Kind::Arc;
                arc.from = from[i];
                arc.to = to[i];
                arc.centre = from[i] + Left (c.in) * (c.turn > 0 ? c.radius : -c.radius);
                arc.radius = c.radius;
                arc.sweep = c.turn;
                arc.startAngle = std::atan2 (arc.from.y - arc.centre.y, arc.from.x - arc.centre.x);
                arc.source = arc.nextSource = c.source;
                ring.edges.push_back (arc);
            }
            Edge line;
            line.from = to[i];
            line.to = from[next];
            line.source = c.source;
            line.nextSource = candidates[next].source;
            ring.edges.push_back (line);
        }
        result.rings.push_back (std::move (ring));
    }
    return result;
}
double Area (const InputRing& ring)
{
    double area = 0;
    const Point origin = ring.points[0];
    for (size_t i = 0; i < ring.points.size (); ++i)
        area += Cross (ring.points[i] - origin, ring.points[(i + 1) % ring.points.size ()] - origin);
    return area * .5;
}
} // namespace

Point At (const Edge& e, double t)
{
    if (t <= 0)
        return e.from;
    if (t >= 1)
        return e.to;
    if (e.kind == Kind::Line)
        return e.from + (e.to - e.from) * t;
    const double angle = e.startAngle + e.sweep * t;
    return e.centre + Point { std::cos (angle), std::sin (angle) } * e.radius;
}
Point Tangent (const Edge& e, double t)
{
    if (e.kind == Kind::Line) {
        const Point d = e.to - e.from;
        return d * (1 / Length (d));
    }
    const double angle = e.startAngle + e.sweep * t, sign = e.sweep > 0 ? 1 : -1;
    return { -std::sin (angle) * sign, std::cos (angle) * sign };
}

bool Build (const std::vector<InputRing>& input, const Settings& s, Region& out, std::string& error)
{
    if (!Valid (s))
        return Fail (error, "invalid fillet tolerance or safety budget");
    Candidates candidates;
    for (const auto& ring : input) {
        const size_t n = ring.points.size ();
        if (n < 3 || (!ring.corners.empty () && ring.corners.size () != n))
            return Fail (error, "invalid fillet ring/corner count");
        std::vector<Candidate> made;
        for (size_t i = 0; i < n; ++i) {
            Candidate c;
            c.point = ring.points[i];
            c.source = i;
            if (!std::isfinite (c.point.x) || !std::isfinite (c.point.y))
                return Fail (error, "non-finite fillet vertex");
            c.in = c.point - ring.points[(i + n - 1) % n];
            c.out = ring.points[(i + 1) % n] - c.point;
            const double before = Length (c.in), after = Length (c.out);
            if (!std::isfinite (before) || !std::isfinite (after) || before <= s.lengthTolerance ||
                after <= s.lengthTolerance)
                return Fail (error, "fillet ring has a degenerate edge");
            c.in = c.in * (1 / before);
            c.out = c.out * (1 / after);
            c.turn = std::atan2 (Cross (c.in, c.out), Dot (c.in, c.out));
            const auto request = ring.corners.empty () ? Request {} : ring.corners[i];
            if (!(request.radius >= 0) || !std::isfinite (request.radius) || !(request.limit >= 0))
                return Fail (error, "invalid requested fillet radius or limit");
            c.requested = request.radius;
            c.limit = request.limit;
            if (std::abs (c.turn) >= kPi - s.angleTolerance)
                return Fail (error, "fillet ring doubles back");
            if (std::abs (c.turn) > s.angleTolerance) {
                c.factor = std::tan (std::abs (c.turn) * .5);
                c.radius = std::min (c.requested, c.limit);
            }
            made.push_back (c);
        }
        if (!std::isfinite (Area (ring)) || std::abs (Area (ring)) <= s.lengthTolerance * s.lengthTolerance)
            return Fail (error, "fillet ring has no area");
        candidates.push_back (std::move (made));
    }
    // Validate assembled sharp topology before fitting. Never use a Boolean
    // cleanup to silently turn a self-intersection into a different region.
    Candidates sharp = candidates;
    for (auto& ring : sharp)
        for (auto& c : ring)
            c.radius = 0;
    const Region original = Rebuild (sharp);
    Collision collision;
    bool budget = false;
    if (FindCollision (original, s, collision, budget) || budget)
        return Fail (error, budget ? "fillet intersection budget exceeded" : "invalid sharp region topology");
    std::vector<std::vector<bool>> nesting (input.size (), std::vector<bool> (input.size (), false));
    std::vector<bool> holes;
    for (size_t i = 0; i < input.size (); ++i) {
        size_t depth = 0;
        for (size_t j = 0; j < input.size (); ++j)
            if (i != j) {
                nesting[i][j] = Contains (original.rings[j], input[i].points[0], s);
                depth += nesting[i][j];
            }
        const bool hole = depth % 2 != 0;
        holes.push_back (hole);
        if ((Area (input[i]) > 0) == hole) {
            auto& ring = candidates[i];
            std::reverse (ring.begin (), ring.end ());
            for (auto& c : ring) {
                std::swap (c.in, c.out);
                c.in = c.in * -1;
                c.out = c.out * -1;
                c.turn = -c.turn;
            }
        }
    }
    // Simultaneous monotone fitting: both endpoints share each edge's budget.
    // A small fillet no longer reserves an arbitrary half-edge against a big one.
    for (auto& ring : candidates) {
        std::vector<double> scale (ring.size (), 1);
        for (size_t i = 0; i < ring.size (); ++i) {
            const size_t j = (i + 1) % ring.size ();
            const double available = std::max (0.0, Length (ring[j].point - ring[i].point) - s.edgeClearance);
            const double used = ring[i].radius * ring[i].factor + ring[j].radius * ring[j].factor;
            if (used > available)
                scale[i] = std::min (scale[i], available / used), scale[j] = std::min (scale[j], available / used);
        }
        for (size_t i = 0; i < ring.size (); ++i)
            ring[i].radius *= scale[i];
    }
    const auto shrink = [&] (size_t ring, size_t source) {
        for (auto& c : candidates[ring])
            if (c.source == source && c.radius > 0) {
                c.radius *= .5;
                c.collisionLimited = true;
                if (c.radius * c.factor <= s.lengthTolerance || c.radius <= s.lengthTolerance)
                    c.radius = 0;
                return true;
            }
        return false;
    };
    for (size_t pass = 0; pass < s.maxFitPasses; ++pass) {
        Region result = Rebuild (candidates);
        if (FindCollision (result, s, collision, budget)) {
            bool reduced = false;
            const auto reduce = [&] (size_t r, size_t e) {
                const auto& edge = result.rings[r].edges[e];
                if (edge.kind == Kind::Arc)
                    reduced |= shrink (r, edge.source);
            };
            reduce (collision.ringA, collision.edgeA);
            reduce (collision.ringB, collision.edgeB);
            if (!reduced)
                return Fail (error, "fillet collision cannot be resolved without changing the source boundary");
            continue;
        }
        if (budget)
            return Fail (error, "fillet intersection budget exceeded");
        bool topology = true, reduced = false;
        for (size_t i = 0; i < result.rings.size (); ++i)
            for (size_t j = 0; j < result.rings.size (); ++j)
                if (i != j && Contains (result.rings[j], At (result.rings[i].edges[0], .5), s) != nesting[i][j]) {
                    topology = false;
                    for (size_t r : { i, j })
                        for (size_t k = 0; k < candidates[r].size (); ++k)
                            reduced |= shrink (r, candidates[r][k].source);
                }
        if (!topology) {
            if (!reduced)
                return Fail (error, "fillet would change region nesting");
            continue;
        }
        for (size_t r = 0; r < candidates.size (); ++r) {
            result.rings[r].hole = holes[r];
            size_t parentDepth = 0;
            for (size_t other = 0; other < nesting.size (); ++other)
                if (nesting[r][other]) {
                    const size_t depth = size_t (std::count (nesting[other].begin (), nesting[other].end (), true)) + 1;
                    if (depth > parentDepth) {
                        result.rings[r].parent = other;
                        parentDepth = depth;
                    }
                }
            const auto& ring = candidates[r];
            for (size_t i = 0; i < ring.size (); ++i) {
                const size_t prev = (i + ring.size () - 1) % ring.size (), next = (i + 1) % ring.size ();
                const auto& c = ring[i];
                double maximum = 0;
                if (c.factor > 0) {
                    const double available = std::min (
                        Length (c.point - ring[prev].point) - s.edgeClearance - ring[prev].radius * ring[prev].factor,
                        Length (c.point - ring[next].point) - s.edgeClearance - ring[next].radius * ring[next].factor);
                    maximum = std::min (c.limit, std::max (0.0, available) / c.factor);
                    if (c.collisionLimited)
                        maximum = std::min (maximum, c.radius);
                    // Roundoff in a tight shared-edge constraint is not a new limitation.
                    maximum = std::max (maximum, c.radius);
                }
                result.rings[r].corners.push_back ({ c.source, c.requested, c.radius, maximum, c.radius * c.factor });
            }
        }
        out = std::move (result);
        error.clear ();
        return true;
    }
    return Fail (error, "fillet collision fitting did not converge within its safety budget");
}

bool Tessellate (const Region& region, const Settings& s, std::vector<std::vector<Point>>& out, std::string& error)
{
    if (!Valid (s))
        return Fail (error, "invalid fillet tessellation settings");
    std::vector<std::vector<Point>> made;
    for (const auto& ring : region.rings) {
        std::vector<Point> points;
        for (const auto& edge : ring.edges) {
            size_t pieces = 1;
            if (edge.kind == Kind::Arc) {
                const double step = 4 * std::asin (std::sqrt (std::min (1.0, s.chordError / (2 * edge.radius))));
                const double count = std::ceil (std::abs (edge.sweep) / step);
                if (!std::isfinite (count) || count > double (s.maxArcSegments))
                    return Fail (error, "fillet chord-error segment budget exceeded");
                pieces = std::max (size_t (1), size_t (count));
            }
            for (size_t i = 0; i < pieces; ++i)
                points.push_back (At (edge, double (i) / pieces));
        }
        made.push_back (std::move (points));
    }
    out = std::move (made);
    error.clear ();
    return true;
}
} // namespace geomsrv::fillet2d
