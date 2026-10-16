#include "Edit/EditPolyline.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace geomsrv::edit {

namespace {

constexpr double kPi = 3.14159265358979323846;

Entity* Get (Document& doc, Id entity, std::string& error)
{
    Entity* e = Find (doc, entity);
    if (e == nullptr)
        error = "the entity no longer exists";
    return e;
}

int VertexAt (const Entity& e, Id vertex, std::string& error)
{
    const int i = VertexIndex (e, vertex);
    if (i < 0)
        error = "the vertex no longer exists";
    return i;
}

int SegmentAt (const Entity& e, Id segment, std::string& error)
{
    const int i = SegmentIndex (e, segment);
    if (i < 0)
        error = "the segment no longer exists";
    return i;
}

// Plan direction and length of a -> b.
bool PlanDirection (Vec3 a, Vec3 b, Vec3& dir, double& length)
{
    length = DistanceXY (a, b);
    return Normalize (Vec3 { b.x - a.x, b.y - a.y, 0.0 }, dir);
}

Vec3 LeftNormal (Vec3 dir)
{
    return { -dir.y, dir.x, 0.0 };
}

// Plan crossing of lines a + t (b - a) and c + s (d - c).
bool CrossPlan (Vec3 a, Vec3 b, Vec3 c, Vec3 d, double& t, double& s)
{
    const double rx = b.x - a.x, ry = b.y - a.y, qx = d.x - c.x, qy = d.y - c.y;
    const double den = rx * qy - ry * qx;
    const double scale = std::hypot (rx, ry) * std::hypot (qx, qy);
    if (!(scale > 0.0) || std::abs (den) < 1.0e-9 * scale)
        return false;
    t = ((c.x - a.x) * qy - (c.y - a.y) * qx) / den;
    s = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
    return true;
}

bool Editable (const Entity& e, std::string& error)
{
    if (e.kind == Kind::Extrusion) {
        error = "an extrusion profile keeps its shape here; edit its faces";
        return false;
    }
    return true;
}

Segment CopyAttributes (const Segment& from, Document& doc)
{
    Segment s = from;
    s.id = NewId (doc);
    return s;
}

// All edges of `target`, in plan.
std::vector<std::pair<Vec3, Vec3>> EdgesOf (const Entity& target)
{
    std::vector<std::pair<Vec3, Vec3>> out;
    for (size_t i = 0; i < SegmentCount (target); ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (target, i, from, to);
        out.push_back ({ target.vertices[from].p, target.vertices[to].p });
    }
    return out;
}

void Reverse (Entity& e)
{
    std::reverse (e.vertices.begin (), e.vertices.end ());
    // Segment i joined i -> i + 1; after reversal it joins (n-2-i) -> (n-1-i).
    std::reverse (e.segments.begin (), e.segments.end ());
}

} // namespace

std::vector<Vec3> Rectangle (Vec3 corner, Vec3 opposite)
{
    return { corner,
             { opposite.x, corner.y, corner.z },
             { opposite.x, opposite.y, corner.z },
             { corner.x, opposite.y, corner.z } };
}

bool MoveVertex (Document& doc, Id entity, Id vertex, Vec3 to, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const int i = e != nullptr ? VertexAt (*e, vertex, error) : -1;
    if (i < 0)
        return false;
    Vertex& v = e->vertices[static_cast<size_t> (i)];
    if ((v.lockXY && (to.x != v.p.x || to.y != v.p.y)) || (v.lockZ && to.z != v.p.z)) {
        error = "that vertex is locked";
        return false;
    }
    if (e->kind == Kind::Extrusion && to.z != v.p.z) {
        error = "an extrusion profile stays flat";
        return false;
    }
    v.p = to;
    return true;
}

