#include "Edit/EditDocument.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace geomsrv::edit {

bool operator== (const Ref& a, const Ref& b)
{
    return a.entity == b.entity && a.part == b.part && a.item == b.item;
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
    return entity.closed ? 3 : 2;
}

void SegmentEnds (const Entity& entity, size_t index, size_t& from, size_t& to)
{
    from = index;
    to = (index + 1) % entity.vertices.size ();
}

size_t FaceCount (const Entity& entity)
{
    return entity.kind == Kind::Extrusion ? 2 + entity.vertices.size () : 0;
}

std::vector<Vec3> FacePolygon (const Entity& entity, size_t face)
{
    std::vector<Vec3> out;
    if (entity.kind != Kind::Extrusion || face >= FaceCount (entity))
        return out;
    const Vec3 up { 0.0, 0.0, entity.height };
    const size_t n = entity.vertices.size ();
    if (face == 0) {
        for (size_t i = n; i-- > 0;)
            out.push_back (entity.vertices[i].p);
    }
    else if (face == 1) {
        for (const auto& v : entity.vertices)
            out.push_back (v.p + up);
    }
    else {
        const size_t i = face - 2;
        const Vec3 a = entity.vertices[i].p, b = entity.vertices[(i + 1) % n].p;
        out = { a, b, b + up, a + up };
    }
    return out;
}

bool FaceNormal (const Entity& entity, size_t face, Vec3& normal)
{
    if (entity.kind != Kind::Extrusion || face >= FaceCount (entity))
        return false;
    if (face < 2) {
        normal = { 0.0, 0.0, face == 0 ? -1.0 : 1.0 };
        return true;
    }
    const size_t n = entity.vertices.size ();
    const Vec3 a = entity.vertices[face - 2].p, b = entity.vertices[(face - 1) % n].p;
    // Profiles are counter-clockwise, so the outside is to the right of each edge.
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
            if (!(SignedAreaXY (e.vertices) > 0.0)) {
                error = "extrusion " + std::to_string (e.id) + " profile is not counter-clockwise";
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
    return tapioca::geometry::ValidateLandscape (doc, error);
}

} // namespace geomsrv::edit
