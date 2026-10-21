#ifndef EVP_EDIT_EDITDOCUMENT_HPP
#define EVP_EDIT_EDITDOCUMENT_HPP

// The virtual editor's model: typed entities with ids that stay stable across edits,
// so a committed element can be found again and a re-opened definition edited in
// place. Pure C++ (no Archicad, ImGui or Win32) so every tool built on it runs in the
// offline suite. Units are metres, coordinates are world, Z is absolute.
#include "Edit/EditPath.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditTypes.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::edit {

enum class Kind : uint8_t { Polyline, Extrusion };
// A path's own parts: its plan control points, its grade breaks and its
// cross-section ranges. `Entity` on a path's id means the whole path.
enum class Part : uint8_t { Entity, Vertex, Segment, Face, PathPoint, Break, Range, LandscapeFace };

struct Vertex {
    Id id = kNoId;
    Vec3 p;
    bool lockXY = false, lockZ = false;
    // A live corner: rounded by an arc of this radius tangent to both legs, and kept as
    // the radius -- the vertex stays the corner's anchor (EditShape.hpp). 0 is sharp.
    double radius = 0.0;
    // Bezier handles, as plan offsets from `p` (zero: none): `out` shapes the segment that
    // leaves this vertex, `in` the one that arrives. A segment with either of its facing
    // handles set is a cubic. A smooth vertex keeps its two handles in line.
    Vec3 in, out;
    bool smooth = false;
    bool operator== (const Vertex&) const = default;
};
// Segment i joins vertex i to vertex i + 1 (wrapping on a closed entity). Its id
// survives edits that keep the segment, so attributes such as width follow it.
struct Segment {
    Id id = kNoId;
    double width = 0.0;
    uint32_t tag = 0;
    bool operator== (const Segment&) const = default;
};
// An Extrusion is a closed, horizontal profile lifted by `height`. Its faces are
// 0 bottom, 1 top and 2 + i the side over segment i.
struct Entity {
    Id id = kNoId;
    Kind kind = Kind::Polyline;
    bool closed = false;
    std::vector<Vertex> vertices;
    std::vector<Segment> segments;
    double height = 0.0;
    std::string role; // the owning tool's word for it ("centerline", "platform", ...)
    bool operator== (const Entity&) const = default;
};
struct Document {
    std::vector<Entity> entities;
    std::vector<Path> paths; // ids share one space with the entities
    std::optional<tapioca::geometry::LandscapeSession> landscape;
    Id nextId = 1; // never reused, so a deleted id cannot alias a later one
    uint64_t revision = 0;
};

struct Ref {
    Id entity = kNoId;
    Part part = Part::Entity;
    Id item = kNoId;       // the part's id, or a face index; unused for Part::Entity
    uint64_t revision = 0; // LandscapeFace only: a call-local locator, never authored identity.
};
bool operator== (const Ref& a, const Ref& b);
bool operator!= (const Ref& a, const Ref& b);

constexpr double kMinSegment = 1.0e-6; // shorter is a duplicate vertex, not a segment

Id NewId (Document& doc);
Entity* Find (Document& doc, Id entity);
const Entity* Find (const Document& doc, Id entity);
Path* FindPath (Document& doc, Id path);
const Path* FindPath (const Document& doc, Id path);
int VertexIndex (const Entity& entity, Id vertex);
int SegmentIndex (const Entity& entity, Id segment);
size_t SegmentCount (const Entity& entity);
size_t MinVertices (const Entity& entity);
// Vertex indices at the start and end of segment `index`.
void SegmentEnds (const Entity& entity, size_t index, size_t& from, size_t& to);
size_t FaceCount (const Entity& entity);
// Corner points of a face, counter-clockwise seen from outside.
std::vector<Vec3> FacePolygon (const Entity& entity, size_t face);
// Outward unit normal of a face (Z for top, -Z for bottom, horizontal for sides).
bool FaceNormal (const Entity& entity, size_t face, Vec3& normal);

Id AddPolyline (Document& doc, const std::vector<Vec3>& points, bool closed, const std::string& role = {});
// The profile is made counter-clockwise and flattened to its first point's Z.
Id AddExtrusion (Document& doc, const std::vector<Vec3>& profile, double height, const std::string& role = {});
// Gives new ids to vertices or segments that lack one and resizes `segments` to the
// vertex count. Segments that already exist keep their ids and attributes.
void Normalize (Document& doc, Entity& entity);

bool Exists (const Document& doc, const Ref& ref);
// The point a reference stands for: a vertex, a segment midpoint, a face or entity
// centroid; on a path, the point on its profile at the part's station (a whole path
// stands for its middle).
bool PointOf (const Document& doc, const Ref& ref, Vec3& point);
// Unique ids, matching segment counts, finite coordinates, enough vertices, no
// zero-length segment, flat profiles and positive heights for extrusions, and paths
// whose alignment, profile and ranges agree.
bool Validate (const Document& doc, std::string& error);
double SignedAreaXY (const std::vector<Vertex>& vertices);

} // namespace geomsrv::edit
#endif