bool InsertVertex (Document& doc, Id entity, Id segment, Vec3 at, std::string& error, Id* created)
{
    Entity* e = Get (doc, entity, error);
    const int k = e != nullptr ? SegmentAt (*e, segment, error) : -1;
    if (k < 0)
        return false;
    if (e->kind == Kind::Extrusion)
        at.z = e->vertices.front ().p.z;
    const size_t at1 = static_cast<size_t> (k) + 1;
    Vertex v { NewId (doc), at };
    if (SegmentCurved (*e, static_cast<size_t> (k))) {
        // De Casteljau at the nearest point: both halves keep the curve exactly.
        size_t from = 0, to = 0;
        SegmentEnds (*e, static_cast<size_t> (k), from, to);
        Vertex &a = e->vertices[from], &b = e->vertices[to];
        const double t = NearestOnSegment (*e, static_cast<size_t> (k), at);
        const Vec3 p0 = a.p, p1 = a.p + a.out, p2 = b.p + b.in, p3 = b.p;
        const Vec3 q0 = Lerp (p0, p1, t), q1 = Lerp (p1, p2, t), q2 = Lerp (p2, p3, t);
        const Vec3 r0 = Lerp (q0, q1, t), r1 = Lerp (q1, q2, t), s = Lerp (r0, r1, t);
        const Vec3 on = SegmentPoint (*e, static_cast<size_t> (k), t);
        a.out = q0 - p0;
        b.in = q2 - p3;
        a.out.z = b.in.z = 0.0;
        v.p = on;
        v.in = { r0.x - s.x, r0.y - s.y, 0.0 };
        v.out = { r1.x - s.x, r1.y - s.y, 0.0 };
        v.smooth = true;
    }
    const Segment second = CopyAttributes (e->segments[static_cast<size_t> (k)], doc);
    e->vertices.insert (e->vertices.begin () + static_cast<std::ptrdiff_t> (at1), v);
    e->segments.insert (e->segments.begin () + static_cast<std::ptrdiff_t> (at1), second);
    if (created != nullptr)
        *created = v.id;
    return true;
}

bool DeleteVertex (Document& doc, Id entity, Id vertex, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const int found = e != nullptr ? VertexAt (*e, vertex, error) : -1;
    if (found < 0)
        return false;
    const size_t i = static_cast<size_t> (found), n = e->vertices.size ();
    if (n <= MinVertices (*e)) {
        error = e->closed ? "a closed outline needs three vertices" : "a polyline needs two vertices";
        return false;
    }
    if (e->vertices[i].lockXY || e->vertices[i].lockZ) {
        error = "that vertex is locked";
        return false;
    }
    // The segment leaving the vertex goes; the one arriving stretches to the next vertex.
    // An open polyline's last vertex has no leaving segment, so its arriving one goes.
    const size_t drop = (!e->closed && i == n - 1) ? i - 1 : i;
    e->vertices.erase (e->vertices.begin () + static_cast<std::ptrdiff_t> (i));
    e->segments.erase (e->segments.begin () + static_cast<std::ptrdiff_t> (drop));
    return true;
}

bool MergeVertices (Document& doc, Id entity, double tolerance, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    if (e == nullptr)
        return false;
    const auto locked = [] (const Vertex& v) { return v.lockXY || v.lockZ; };
    for (size_t i = 0; i < SegmentCount (*e);) {
        size_t from = 0, to = 0;
        SegmentEnds (*e, i, from, to);
        if (Distance (e->vertices[from].p, e->vertices[to].p) >= tolerance) {
            ++i;
            continue;
        }
        if (e->vertices.size () <= MinVertices (*e)) {
            error = "merging would leave too few vertices";
            return false;
        }
        // Dropping the short segment i with either of its ends leaves the neighbouring
        // segments joined correctly, except dropping vertex 0 (the closing wrap), which
        // would shift every later segment. Keep a locked end where one is locked.
        size_t gone = from;
        if (locked (e->vertices[from])) {
            if (to == 0 || locked (e->vertices[to])) {
                error = "merging would move a locked vertex";
                return false;
            }
            gone = to;
        }
        e->vertices.erase (e->vertices.begin () + static_cast<std::ptrdiff_t> (gone));
        e->segments.erase (e->segments.begin () + static_cast<std::ptrdiff_t> (i));
    }
    return true;
}

