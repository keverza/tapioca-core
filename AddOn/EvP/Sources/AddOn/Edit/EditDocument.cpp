#include "Edit/EditDocument.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace geomsrv::edit {

bool operator== (const Ref& a, const Ref& b)
{
    return a.entity == b.entity && a.part == b.part && a.item == b.item && a.revision == b.revision;
}

bool operator!= (const Ref& a, const Ref& b)
{
    return !(a == b);
}

Id NewId (Document& doc)
{
    if (doc.nextId == kNoId || doc.nextId == std::numeric_limits<Id>::max ())
        return kNoId;
    return doc.nextId++;
}

Entity* Find (Document& doc, Id entity)
{
    for (auto& e : doc.entities)
        if (e.id == entity)
            return &e;
    return nullptr;
}

const Entity* Find (const Document& doc, Id entity)
{
    for (const auto& e : doc.entities)
        if (e.id == entity)
            return &e;
    return nullptr;
}

Path* FindPath (Document& doc, Id path)
{
    for (auto& p : doc.paths)
        if (p.id == path)
            return &p;
    return nullptr;
}

const Path* FindPath (const Document& doc, Id path)
{
    for (const auto& p : doc.paths)
        if (p.id == path)
            return &p;
    return nullptr;
}

namespace {

bool PathPart (const Path& path, const Ref& ref, double& station)
{
    AlignmentGeometry g;
    std::string error;
    if (!BuildAlignment (path.points, g, error))
        return false;
    switch (ref.part) {
        case Part::Entity:
            station = g.length * 0.5;
            return true;
        case Part::PathPoint:
            for (size_t i = 0; i < path.points.size (); ++i)
                if (path.points[i].id == ref.item) {
                    station = g.pointStations[i];
                    return true;
                }
            return false;
        case Part::Break:
            for (const auto& b : path.profile)
                if (b.id == ref.item) {
                    station = b.station;
                    return true;
                }
            return false;
        case Part::Range:
            for (const auto& r : path.sections)
                if (r.id == ref.item) {
                    station = r.station;
                    return true;
                }
            return false;
        default:
            return false;
    }
}

} // namespace

int VertexIndex (const Entity& entity, Id vertex)
{
    for (size_t i = 0; i < entity.vertices.size (); ++i)
        if (entity.vertices[i].id == vertex)
            return static_cast<int> (i);
    return -1;
}

int SegmentIndex (const Entity& entity, Id segment)
{
    for (size_t i = 0; i < entity.segments.size (); ++i)
        if (entity.segments[i].id == segment)
            return static_cast<int> (i);
    return -1;
}

size_t SegmentCount (const Entity& entity)
{
    const size_t n = entity.vertices.size ();
    if (n < 2)
        return 0;
    return entity.closed ? n : n - 1;
}

size_t MinVertices (const Entity& entity)
{
    return entity.closed ? 3 * std::max<size_t> (1, entity.contours.size ()) : 2;
}

void SegmentEnds (const Entity& entity, size_t index, size_t& from, size_t& to)
{
    from = index;
    to = NextVertex (entity, index);
}

bool IsCompound (const Entity& entity)
{
    return entity.contours.size () > 1;
}

size_t ContourCount (const Entity& entity)
{
    if (!entity.contours.empty ())
        return entity.contours.size ();
    return entity.vertices.empty () ? 0 : 1;
}

void ContourRange (const Entity& entity, size_t contour, size_t& begin, size_t& end)
{
    if (entity.contours.empty ()) {
        begin = 0;
        end = contour == 0 ? entity.vertices.size () : 0;
        return;
    }
    begin = 0;
    for (size_t c = 0; c < contour && c < entity.contours.size (); ++c)
        begin += entity.contours[c].count;
    end = contour < entity.contours.size () ? begin + entity.contours[contour].count : begin;
    begin = std::min (begin, entity.vertices.size ());
    end = std::min (end, entity.vertices.size ());
}

size_t ContourOf (const Entity& entity, size_t index)
{
    size_t begin = 0;
    for (size_t c = 0; c < entity.contours.size (); ++c) {
        begin += entity.contours[c].count;
        if (index < begin)
            return c;
    }
    return entity.contours.empty () ? 0 : entity.contours.size () - 1;
}

