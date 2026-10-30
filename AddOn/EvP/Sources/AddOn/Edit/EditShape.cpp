#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace geomsrv::edit {

namespace {
constexpr double kStraight = 1.0e-6; // legs this close to opposite run straight on: no corner
} // namespace

namespace {

bool Set (Vec3 v)
{
    return std::hypot (v.x, v.y) > 1.0e-12;
}

Vec3 Plan (Vec3 v)
{
    return { v.x, v.y, 0.0 };
}

Vec3 Cubic (Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double t)
{
    const double u = 1.0 - t;
    return p0 * (u * u * u) + p1 * (3.0 * u * u * t) + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
}

Vertex* VertexById (Entity& e, Id id)
{
    const int i = VertexIndex (e, id);
    return i < 0 ? nullptr : &e.vertices[size_t (i)];
}

bool Editable (Document& doc, Id entity, Id vertex, Entity*& e, Vertex*& v, std::string& error)
{
    e = Find (doc, entity);
    v = e != nullptr ? VertexById (*e, vertex) : nullptr;
    if (v == nullptr) {
        error = "that vertex no longer exists";
        return false;
    }
    if (e->ellipse) {
        error = "a parametric ellipse is edited by its parameters; convert it to a curve for handle edits";
        return false;
    }
    if (v->lockXY) {
        error = "that vertex is locked";
        return false;
    }
    return true;
}

} // namespace

bool SegmentCurved (const Entity& e, size_t i)
{
    size_t from = 0, to = 0;
    if (i >= SegmentCount (e))
        return false;
    SegmentEnds (e, i, from, to);
    return Set (e.vertices[from].out) || Set (e.vertices[to].in);
}

Vec3 SegmentPoint (const Entity& e, size_t i, double t)
{
    size_t from = 0, to = 0;
    SegmentEnds (e, i, from, to);
    const Vertex &a = e.vertices[from], &b = e.vertices[to];
    if (!SegmentCurved (e, i))
        return Lerp (a.p, b.p, t);
    const Vec3 p = Cubic (Plan (a.p), Plan (a.p + a.out), Plan (b.p + b.in), Plan (b.p), t);
    return { p.x, p.y, a.p.z + (b.p.z - a.p.z) * t };
}

std::vector<Vec3> SegmentPoints (const Entity& e, size_t i, int pieces)
{
    std::vector<Vec3> out;
    const int n = SegmentCurved (e, i) ? std::max (pieces, 1) : 1;
    for (int k = 0; k <= n; ++k)
        out.push_back (SegmentPoint (e, i, double (k) / n));
    return out;
}

double NearestOnSegment (const Entity& e, size_t i, Vec3 p)
{
    // Coarse samples, then a few halvings about the best.
    double best = 0.0, bestD = std::numeric_limits<double>::infinity ();
    constexpr int kSamples = 48;
    for (int k = 0; k <= kSamples; ++k) {
        const double t = double (k) / kSamples, d = DistanceXY (SegmentPoint (e, i, t), p);
        if (d < bestD)
            bestD = d, best = t;
    }
    double step = 1.0 / kSamples;
    for (int it = 0; it < 20; ++it) {
        step *= 0.5;
        for (const double t : { best - step, best + step })
            if (t >= 0.0 && t <= 1.0) {
                const double d = DistanceXY (SegmentPoint (e, i, t), p);
                if (d < bestD)
                    bestD = d, best = t;
            }
    }
    return best;
}

bool SetHandle (Document& doc, Id entity, Id vertex, bool out, Vec3 offset, bool independent, std::string& error)
{
    Entity* e = nullptr;
    Vertex* v = nullptr;
    if (!Editable (doc, entity, vertex, e, v, error))
        return false;
    if (!Finite (offset)) {
        error = "the handle is not finite";
        return false;
    }
    if (independent)
        v->smooth = false;
    Vec3& moved = out ? v->out : v->in;
    Vec3& other = out ? v->in : v->out;
    moved = Plan (offset);
    const double length = std::hypot (moved.x, moved.y);
    if (v->smooth && length > 1.0e-12) {
        const double keep = Set (other) ? std::hypot (other.x, other.y) : length;
        other = moved * (-keep / length);
    }
    return true;
}