bool OffsetSegment (Document& doc, Id entity, Id segment, double distance, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const int k = e != nullptr ? SegmentAt (*e, segment, error) : -1;
    if (k < 0)
        return false;
    const size_t n = e->vertices.size ();
    size_t from = 0, to = 0;
    SegmentEnds (*e, static_cast<size_t> (k), from, to);
    Vertex &a = e->vertices[from], &b = e->vertices[to];
    if (a.lockXY || b.lockXY) {
        error = "a locked vertex cannot be offset";
        return false;
    }
    Vec3 dir;
    double length = 0.0;
    if (!PlanDirection (a.p, b.p, dir, length)) {
        error = "a vertical segment has no plan offset";
        return false;
    }
    const Vec3 shift = LeftNormal (dir) * distance;
    const Vec3 a1 = a.p + shift, b1 = b.p + shift;
    // Each end slides along its neighbour's line, so the neighbour keeps its direction.
    const auto slide = [&] (size_t end, size_t neighbour, Vec3 fallback) {
        Vec3 out = fallback;
        double t = 0.0, s = 0.0;
        if (CrossPlan (a1, b1, e->vertices[neighbour].p, e->vertices[end].p, t, s)) {
            const Vec3 hit = Lerp (a1, b1, t);
            out = { hit.x, hit.y, e->vertices[end].p.z };
        }
        return out;
    };
    const bool hasPrev = e->closed || from > 0, hasNext = e->closed || to + 1 < n;
    const Vec3 newA = hasPrev ? slide (from, (from + n - 1) % n, a1) : a1;
    const Vec3 newB = hasNext ? slide (to, (to + 1) % n, b1) : b1;
    Vec3 check;
    double newLength = 0.0;
    if (!PlanDirection (newA, newB, check, newLength) || Dot (check, dir) <= 0.0) {
        error = "the offset turns the segment inside out";
        return false;
    }
    // A neighbour that slid past its own far vertex has reversed: refuse that too.
    const auto keeps = [&] (Vec3 from0, Vec3 to0, Vec3 from1, Vec3 to1) {
        Vec3 d0, d1;
        double l0 = 0.0, l1 = 0.0;
        return PlanDirection (from0, to0, d0, l0) && PlanDirection (from1, to1, d1, l1) && Dot (d0, d1) > 0.0;
    };
    const Vec3 prev = e->vertices[(from + n - 1) % n].p, next = e->vertices[(to + 1) % n].p;
    if ((hasPrev && !keeps (prev, a.p, prev, newA)) || (hasNext && !keeps (b.p, next, newB, next))) {
        error = "the offset runs past a neighbouring vertex";
        return false;
    }
    a.p = newA;
    b.p = newB;
    return true;
}

bool OffsetEntity (Document& doc, Id entity, double distance, std::string& error, Id* created)
{
    const Entity* found = Find (doc, entity);
    if (found == nullptr) {
        error = "the entity no longer exists";
        return false;
    }
    Entity copy = *found;
    const size_t n = copy.vertices.size (), segs = SegmentCount (copy);
    std::vector<Vec3> dirs (segs);
    for (size_t i = 0; i < segs; ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (copy, i, from, to);
        double length = 0.0;
        if (!PlanDirection (copy.vertices[from].p, copy.vertices[to].p, dirs[i], length)) {
            error = "a vertical segment has no plan offset";
            return false;
        }
    }
    // An extrusion profile is counter-clockwise: positive grows it outward.
    const double d = copy.kind == Kind::Extrusion ? -distance : distance;
    std::vector<Vec3> moved (n);
    for (size_t i = 0; i < n; ++i) {
        const bool hasIn = copy.closed || i > 0, hasOut = copy.closed || i + 1 < n;
        const Vec3 p = copy.vertices[i].p;
        if (!hasIn || !hasOut) {
            moved[i] = p + LeftNormal (dirs[hasOut ? i : i - 1]) * d;
            continue;
        }
        const Vec3 din = dirs[(i + segs - 1) % segs], dout = dirs[i % segs];
        const Vec3 nin = LeftNormal (din), nout = LeftNormal (dout);
        double t = 0.0, s = 0.0;
        const Vec3 a = p + nin * d, b = p + nout * d;
        if (!CrossPlan (a - din, a, b, b + dout, t, s)) {
            moved[i] = b; // collinear: both offsets agree
            continue;
        }
        const Vec3 corner = Lerp (a - din, a, t);
        if (DistanceXY (corner, p) > 10.0 * std::abs (d) + kMinSegment) {
            error = "a corner is too sharp to offset";
            return false;
        }
        moved[i] = { corner.x, corner.y, p.z };
    }
    for (size_t i = 0; i < segs; ++i) {
        size_t from = 0, to = 0;
        SegmentEnds (copy, i, from, to);
        Vec3 dir;
        double length = 0.0;
        if (!PlanDirection (moved[from], moved[to], dir, length) || Dot (dir, dirs[i]) <= 0.0) {
            error = "the offset is larger than the outline allows";
            return false;
        }
    }
    copy.id = kNoId;
    for (size_t i = 0; i < n; ++i) {
        copy.vertices[i].id = kNoId;
        copy.vertices[i].p = moved[i];
        copy.vertices[i].lockXY = copy.vertices[i].lockZ = false;
    }
    for (auto& s : copy.segments)
        s.id = kNoId;
    Normalize (doc, copy);
    doc.entities.push_back (copy);
    if (created != nullptr)
        *created = copy.id;
    return true;
}