int ContourIndex (const Entity& entity, Id contour)
{
    for (size_t c = 0; c < entity.contours.size (); ++c)
        if (entity.contours[c].id == contour)
            return static_cast<int> (c);
    return -1;
}

size_t NextVertex (const Entity& entity, size_t index)
{
    const size_t n = entity.vertices.size ();
    if (n == 0)
        return 0;
    if (entity.contours.empty ())
        return (index + 1) % n;
    size_t begin = 0, end = 0;
    ContourRange (entity, ContourOf (entity, index), begin, end);
    return index + 1 < end ? index + 1 : begin;
}

size_t PrevVertex (const Entity& entity, size_t index)
{
    const size_t n = entity.vertices.size ();
    if (n == 0)
        return 0;
    if (entity.contours.empty ())
        return (index + n - 1) % n;
    size_t begin = 0, end = 0;
    ContourRange (entity, ContourOf (entity, index), begin, end);
    return index > begin ? index - 1 : (end > begin ? end - 1 : begin);
}

bool OpenEnd (const Entity& entity, size_t index)
{
    return !entity.closed && (index == 0 || index + 1 >= entity.vertices.size ());
}

std::vector<Vec3> ContourPoints (const Entity& entity, size_t contour)
{
    size_t begin = 0, end = 0;
    ContourRange (entity, contour, begin, end);
    std::vector<Vec3> out;
    for (size_t i = begin; i < end; ++i)
        out.push_back (entity.vertices[i].p);
    return out;
}