bool SetSmooth (Document& doc, Id entity, Id vertex, bool smooth, std::string& error)
{
    Entity* e = nullptr;
    Vertex* v = nullptr;
    if (!Editable (doc, entity, vertex, e, v, error))
        return false;
    if (!smooth) {
        v->in = v->out = {};
        v->smooth = false;
        return true;
    }
    const size_t n = e->vertices.size (), i = size_t (VertexIndex (*e, vertex));
    const bool hasPrev = e->closed || i > 0, hasNext = e->closed || i + 1 < n;
    const Vec3 prev = hasPrev ? e->vertices[(i + n - 1) % n].p : v->p;
    const Vec3 next = hasNext ? e->vertices[(i + 1) % n].p : v->p;
    Vec3 dir;
    if (!Normalize (Plan (next - prev), dir)) {
        error = "a smooth point needs neighbours on either side";
        return false;
    }
    v->in = dir * (-DistanceXY (v->p, prev) / 3.0);
    v->out = dir * (DistanceXY (v->p, next) / 3.0);
    v->smooth = true;
    v->radius = 0.0; // a smooth point is no corner
    return true;
}

bool SetSegmentCurved (Document& doc, Id entity, Id segment, bool curved, std::string& error)
{
    Entity* e = Find (doc, entity);
    if (e != nullptr && e->ellipse) {
        error = "a parametric ellipse is edited by its parameters; convert it to a curve for handle edits";
        return false;
    }
    const int i = e != nullptr ? SegmentIndex (*e, segment) : -1;
    if (i < 0) {
        error = "that segment no longer exists";
        return false;
    }
    size_t from = 0, to = 0;
    SegmentEnds (*e, size_t (i), from, to);
    Vertex &a = e->vertices[from], &b = e->vertices[to];
    if ((a.lockXY || b.lockXY) && curved != SegmentCurved (*e, size_t (i))) {
        error = "an end of that segment is locked";
        return false;
    }
    if (!curved) {
        a.out = b.in = {};
        return true;
    }
    if (SegmentCurved (*e, size_t (i)))
        return true;
    const Vec3 chord = Plan (b.p - a.p);
    a.out = chord * (1.0 / 3.0);
    b.in = chord * (-1.0 / 3.0);
    a.radius = b.radius = 0.0; // a curved leg ends any rounding
    return true;
}

bool BendSegment (Document& doc, Id entity, Id segment, double t, Vec3 move, std::string& error)
{
    Entity* e = Find (doc, entity);
    if (e != nullptr && e->ellipse) {
        error = "a parametric ellipse is edited by its parameters; convert it to a curve for handle edits";
        return false;
    }
    const int i = e != nullptr ? SegmentIndex (*e, segment) : -1;
    if (i < 0) {
        error = "that segment no longer exists";
        return false;
    }
    t = std::clamp (t, 0.05, 0.95);
    size_t from = 0, to = 0;
    SegmentEnds (*e, size_t (i), from, to);
    Vertex &a = e->vertices[from], &b = e->vertices[to];
    if (a.lockXY || b.lockXY) {
        error = "an end of that segment is locked";
        return false;
    }
    if (!SegmentCurved (*e, size_t (i))) {
        const Vec3 chord = Plan (b.p - a.p);
        a.out = chord * (1.0 / 3.0);
        b.in = chord * (-1.0 / 3.0);
    }
    const Vec3 shift = Plan (move) * (1.0 / (3.0 * t * (1.0 - t)));
    a.out = a.out + shift;
    b.in = b.in + shift;
    a.radius = b.radius = 0.0;
    // A smooth end keeps its other handle in line.
    for (auto [v, out] : { std::pair<Vertex*, bool> { &a, true }, std::pair<Vertex*, bool> { &b, false } })
        if (v->smooth) {
            Vec3& moved = out ? v->out : v->in;
            Vec3& other = out ? v->in : v->out;
            const double length = std::hypot (moved.x, moved.y), keep = std::hypot (other.x, other.y);
            if (length > 1.0e-12 && keep > 1.0e-12)
                other = moved * (-keep / length);
        }
    return true;
}