bool ExtendToEntity (Document& doc, Id entity, bool atStart, Id target, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const Entity* t = Find (doc, target);
    if (e == nullptr || !Editable (*e, error))
        return false;
    if (t == nullptr || target == entity || e->closed) {
        error = e->closed ? "a closed outline has no end to extend" : "pick another entity to extend to";
        return false;
    }
    const size_t n = e->vertices.size ();
    Vertex& end = e->vertices[atStart ? 0 : n - 1];
    const Vec3 inner = e->vertices[atStart ? 1 : n - 2].p;
    if (end.lockXY) {
        error = "that end is locked";
        return false;
    }
    double best = std::numeric_limits<double>::infinity ();
    for (const auto& [c, d] : EdgesOf (*t)) {
        double u = 0.0, s = 0.0;
        if (CrossPlan (inner, end.p, c, d, u, s) && u > 1.0 + 1.0e-9 && s >= -1.0e-9 && s <= 1.0 + 1.0e-9)
            best = std::min (best, u);
    }
    if (!std::isfinite (best)) {
        error = "the extension never meets that entity";
        return false;
    }
    const Vec3 p = Lerp (inner, end.p, best); // the slope continues with the segment
    end.p = end.lockZ ? Vec3 { p.x, p.y, end.p.z } : p;
    return true;
}

bool TrimAtEntity (Document& doc, Id entity, bool atStart, Id target, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const Entity* t = Find (doc, target);
    if (e == nullptr || !Editable (*e, error))
        return false;
    if (t == nullptr || target == entity || e->closed) {
        error = e->closed ? "a closed outline has no end to trim" : "pick another entity to trim at";
        return false;
    }
    const auto edges = EdgesOf (*t);
    if (!atStart)
        Reverse (*e);
    bool done = false;
    for (size_t k = 0; k + 1 < e->vertices.size () && !done; ++k) {
        double first = std::numeric_limits<double>::infinity ();
        const Vec3 a = e->vertices[k].p, b = e->vertices[k + 1].p;
        for (const auto& [c, d] : edges) {
            double u = 0.0, s = 0.0;
            if (CrossPlan (a, b, c, d, u, s) && u > 1.0e-9 && u < 1.0 - 1.0e-9 && s >= -1.0e-9 && s <= 1.0 + 1.0e-9)
                first = std::min (first, u);
        }
        if (!std::isfinite (first))
            continue;
        for (size_t i = 0; i <= k; ++i)
            if (e->vertices[i].lockXY) {
                error = "trimming would remove a locked vertex";
                if (!atStart)
                    Reverse (*e);
                return false;
            }
        // Vertices before the cut go; the cut segment keeps its id and starts at the cut.
        Vertex cut = e->vertices[k];
        cut.p = Lerp (a, b, first);
        e->vertices.erase (e->vertices.begin (), e->vertices.begin () + static_cast<std::ptrdiff_t> (k + 1));
        e->vertices.insert (e->vertices.begin (), cut);
        e->segments.erase (e->segments.begin (), e->segments.begin () + static_cast<std::ptrdiff_t> (k));
        done = true;
    }
    if (!atStart)
        Reverse (*e);
    if (!done)
        error = "the polyline never meets that entity";
    return done;
}