namespace {

double RingArea (const std::vector<Vec3>& ring)
{
    double twice = 0.0;
    for (size_t i = 0; i < ring.size (); ++i) {
        const Vec3 a = ring[i], b = ring[(i + 1) % ring.size ()];
        twice += a.x * b.y - b.x * a.y;
    }
    return twice * 0.5;
}

// Crossing-number test on anchors; a point on the boundary may go either way.
bool InsideRing (const std::vector<Vec3>& ring, Vec3 p)
{
    bool in = false;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++) {
        const Vec3 a = ring[i], b = ring[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

} // namespace

size_t ContourDepth (const Entity& entity, size_t contour)
{
    const size_t count = ContourCount (entity);
    if (contour >= count || count < 2)
        return 0;
    const auto mine = ContourPoints (entity, contour);
    if (mine.empty ())
        return 0;
    size_t depth = 0;
    for (size_t c = 0; c < count; ++c)
        if (c != contour) {
            const auto other = ContourPoints (entity, c);
            if (other.size () >= 3 && InsideRing (other, mine.front ()))
                ++depth;
        }
    return depth;
}

double SignedAreaXY (const Entity& entity)
{
    double area = 0.0;
    for (size_t c = 0; c < ContourCount (entity); ++c)
        area += RingArea (ContourPoints (entity, c));
    return area;
}

bool InsideXY (const Entity& entity, Vec3 point)
{
    bool in = false;
    for (size_t c = 0; c < ContourCount (entity); ++c) {
        const auto ring = ContourPoints (entity, c);
        if (ring.size () >= 3 && InsideRing (ring, point))
            in = !in;
    }
    return in;
}

size_t FaceCount (const Entity& entity)
{
    return entity.kind == Kind::Extrusion ? 2 + entity.vertices.size () : 0;
}

std::vector<std::vector<Vec3>> FaceRings (const Entity& entity, size_t face)
{
    std::vector<std::vector<Vec3>> out;
    if (entity.kind != Kind::Extrusion || face >= FaceCount (entity))
        return out;
    const Vec3 up { 0.0, 0.0, entity.height };
    if (face >= 2) {
        const size_t i = face - 2;
        const Vec3 a = entity.vertices[i].p, b = entity.vertices[NextVertex (entity, i)].p;
        out.push_back ({ a, b, b + up, a + up });
        return out;
    }
    // Each contour already winds with the solid on its left; the bottom is seen from below,
    // so it runs every ring backwards.
    for (size_t c = 0; c < ContourCount (entity); ++c) {
        auto ring = ContourPoints (entity, c);
        if (face == 0)
            std::reverse (ring.begin (), ring.end ());
        else
            for (auto& p : ring)
                p = p + up;
        out.push_back (std::move (ring));
    }
    return out;
}

std::vector<Vec3> FacePolygon (const Entity& entity, size_t face)
{
    auto rings = FaceRings (entity, face);
    return rings.empty () ? std::vector<Vec3> {} : std::move (rings.front ());
}

bool FaceNormal (const Entity& entity, size_t face, Vec3& normal)
{
    if (entity.kind != Kind::Extrusion || face >= FaceCount (entity))
        return false;
    if (face < 2) {
        normal = { 0.0, 0.0, face == 0 ? -1.0 : 1.0 };
        return true;
    }
    const size_t i = face - 2;
    const Vec3 a = entity.vertices[i].p, b = entity.vertices[NextVertex (entity, i)].p;
    // Every contour has the solid on its left (holes wind clockwise), so the outside is to
    // the right of each edge -- into a hole for a hole's side.
    return Normalize (Vec3 { b.y - a.y, a.x - b.x, 0.0 }, normal);
}

void Normalize (Document& doc, Entity& entity)
{
    if (entity.id == kNoId)
        entity.id = NewId (doc);
    for (auto& v : entity.vertices)
        if (v.id == kNoId)
            v.id = NewId (doc);
    entity.segments.resize (SegmentCount (entity));
    for (auto& s : entity.segments)
        if (s.id == kNoId)
            s.id = NewId (doc);
    for (auto& c : entity.contours)
        if (c.id == kNoId)
            c.id = NewId (doc);
}

double SignedAreaXY (const std::vector<Vertex>& vertices)
{
    double twice = 0.0;
    const size_t n = vertices.size ();
    for (size_t i = 0; i < n; ++i) {
        const Vec3 a = vertices[i].p, b = vertices[(i + 1) % n].p;
        twice += a.x * b.y - b.x * a.y;
    }
    return twice * 0.5;
}

namespace {

// Proper or touching intersection of two plan segments (shared endpoints count).
bool SegmentsMeetXY (Vec3 a, Vec3 b, Vec3 c, Vec3 d)
{
    const auto orient = [] (Vec3 p, Vec3 q, Vec3 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
    const auto within = [] (Vec3 p, Vec3 q, Vec3 r) {
        return std::min (p.x, q.x) <= r.x && r.x <= std::max (p.x, q.x) && std::min (p.y, q.y) <= r.y &&
               r.y <= std::max (p.y, q.y);
    };
    const double d1 = orient (c, d, a), d2 = orient (c, d, b), d3 = orient (a, b, c), d4 = orient (a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;
    return (d1 == 0 && within (c, d, a)) || (d2 == 0 && within (c, d, b)) || (d3 == 0 && within (a, b, c)) ||
           (d4 == 0 && within (a, b, d));
}

struct Bounds {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

Bounds BoundsOf (const std::vector<Vec3>& ring)
{
    Bounds b { ring.front ().x, ring.front ().y, ring.front ().x, ring.front ().y };
    for (const auto& p : ring) {
        b.x0 = std::min (b.x0, p.x), b.y0 = std::min (b.y0, p.y);
        b.x1 = std::max (b.x1, p.x), b.y1 = std::max (b.y1, p.y);
    }
    return b;
}

bool Overlap (const Bounds& a, const Bounds& b)
{
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

} // namespace

bool ValidateContours (const Entity& e, std::string& error)
{
    const std::string name = "entity " + std::to_string (e.id);
    if (e.contours.empty ())
        return true;
    if (e.contours.size () < 2) {
        error = name + " has a contour table with a single contour; a simple entity has none";
        return false;
    }
    if (!e.closed) {
        error = name + " has contours but is open";
        return false;
    }
    if (e.ellipse) {
        error = name + " is an ellipse and cannot have contours";
        return false;
    }
    size_t total = 0;
    for (const auto& c : e.contours) {
        if (c.count < 3) {
            error = name + " contour " + std::to_string (c.id) + " has fewer than three vertices";
            return false;
        }
        total += c.count;
    }
    if (total != e.vertices.size ()) {
        error = name + " contour counts do not add up to its vertices";
        return false;
    }
    // The drawn outline of each contour: curves and live corners are what is seen, so they
    // are what must not cross. Bounded work: a compound this large refuses rather than stalls.
    std::vector<std::vector<Vec3>> rings;
    size_t pieces = 0;
    for (size_t c = 0; c < e.contours.size (); ++c) {
        rings.push_back (ShapeRing (e, c));
        if (rings.back ().size () < 3) {
            error = name + " contour " + std::to_string (e.contours[c].id) + " is degenerate";
            return false;
        }
        pieces += rings.back ().size ();
    }
    if (pieces > 20000) {
        error = name + " has more than 20000 drawn boundary pieces across its contours";
        return false;
    }
    std::vector<Bounds> bounds;
    for (const auto& r : rings)
        bounds.push_back (BoundsOf (r));
    size_t work = 0;
    for (size_t a = 0; a < rings.size (); ++a)
        for (size_t b = a + 1; b < rings.size (); ++b) {
            if (!Overlap (bounds[a], bounds[b]))
                continue;
            for (size_t i = 0; i < rings[a].size (); ++i) {
                const Vec3 p = rings[a][i], q = rings[a][(i + 1) % rings[a].size ()];
                const Bounds s { std::min (p.x, q.x), std::min (p.y, q.y), std::max (p.x, q.x), std::max (p.y, q.y) };
                if (!Overlap (s, bounds[b]))
                    continue;
                for (size_t j = 0; j < rings[b].size (); ++j) {
                    if (++work > 4000000) {
                        error = name + " contour crossing check exceeded its work budget";
                        return false;
                    }
                    if (SegmentsMeetXY (p, q, rings[b][j], rings[b][(j + 1) % rings[b].size ()])) {
                        error = name + " contours " + std::to_string (e.contours[a].id) + " and " +
                                std::to_string (e.contours[b].id) + " cross or touch";
                        return false;
                    }
                }
            }
        }
    // No crossings, so one point decides containment for a whole contour.
    for (size_t c = 1; c < rings.size (); ++c)
        if (!InsideRing (rings[0], rings[c].front ())) {
            error = name + " contour " + std::to_string (e.contours[c].id) + " is outside its outer contour";
            return false;
        }
    for (size_t c = 0; c < rings.size (); ++c) {
        size_t depth = 0;
        for (size_t o = 0; o < rings.size (); ++o)
            depth += o != c && InsideRing (rings[o], rings[c].front ()) ? 1 : 0;
        const double area = RingArea (rings[c]);
        const bool hole = depth % 2 == 1;
        if (!(std::abs (area) > kMinSegment * kMinSegment) || (hole ? area > 0.0 : area < 0.0)) {
            error = name + " contour " + std::to_string (e.contours[c].id) +
                    (hole ? " is a hole and must wind clockwise" : " must wind counter-clockwise");
            return false;
        }
    }
    return true;
}

namespace {

// Rings as one entity's flat storage: each re-wound to its depth (even CCW, odd CW), ring 0 first.
bool BuildRings (Entity& e, const std::vector<std::vector<Vec3>>& rings, std::string& error)
{
    std::vector<std::vector<Vec3>> clean;
    for (const auto& ring : rings) {
        std::vector<Vec3> r;
        for (const auto& p : ring)
            if (r.empty () || DistanceXY (r.back (), p) >= kMinSegment)
                r.push_back (p);
        while (r.size () > 1 && DistanceXY (r.front (), r.back ()) < kMinSegment)
            r.pop_back ();
        if (r.size () < 3 || !(std::abs (RingArea (r)) > kMinSegment * kMinSegment)) {
            error = "a contour needs at least three distinct points and an area";
            return false;
        }
        clean.push_back (std::move (r));
    }
    if (clean.empty ()) {
        error = "a profile needs at least one contour";
        return false;
    }
    e.vertices.clear ();
    e.segments.clear ();
    e.contours.clear ();
    e.ellipse.reset ();
    e.closed = true;
    for (size_t c = 0; c < clean.size (); ++c) {
        size_t depth = 0;
        for (size_t o = 0; o < clean.size (); ++o)
            depth += o != c && InsideRing (clean[o], clean[c].front ()) ? 1 : 0;
        if ((depth % 2 == 1) == (RingArea (clean[c]) > 0.0))
            std::reverse (clean[c].begin (), clean[c].end ());
        for (const auto& p : clean[c])
            e.vertices.push_back (Vertex { kNoId, p });
        if (clean.size () > 1)
            e.contours.push_back ({ kNoId, static_cast<uint32_t> (clean[c].size ()) });
    }
    return true;
}

} // namespace

Id AddCompound (Document& doc, const std::vector<std::vector<Vec3>>& rings, Kind kind, double height,
                const std::string& role)
{
    Entity e;
    e.kind = kind;
    e.height = kind == Kind::Extrusion ? height : 0.0;
    e.role = role;
    std::string error;
    if (!BuildRings (e, rings, error))
        return kNoId;
    if (kind == Kind::Extrusion) {
        const double z = e.vertices.front ().p.z;
        for (auto& v : e.vertices)
            v.p.z = z;
    }
    Normalize (doc, e);
    if (!ValidateContours (e, error))
        return kNoId;
    doc.entities.push_back (e);
    ++doc.revision;
    return e.id;
}

bool SetRings (Document& doc, Id entity, const std::vector<std::vector<Vec3>>& rings, std::string& error)
{
    Entity* target = Find (doc, entity);
    if (target == nullptr) {
        error = "no such entity";
        return false;
    }
    Entity e = *target;
    if (!BuildRings (e, rings, error))
        return false;
    if (e.kind == Kind::Extrusion) {
        const double z = target->vertices.empty () ? e.vertices.front ().p.z : target->vertices.front ().p.z;
        for (auto& v : e.vertices)
            v.p.z = z;
    }
    Document scratch;
    scratch.nextId = doc.nextId;
    Normalize (scratch, e);
    if (!ValidateContours (e, error))
        return false;
    doc.nextId = scratch.nextId;
    *target = std::move (e);
    ++doc.revision;
    return true;
}

Id AddPolyline (Document& doc, const std::vector<Vec3>& points, bool closed, const std::string& role)
{
    Entity e;
    e.kind = Kind::Polyline;
    e.closed = closed;
    e.role = role;
    for (const auto& p : points)
        e.vertices.push_back (Vertex { kNoId, p });
    Normalize (doc, e);
    doc.entities.push_back (e);
    ++doc.revision;
    return e.id;
}

Id AddExtrusion (Document& doc, const std::vector<Vec3>& profile, double height, const std::string& role)
{
    Entity e;
    e.kind = Kind::Extrusion;
    e.closed = true;
    e.height = height;
    e.role = role;
    const double z = profile.empty () ? 0.0 : profile.front ().z;
    for (auto p : profile) {
        p.z = z;
        e.vertices.push_back (Vertex { kNoId, p });
    }
    if (SignedAreaXY (e.vertices) < 0.0)
        std::reverse (e.vertices.begin (), e.vertices.end ());
    Normalize (doc, e);
    doc.entities.push_back (e);
    ++doc.revision;
    return e.id;
}

bool Exists (const Document& doc, const Ref& ref)
{
    if (const Path* path = FindPath (doc, ref.entity)) {
        double station = 0.0;
        return PathPart (*path, ref, station);
    }
    const Entity* e = Find (doc, ref.entity);
    if (e == nullptr)
        return false;
    switch (ref.part) {
        case Part::Entity:
            return true;
        case Part::Vertex:
            return VertexIndex (*e, ref.item) >= 0;
        case Part::Segment:
            return SegmentIndex (*e, ref.item) >= 0;
        case Part::Face:
            return ref.item < FaceCount (*e);
        case Part::PathPoint:
        case Part::Break:
        case Part::Range:
        case Part::LandscapeFace:
            return false; // path parts on an entity
    }
    return false;
}

static Vec3 Centroid (const std::vector<Vec3>& points)
{
    Vec3 sum;
    for (const auto& p : points)
        sum = sum + p;
    return points.empty () ? sum : sum * (1.0 / static_cast<double> (points.size ()));
}

bool PointOf (const Document& doc, const Ref& ref, Vec3& point)
{
    if (const Path* path = FindPath (doc, ref.entity)) {
        AlignmentGeometry g;
        std::string error;
        double station = 0.0;
        StationFrame frame;
        if (!PathPart (*path, ref, station) || !BuildAlignment (path->points, g, error) ||
            !Evaluate (*path, g, station, frame))
            return false;
        point = frame.centre;
        for (const auto& c : path->points) // a bend's control point is its corner
            if (ref.part == Part::PathPoint && c.id == ref.item)
                point = { c.p.x, c.p.y, frame.centre.z };
        return true;
    }
    if (!Exists (doc, ref))
        return false;
    const Entity& e = *Find (doc, ref.entity);
    switch (ref.part) {
        case Part::Entity: {
            std::vector<Vec3> points;
            for (const auto& v : e.vertices)
                points.push_back (v.p);
            point = Centroid (points) + Vec3 { 0.0, 0.0, e.height * 0.5 };
            return true;
        }
        case Part::Vertex:
            point = e.vertices[static_cast<size_t> (VertexIndex (e, ref.item))].p;
            return true;
        case Part::Segment: {
            size_t from = 0, to = 0;
            SegmentEnds (e, static_cast<size_t> (SegmentIndex (e, ref.item)), from, to);
            point = Lerp (e.vertices[from].p, e.vertices[to].p, 0.5);
            return true;
        }
        case Part::Face:
            point = Centroid (FacePolygon (e, ref.item));
            return true;
        case Part::PathPoint:
        case Part::Break:
        case Part::Range:
        case Part::LandscapeFace:
            return false;
    }
    return false;
}

bool Validate (const Document& doc, std::string& error)
{
    std::set<Id> seen;
    auto claim = [&] (Id id, const char* what) {
        if (id == kNoId || id >= doc.nextId || !seen.insert (id).second) {
            error = std::string ("invalid or duplicate ") + what + " id " + std::to_string (id);
            return false;
        }
        return true;
    };
    for (const auto& e : doc.entities) {
        if (!claim (e.id, "entity"))
            return false;
        if (e.vertices.size () < MinVertices (e)) {
            error = "entity " + std::to_string (e.id) + " has too few vertices";
            return false;
        }
        if (e.segments.size () != SegmentCount (e)) {
            error = "entity " + std::to_string (e.id) + " segment count does not match its vertices";
            return false;
        }
        for (const auto& c : e.contours)
            if (!claim (c.id, "contour"))
                return false;
        for (const auto& v : e.vertices) {
            if (!claim (v.id, "vertex"))
                return false;
            if (!Finite (v.p)) {
                error = "vertex " + std::to_string (v.id) + " is not finite";
                return false;
            }
        }
        for (size_t i = 0; i < e.segments.size (); ++i) {
            if (!claim (e.segments[i].id, "segment"))
                return false;
            size_t from = 0, to = 0;
            SegmentEnds (e, i, from, to);
            if (Distance (e.vertices[from].p, e.vertices[to].p) < kMinSegment) {
                error = "segment " + std::to_string (e.segments[i].id) + " has zero length";
                return false;
            }
        }
        if (e.kind == Kind::Extrusion) {
            if (!e.closed || !(e.height > 0.0) || !std::isfinite (e.height)) {
                error = "extrusion " + std::to_string (e.id) + " needs a closed profile and a positive height";
                return false;
            }
            for (const auto& v : e.vertices)
                if (std::abs (v.p.z - e.vertices.front ().p.z) > kMinSegment) {
                    error = "extrusion " + std::to_string (e.id) + " profile is not flat";
                    return false;
                }
            if (!(SignedAreaXY (e) > 0.0)) {
                error = "extrusion " + std::to_string (e.id) + " profile is not counter-clockwise";
                return false;
            }
        }
        if (!ValidateContours (e, error))
            return false;
        if (e.ellipse) {
            std::string why;
            if (!ValidateEllipse (e, why)) {
                error = "entity " + std::to_string (e.id) + ": " + why;
                return false;
            }
        }
    }
    for (const auto& p : doc.paths) {
        if (!claim (p.id, "path"))
            return false;
        for (const auto& c : p.points)
            if (!claim (c.id, "control point"))
                return false;
        for (const auto& b : p.profile)
            if (!claim (b.id, "grade break"))
                return false;
        for (const auto& r : p.sections)
            if (!claim (r.id, "cross-section range"))
                return false;
        AlignmentGeometry g;
        std::string why;
        if (!ValidatePath (p, g, why)) {
            error = "path " + std::to_string (p.id) + ": " + why;
            return false;
        }
    }
    return ValidateOrganization (doc, error) && tapioca::geometry::ValidateLandscape (doc, error);
}

} // namespace geomsrv::edit