bool CornerOf (const Entity& e, size_t i, CornerGeometry& c)
{
    const size_t n = e.vertices.size ();
    if (n < 3 || i >= n || (!e.closed && (i == 0 || i + 1 >= n)))
        return false;
    // Only between straight legs: a curve ends a rounding.
    if (Set (e.vertices[i].in) || Set (e.vertices[i].out) || Set (e.vertices[(i + n - 1) % n].out) ||
        Set (e.vertices[(i + 1) % n].in))
        return false;
    c.at = i;
    c.prev = (i + n - 1) % n;
    c.next = (i + 1) % n;
    c.corner = e.vertices[i].p;
    const Vec3 a = e.vertices[c.prev].p - c.corner, b = e.vertices[c.next].p - c.corner;
    c.legPrev = std::hypot (a.x, a.y);
    c.legNext = std::hypot (b.x, b.y);
    if (c.legPrev < kMinSegment || c.legNext < kMinSegment)
        return false;
    c.toPrev = { a.x / c.legPrev, a.y / c.legPrev, 0.0 };
    c.toNext = { b.x / c.legNext, b.y / c.legNext, 0.0 };
    const double angle = std::acos (std::clamp (Dot (c.toPrev, c.toNext), -1.0, 1.0));
    if (angle > 3.14159265358979323846 - kStraight || angle < kStraight)
        return false;
    c.half = angle * 0.5;
    return Normalize (c.toPrev + c.toNext, c.bisector);
}

double TangentLength (const CornerGeometry& c, double radius)
{
    return radius / std::tan (c.half);
}

double MaxRadius (const Entity& e, const std::vector<size_t>& set)
{
    if (set.empty ())
        return 0.0;
    const auto inSet = [&] (size_t i) { return std::find (set.begin (), set.end (), i) != set.end (); };
    double best = std::numeric_limits<double>::infinity ();
    for (size_t i : set) {
        CornerGeometry c;
        if (!CornerOf (e, i, c))
            return 0.0;
        const double cot = 1.0 / std::tan (c.half);
        // Each leg: this rounding and the neighbour's -- the same radius when it is in the
        // set too, its own otherwise.
        for (const auto [other, leg] : { std::pair { c.prev, c.legPrev }, std::pair { c.next, c.legNext } }) {
            CornerGeometry o;
            const bool corner = CornerOf (e, other, o);
            if (corner && inSet (other))
                best = std::min (best, leg / (cot + 1.0 / std::tan (o.half)));
            else {
                const double taken = corner ? TangentLength (o, e.vertices[other].radius) : 0.0;
                best = std::min (best, std::max (0.0, leg - taken) / cot);
            }
        }
    }
    return std::isfinite (best) ? best : 0.0;
}

double DrawnRadius (const Entity& e, size_t i)
{
    CornerGeometry c;
    if (i >= e.vertices.size () || !(e.vertices[i].radius > 0.0) || !CornerOf (e, i, c))
        return 0.0;
    double t = TangentLength (c, e.vertices[i].radius);
    for (const auto [other, leg] : { std::pair { c.prev, c.legPrev }, std::pair { c.next, c.legNext } }) {
        CornerGeometry o;
        const double theirs = e.vertices[other].radius > 0.0 && CornerOf (e, other, o)
                                  ? TangentLength (o, e.vertices[other].radius)
                                  : 0.0;
        const double mine = TangentLength (c, e.vertices[i].radius);
        if (mine + theirs > leg)
            t = std::min (t, leg * mine / (mine + theirs)); // both give way in proportion
    }
    return t * std::tan (c.half);
}