bool SplitAtVertex (Document& doc, Id entity, Id vertex, std::string& error, Id* created)
{
    Entity* e = Get (doc, entity, error);
    const int found = e != nullptr ? VertexAt (*e, vertex, error) : -1;
    if (found < 0 || !Editable (*e, error))
        return false;
    const size_t i = static_cast<size_t> (found), n = e->vertices.size ();
    if (e->closed) {
        // Open the ring at the vertex: it becomes both ends.
        std::rotate (e->vertices.begin (), e->vertices.begin () + static_cast<std::ptrdiff_t> (i), e->vertices.end ());
        std::rotate (e->segments.begin (), e->segments.begin () + static_cast<std::ptrdiff_t> (i), e->segments.end ());
        Vertex end = e->vertices.front ();
        end.id = NewId (doc);
        e->vertices.push_back (end);
        e->closed = false;
        if (created != nullptr)
            *created = kNoId;
        return true;
    }
    if (i == 0 || i + 1 >= n) {
        error = "split at an interior vertex";
        return false;
    }
    Entity tail = *e;
    tail.id = NewId (doc);
    tail.vertices.assign (e->vertices.begin () + static_cast<std::ptrdiff_t> (i), e->vertices.end ());
    tail.vertices.front ().id = NewId (doc);
    tail.segments.assign (e->segments.begin () + static_cast<std::ptrdiff_t> (i), e->segments.end ());
    e->vertices.resize (i + 1);
    e->segments.resize (i);
    if (created != nullptr)
        *created = tail.id;
    doc.entities.push_back (tail);
    return true;
}

bool Join (Document& doc, Id first, Id second, double tolerance, std::string& error)
{
    Entity* a = Get (doc, first, error);
    Entity* b = Get (doc, second, error);
    if (a == nullptr || b == nullptr)
        return false;
    if (first == second || a->closed || b->closed || a->kind != Kind::Polyline || b->kind != Kind::Polyline) {
        error = "join two different open polylines";
        return false;
    }
    const auto near = [&] (const Vertex& x, const Vertex& y) { return Distance (x.p, y.p) <= tolerance; };
    // Orient both so that a's end meets tail's start.
    Entity tail = *b;
    if (near (a->vertices.front (), tail.vertices.front ()) || near (a->vertices.front (), tail.vertices.back ())) {
        if (!near (a->vertices.back (), tail.vertices.front ()) && !near (a->vertices.back (), tail.vertices.back ()))
            Reverse (*a);
    }
    if (!near (a->vertices.back (), tail.vertices.front ()) && near (a->vertices.back (), tail.vertices.back ()))
        Reverse (tail);
    if (!near (a->vertices.back (), tail.vertices.front ())) {
        error = "the polylines do not meet";
        return false;
    }
    a->vertices.insert (a->vertices.end (), tail.vertices.begin () + 1, tail.vertices.end ());
    a->segments.insert (a->segments.end (), tail.segments.begin (), tail.segments.end ());
    if (a->vertices.size () > 3 && near (a->vertices.front (), a->vertices.back ())) {
        a->vertices.pop_back (); // the last segment now closes onto the first vertex
        a->closed = true;
    }
    doc.entities.erase (
        std::remove_if (doc.entities.begin (), doc.entities.end (), [&] (const Entity& x) { return x.id == second; }),
        doc.entities.end ());
    return true;
}

namespace {

struct Corner {
    size_t prev = 0, at = 0, next = 0;
    Vec3 u, w; // plan directions from the corner to its neighbours
    double toPrev = 0.0, toNext = 0.0, angle = 0.0;
};

bool CornerAt (const Entity& e, size_t i, Corner& c, std::string& error)
{
    const size_t n = e.vertices.size ();
    if (!e.closed && (i == 0 || i + 1 >= n)) {
        error = "an end of a polyline has no corner";
        return false;
    }
    if (e.vertices[i].lockXY) {
        error = "that corner is locked";
        return false;
    }
    c.at = i;
    c.prev = (i + n - 1) % n;
    c.next = (i + 1) % n;
    const Vec3 p = e.vertices[i].p;
    if (!PlanDirection (p, e.vertices[c.prev].p, c.u, c.toPrev) ||
        !PlanDirection (p, e.vertices[c.next].p, c.w, c.toNext)) {
        error = "a corner needs two plan segments";
        return false;
    }
    c.angle = std::acos (std::clamp (Dot (c.u, c.w), -1.0, 1.0));
    if (c.angle > kPi - 1.0e-6) {
        error = "the vertex is not a corner";
        return false;
    }
    return true;
}

// Replaces the corner vertex by `points` (first keeps the corner's id): the arriving
// segment keeps its id, the leaving one keeps its id from the last point, and the
// pieces between copy the arriving segment's attributes.
void ReplaceCorner (Document& doc, Entity& e, const Corner& c, const std::vector<Vec3>& points)
{
    const size_t i = c.at;
    const Segment arriving = e.segments[(i + e.segments.size () - 1) % e.segments.size ()];
    std::vector<Vertex> verts;
    std::vector<Segment> pieces;
    for (size_t k = 0; k < points.size (); ++k) {
        Vertex v = e.vertices[i];
        v.p = points[k];
        if (k > 0)
            v.id = NewId (doc);
        verts.push_back (v);
        if (k + 1 < points.size ())
            pieces.push_back (CopyAttributes (arriving, doc));
    }
    e.vertices.erase (e.vertices.begin () + static_cast<std::ptrdiff_t> (i));
    e.vertices.insert (e.vertices.begin () + static_cast<std::ptrdiff_t> (i), verts.begin (), verts.end ());
    // Segment i (leaving) is pushed after the new pieces.
    e.segments.insert (e.segments.begin () + static_cast<std::ptrdiff_t> (i), pieces.begin (), pieces.end ());
}

} // namespace

bool Fillet (Document& doc, Id entity, Id vertex, double radius, int arcSegments, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const int found = e != nullptr ? VertexAt (*e, vertex, error) : -1;
    if (found < 0)
        return false;
    if (!(radius > 0.0) || arcSegments < 1) {
        error = "a fillet needs a positive radius";
        return false;
    }
    Corner c;
    if (!CornerAt (*e, static_cast<size_t> (found), c, error))
        return false;
    const double tangent = radius / std::tan (c.angle * 0.5);
    if (tangent >= c.toPrev - kMinSegment || tangent >= c.toNext - kMinSegment) {
        error = "the radius does not fit between the neighbouring vertices";
        return false;
    }
    const Vec3 p = e->vertices[c.at].p, pp = e->vertices[c.prev].p, pn = e->vertices[c.next].p;
    Vec3 t1 = Lerp (p, pp, tangent / c.toPrev), t2 = Lerp (p, pn, tangent / c.toNext);
    Vec3 bisector;
    Normalize (c.u + c.w, bisector);
    const Vec3 centre = p + bisector * (radius / std::sin (c.angle * 0.5));
    const double a1 = std::atan2 (t1.y - centre.y, t1.x - centre.x);
    double sweep = std::atan2 (t2.y - centre.y, t2.x - centre.x) - a1;
    while (sweep > kPi)
        sweep -= 2.0 * kPi;
    while (sweep < -kPi)
        sweep += 2.0 * kPi;
    std::vector<Vec3> points;
    for (int k = 0; k <= arcSegments; ++k) {
        const double f = static_cast<double> (k) / arcSegments;
        const double a = a1 + sweep * f;
        points.push_back (
            { centre.x + radius * std::cos (a), centre.y + radius * std::sin (a), t1.z + (t2.z - t1.z) * f });
    }
    points.front () = t1;
    points.back () = t2;
    ReplaceCorner (doc, *e, c, points);
    return true;
}

bool Chamfer (Document& doc, Id entity, Id vertex, double distance, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    const int found = e != nullptr ? VertexAt (*e, vertex, error) : -1;
    if (found < 0)
        return false;
    Corner c;
    if (!CornerAt (*e, static_cast<size_t> (found), c, error))
        return false;
    if (!(distance > 0.0) || distance >= c.toPrev - kMinSegment || distance >= c.toNext - kMinSegment) {
        error = "the chamfer does not fit between the neighbouring vertices";
        return false;
    }
    const Vec3 p = e->vertices[c.at].p;
    ReplaceCorner (
        doc, *e, c,
        { Lerp (p, e->vertices[c.prev].p, distance / c.toPrev), Lerp (p, e->vertices[c.next].p, distance / c.toNext) });
    return true;
}

bool SetCoordinate (Document& doc, const std::vector<Ref>& refs, int axis, double value, std::string& error)
{
    if (axis < 0 || axis > 2 || !std::isfinite (value)) {
        error = "pick X, Y or Z and a number";
        return false;
    }
    for (const auto& r : refs) {
        Entity* e = Get (doc, r.entity, error);
        if (e == nullptr || !Exists (doc, r)) {
            error = "a selected item no longer exists";
            return false;
        }
        std::set<size_t> hit;
        if (r.part == Part::Vertex)
            hit.insert (static_cast<size_t> (VertexIndex (*e, r.item)));
        else if (r.part == Part::Segment) {
            size_t from = 0, to = 0;
            SegmentEnds (*e, static_cast<size_t> (SegmentIndex (*e, r.item)), from, to);
            hit = { from, to };
        }
        else
            for (size_t i = 0; i < e->vertices.size (); ++i)
                hit.insert (i);
        if (axis == 2 && e->kind == Kind::Extrusion && hit.size () != e->vertices.size ()) {
            error = "an extrusion profile stays flat";
            return false;
        }
        for (size_t i : hit) {
            Vertex& v = e->vertices[i];
            if ((axis == 2 && v.lockZ) || (axis < 2 && v.lockXY)) {
                error = "a selected vertex is locked";
                return false;
            }
            (axis == 0 ? v.p.x : axis == 1 ? v.p.y : v.p.z) = value;
        }
    }
    return true;
}