std::vector<Vec3> Shape (const Entity& e, double maxAngle)
{
    std::vector<Vec3> out;
    const size_t n = e.vertices.size ();
    if (!(maxAngle > 0.0))
        maxAngle = 0.15;
    const auto curve = [&] (size_t i) {
        // A curved segment leaving vertex i: its points between its ends.
        if (i < SegmentCount (e) && SegmentCurved (e, i)) {
            const auto points = SegmentPoints (e, i);
            out.insert (out.end (), points.begin () + 1, points.end () - 1);
        }
    };
    for (size_t i = 0; i < n; ++i) {
        const double r = DrawnRadius (e, i);
        CornerGeometry c;
        if (!(r > 0.0) || !CornerOf (e, i, c)) {
            out.push_back (e.vertices[i].p);
            curve (i);
            continue;
        }
        const double t = TangentLength (c, r);
        const Vec3 p = c.corner, t1 = p + c.toPrev * t, t2 = p + c.toNext * t;
        const Vec3 centre = p + c.bisector * (r / std::sin (c.half));
        const double a1 = std::atan2 (t1.y - centre.y, t1.x - centre.x);
        double sweep = std::atan2 (t2.y - centre.y, t2.x - centre.x) - a1;
        sweep = std::remainder (sweep, 2.0 * 3.14159265358979323846);
        const int steps = std::max (2, int (std::ceil (std::abs (sweep) / maxAngle)));
        for (int k = 0; k <= steps; ++k) {
            const double a = a1 + sweep * k / steps;
            out.push_back ({ centre.x + r * std::cos (a), centre.y + r * std::sin (a), p.z });
        }
        out[out.size () - steps - 1] = { t1.x, t1.y, p.z };
        out.back () = { t2.x, t2.y, p.z };
    }
    return out;
}

bool Rounded (const Entity& e)
{
    for (const auto& v : e.vertices)
        if (v.radius > 0.0 || Set (v.in) || Set (v.out))
            return true;
    return false;
}

ShapeFeatures Features (const Entity& e, double maxAngle)
{
    ShapeFeatures f;
    const size_t n = e.vertices.size ();
    if (n < 2)
        return f;
    int run = 0;
    // Each vertex: where the outline leaves it (a tangent point after a rounding) and where
    // it arrives (one before).
    std::vector<Vec3> leave (n), arrive (n);
    for (size_t i = 0; i < n; ++i) {
        leave[i] = arrive[i] = e.vertices[i].p;
        const double r = DrawnRadius (e, i);
        CornerGeometry c;
        if (!(r > 0.0) || !CornerOf (e, i, c))
            continue;
        const double t = TangentLength (c, r);
        arrive[i] = { c.corner.x + c.toPrev.x * t, c.corner.y + c.toPrev.y * t, c.corner.z };
        leave[i] = { c.corner.x + c.toNext.x * t, c.corner.y + c.toNext.y * t, c.corner.z };
        f.tangentPoints.push_back (arrive[i]);
        f.tangentPoints.push_back (leave[i]);
        const Vec3 centre = c.corner + c.bisector * (r / std::sin (c.half));
        f.centres.push_back ({ centre.x, centre.y, c.corner.z });
        // The arc's pieces, one run.
        const double a1 = std::atan2 (arrive[i].y - centre.y, arrive[i].x - centre.x);
        const double sweep = std::remainder (std::atan2 (leave[i].y - centre.y, leave[i].x - centre.x) - a1,
                                             2.0 * 3.14159265358979323846);
        const int steps = std::max (2, int (std::ceil (std::abs (sweep) / (maxAngle > 0.0 ? maxAngle : 0.15))));
        Vec3 last = arrive[i];
        for (int k = 1; k <= steps; ++k) {
            const double a = a1 + sweep * k / steps;
            const Vec3 p =
                k == steps ? leave[i] : Vec3 { centre.x + r * std::cos (a), centre.y + r * std::sin (a), c.corner.z };
            f.edges.push_back ({ last, p, run, i });
            last = p;
        }
        ++run;
    }
    for (size_t i = 0; i < SegmentCount (e); ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (e, i, from, to);
        if (!SegmentCurved (e, i)) {
            f.edges.push_back ({ leave[from], arrive[to], -1, i });
            continue;
        }
        const auto points = SegmentPoints (e, i);
        for (size_t k = 0; k + 1 < points.size (); ++k)
            f.edges.push_back ({ points[k], points[k + 1], run, i });
        ++run;
    }
    return f;
}

std::vector<int> CurveRuns (const std::vector<Vec3>& points, bool closed, int firstRun)
{
    const size_t pieces = points.size () < 2 ? 0 : points.size () - (closed ? 0 : 1);
    std::vector<int> runs (pieces, -1);
    const auto direction = [&] (size_t i) {
        const Vec3 a = points[i], b = points[(i + 1) % points.size ()];
        return std::atan2 (b.y - a.y, b.x - a.x);
    };
    // A piece bends from its neighbour by a sliver: a sampled curve, not a corner.
    const auto bends = [&] (size_t i, size_t j) {
        const double turn = std::abs (std::remainder (direction (j) - direction (i), 2.0 * 3.14159265358979323846));
        return turn > 1.0e-6 && turn < 0.35;
    };
    // A piece of a curve bends from the pieces on both its sides; a straight lead-in bends
    // only into the curve.
    std::vector<bool> curved (pieces, false);
    for (size_t i = 0; i < pieces; ++i) {
        const bool hasPrev = closed || i > 0, hasNext = closed || i + 1 < pieces;
        curved[i] =
            pieces > 2 && hasPrev && hasNext && bends ((i + pieces - 1) % pieces, i) && bends (i, (i + 1) % pieces);
    }
    int run = firstRun - 1;
    for (size_t i = 0; i < pieces; ++i)
        if (curved[i]) {
            if (i == 0 || !curved[i - 1])
                ++run;
            runs[i] = run;
        }
    return runs;
}

Vec3 CornerGripAt (const Entity& e, size_t i, double minInset)
{
    CornerGeometry c;
    if (!CornerOf (e, i, c))
        return i < e.vertices.size () ? e.vertices[i].p : Vec3 {};
    const double r = DrawnRadius (e, i);
    const double inset = r > 0.0 ? r / std::sin (c.half) - r : 0.0;
    return c.corner + c.bisector * std::max (inset, minInset);
}

double RadiusFromGrip (const Entity& e, size_t i, Vec3 p, double minInset)
{
    CornerGeometry c;
    if (!CornerOf (e, i, c))
        return 0.0;
    const double d = Dot (Vec3 { p.x - c.corner.x, p.y - c.corner.y, 0.0 }, c.bisector);
    if (!(d > minInset))
        return 0.0;
    return d / (1.0 / std::sin (c.half) - 1.0);
}

bool SetCornerRadius (Document& doc, Id entity, const std::vector<Id>& vertices, double radius, std::string& error)
{
    Entity* e = Find (doc, entity);
    if (e == nullptr) {
        error = "that object no longer exists";
        return false;
    }
    if (e->ellipse) {
        error = "a parametric ellipse is edited by its parameters; convert it to a curve for corner edits";
        return false;
    }
    if (!(radius >= 0.0) || !std::isfinite (radius)) {
        error = "a corner radius cannot be negative";
        return false;
    }
    std::vector<size_t> set;
    for (Id v : vertices) {
        const int i = VertexIndex (*e, v);
        CornerGeometry c;
        if (i < 0 || !CornerOf (*e, size_t (i), c)) {
            error = "only a corner can be rounded: not an end, and not a straight run";
            return false;
        }
        if (e->vertices[size_t (i)].lockXY) {
            error = "that corner is locked";
            return false;
        }
        set.push_back (size_t (i));
    }
    const double fits = MaxRadius (*e, set);
    if (radius > fits * (1.0 + 1.0e-9) + 1.0e-12) {
        char text[96];
        std::snprintf (text, sizeof text, "the radius does not fit its legs: at most %.3f m", fits);
        error = text;
        return false;
    }
    for (size_t i : set)
        e->vertices[i].radius = radius;
    return true;
}

} // namespace geomsrv::edit