bool ProjectToSurface (Document& doc, Id entity, const HeightField& height, std::string& error)
{
    Entity* e = Get (doc, entity, error);
    if (e == nullptr || !Editable (*e, error))
        return false;
    if (!height) {
        error = "there is no surface to project onto";
        return false;
    }
    for (auto& v : e->vertices) {
        double z = 0.0;
        if (!height (v.p.x, v.p.y, z) || !std::isfinite (z)) {
            error = "part of the polyline lies outside the surface";
            return false;
        }
        if (v.lockZ)
            continue; // a fixed elevation wins over the surface
        v.p.z = z;
    }
    return true;
}

std::vector<Action> PolylineActions (const ActionDefaults& d)
{
    using Refs = std::vector<Ref>;
    const uint32_t vertex = Bit (SelectionKind::Vertex), segment = Bit (SelectionKind::Segment),
                   entity = Bit (SelectionKind::Entity);
    std::vector<Action> table;
    table.push_back (
        { "insert-vertex", "Insert vertex", segment, 0xFu, 1, 1, [] (const Document& doc, const Refs& refs) {
             Vec3 mid;
             const Ref r = refs.front ();
             if (!PointOf (doc, r, mid))
                 return Operation {};
             return Operation (
                 [r, mid] (Document& x, std::string& error) { return InsertVertex (x, r.entity, r.item, mid, error); });
         } });
    table.push_back ({ "delete-vertex", "Delete vertex", vertex, 0xFu, 1, 0, [] (const Document&, const Refs& refs) {
                          return Operation ([refs] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!DeleteVertex (x, r.entity, r.item, error))
                                      return false;
                              return true;
                          });
                      } });
    // A fillet is a live corner: it keeps its radius, editable by its grip or by a typed
    // value, until it is baked into points.
    table.push_back ({ "fillet", "Round corner", vertex, 0xFu, 1, 0, [d] (const Document&, const Refs& refs) {
                          return Operation ([refs, d] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!SetCornerRadius (x, r.entity, { r.item }, d.filletRadius, error))
                                      return false;
                              return true;
                          });
                      } });
    table.push_back ({ "sharpen", "Sharp corner", vertex, 0xFu, 1, 0, [] (const Document& doc, const Refs& refs) {
                          bool any = false;
                          for (const auto& r : refs)
                              if (const Entity* e = Find (doc, r.entity))
                                  for (const auto& v : e->vertices)
                                      any = any || (v.id == r.item && v.radius > 0.0);
                          if (!any)
                              return Operation {};
                          return Operation ([refs] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!SetCornerRadius (x, r.entity, { r.item }, 0.0, error))
                                      return false;
                              return true;
                          });
                      } });
    // A centreline drawn as a polyline -- lines and rounded corners -- becomes a path.
    table.push_back (
        { "make-path", "Make a path along it", entity, 0xFu, 1, 0, [d] (const Document& doc, const Refs& refs) {
             for (const auto& r : refs) {
                 const Entity* e = Find (doc, r.entity);
                 if (e == nullptr || e->kind != Kind::Polyline || r.part != Part::Entity)
                     return Operation {};
             }
             return Operation ([refs, d] (Document& x, std::string& error) {
                 for (const auto& r : refs)
                     if (PathFromPolyline (x, r.entity, d.pathWidth, error) == kNoId)
                         return false;
                 return true;
             });
         } });
    table.push_back ({ "bake-corners", "Turn rounded corners into points", entity, 0xFu, 1, 0,
                       [] (const Document& doc, const Refs& refs) {
                           bool any = false;
                           for (const auto& r : refs)
                               if (const Entity* e = Find (doc, r.entity))
                                   for (const auto& v : e->vertices)
                                       any = any || v.radius > 0.0;
                           if (!any)
                               return Operation {};
                           return Operation ([refs] (Document& x, std::string& error) {
                               for (const auto& r : refs) {
                                   std::vector<std::pair<Id, double>> rounded;
                                   if (const Entity* e = Find (x, r.entity))
                                       for (size_t i = 0; i < e->vertices.size (); ++i)
                                           if (e->vertices[i].radius > 0.0)
                                               rounded.push_back ({ e->vertices[i].id, DrawnRadius (*e, i) });
                                   for (const auto& [id, radius] : rounded) {
                                       Entity* e = Find (x, r.entity);
                                       const int i = VertexIndex (*e, id);
                                       if (i < 0)
                                           continue;
                                       e->vertices[size_t (i)].radius = 0.0;
                                       if (radius > 0.0 && !Fillet (x, r.entity, id, radius, 8, error))
                                           return false;
                                   }
                               }
                               return true;
                           });
                       } });
    const auto anchors = [] (const Document& doc, const Refs& refs, bool smooth) {
        bool any = false; // something to change: a corner to smooth, or handles to retract
        for (const auto& r : refs)
            if (const Entity* e = Find (doc, r.entity))
                for (const auto& v : e->vertices)
                    if (v.id == r.item)
                        any = any || (smooth ? !v.smooth
                                             : (v.smooth || std::hypot (v.in.x, v.in.y) > 0.0 ||
                                                std::hypot (v.out.x, v.out.y) > 0.0));
        if (!any)
            return Operation {};
        return Operation ([refs, smooth] (Document& x, std::string& error) {
            for (const auto& r : refs)
                if (!SetSmooth (x, r.entity, r.item, smooth, error))
                    return false;
            return true;
        });
    };
    table.push_back ({ "smooth", "Smooth point", vertex, 0xFu, 1, 0,
                       [anchors] (const Document& doc, const Refs& refs) { return anchors (doc, refs, true); } });
    table.push_back ({ "corner", "Corner point", vertex, 0xFu, 1, 0,
                       [anchors] (const Document& doc, const Refs& refs) { return anchors (doc, refs, false); } });
    const auto segments = [] (const Document& doc, const Refs& refs, bool curved) {
        bool any = false;
        for (const auto& r : refs)
            if (const Entity* e = Find (doc, r.entity)) {
                const int i = SegmentIndex (*e, r.item);
                any = any || (i >= 0 && SegmentCurved (*e, size_t (i)) != curved);
            }
        if (!any)
            return Operation {};
        return Operation ([refs, curved] (Document& x, std::string& error) {
            for (const auto& r : refs)
                if (!SetSegmentCurved (x, r.entity, r.item, curved, error))
                    return false;
            return true;
        });
    };
    table.push_back ({ "curve", "Make curve", segment, 0xFu, 1, 0,
                       [segments] (const Document& doc, const Refs& refs) { return segments (doc, refs, true); } });
    table.push_back ({ "straighten", "Make straight", segment, 0xFu, 1, 0,
                       [segments] (const Document& doc, const Refs& refs) { return segments (doc, refs, false); } });
    table.push_back ({ "chamfer", "Chamfer", vertex, 0xFu, 1, 0, [d] (const Document&, const Refs& refs) {
                          return Operation ([refs, d] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!Chamfer (x, r.entity, r.item, d.chamferDistance, error))
                                      return false;
                              return true;
                          });
                      } });
    table.push_back ({ "split", "Split here", vertex, 0xFu, 1, 1, [] (const Document&, const Refs& refs) {
                          const Ref r = refs.front ();
                          return Operation ([r] (Document& x, std::string& error) {
                              return SplitAtVertex (x, r.entity, r.item, error);
                          });
                      } });
    table.push_back ({ "join", "Join", entity, 0xFu, 2, 2, [d] (const Document&, const Refs& refs) {
                          const Id a = refs[0].entity, b = refs[1].entity;
                          return Operation ([a, b, d] (Document& x, std::string& error) {
                              return Join (x, a, b, d.joinTolerance, error);
                          });
                      } });
    table.push_back (
        { "merge", "Merge duplicate vertices", entity, 0xFu, 1, 0, [d] (const Document&, const Refs& refs) {
             return Operation ([refs, d] (Document& x, std::string& error) {
                 for (const auto& r : refs)
                     if (!MergeVertices (x, r.entity, d.mergeTolerance, error))
                         return false;
                 return true;
             });
         } });
    return table;
}

} // namespace geomsrv::edit
