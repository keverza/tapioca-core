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
enum class EllipseMode : uint8_t { Circle, Ellipse };
// An analytic ellipse in its plane: centre + axisU*cos(t) + axisV*sin(t).
// The axes may be non-orthogonal after an affine transform; this is still an exact
// ellipse (the axes are conjugate diameters). `mode` is authored identity, not inferred
// from the current radii, so a flattened circle remains an ellipse if later made round.
struct EllipseDefinition {
    EllipseMode mode = EllipseMode::Ellipse;
    Vec3 center, axisU, axisV;
    bool operator== (const EllipseDefinition&) const = default;
};
// One closed ring of a compound entity: the next `count` vertices of the flat vertex list,
// after the rings before it. Its id is stable across edits that keep the ring, so a hole can
// be named again after a vertex in it moves; it is never a vertex or segment id.
struct EntityContour {
    Id id = kNoId;
    uint32_t count = 0;
    bool operator== (const EntityContour&) const = default;
};
// An Extrusion is a closed, horizontal profile lifted by `height`. Its faces are
// 0 bottom, 1 top and 2 + i the side over segment i.
//
// A compound entity is closed and carries a contour table (`contours`, two or more rows) that
// splits `vertices` and `segments` -- which keep their flat, id-stable storage -- into rings.
// Contour 0 is the outer boundary; every other contour lies strictly inside it, and no two
// contours touch or cross. Fill is even-odd: a point is solid when it is inside an odd number of
// contours, so a contour's depth (how many others contain it) makes it a hole when odd and an
// island when even. Windings follow depth -- even counter-clockwise, odd clockwise -- so the
// solid is always on the left of every segment and the net shoelace area is the solid area.
// Segment i joins vertex i to the next vertex of the same contour, wrapping within it. An
// empty table is one contour of every vertex: every entity written before contours existed.
struct Entity {
    Id id = kNoId;
    Kind kind = Kind::Polyline;
    bool closed = false;
    std::vector<Vertex> vertices;
    std::vector<Segment> segments;
    double height = 0.0;
    std::string role; // the owning tool's word for it ("centerline", "platform", ...)
    Membership member;
    std::optional<EllipseDefinition> ellipse; // authoritative definition; vertices are its Bezier quarters
    std::vector<EntityContour> contours;      // empty: a single contour (the legacy, simple entity)
    bool operator== (const Entity&) const = default;
};
// The editor's own layers and groups, never Archicad's: nothing here creates an Archicad
// layer. A layer hides or locks everything on it and colours its lines; `rgba` 0 draws in the
// layer style's colour. A group may sit in another (`parent`), never in itself.
struct Layer {
    Id id = kNoId;
    std::string name;
    bool visible = true, locked = false;
    uint32_t rgba = 0;
    bool operator== (const Layer&) const = default;
};
struct Group {
    Id id = kNoId;
    std::string name;
    Id parent = kNoId;
    bool operator== (const Group&) const = default;
};
struct Document {
    std::vector<Entity> entities;
    std::vector<Path> paths; // ids share one space with the entities
    std::optional<tapioca::geometry::LandscapeSession> landscape;
    // Layer 0: always there, with no id, so every object is on a layer. It cannot be deleted
    // or renamed; what leaves another layer lands on it.
    Layer layer0 { kNoId, "0" };
    std::vector<Layer> layers;
    std::vector<Group> groups;
    std::vector<Id> isolated; // the objects Isolate hid, which Show isolated brings back
    Id nextId = 1;            // never reused, so a deleted id cannot alias a later one
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

// ---- contours ----------------------------------------------------------------------------
// Every caller that walks from a vertex to its neighbour goes through these, so a compound
// entity's rings wrap within themselves and a simple entity behaves exactly as before.

// A compound entity: two or more contours.
bool IsCompound (const Entity& entity);
// 1 for a simple entity (0 when it has no vertices).
size_t ContourCount (const Entity& entity);
// Vertex (and segment) indices [begin, end) of contour `contour`.
void ContourRange (const Entity& entity, size_t contour, size_t& begin, size_t& end);
// The contour holding vertex `index`.
size_t ContourOf (const Entity& entity, size_t index);
// The contour with this id, or -1. A simple entity has none.
int ContourIndex (const Entity& entity, Id contour);
// The next and previous vertex in the same contour, wrapping on a closed entity. On an open
// one the ends have no neighbour past them; the caller checks that, as before.
size_t NextVertex (const Entity& entity, size_t index);
size_t PrevVertex (const Entity& entity, size_t index);
// Vertex `index` is the first or last of an open entity: it has only one neighbour.
bool OpenEnd (const Entity& entity, size_t index);
// How many other contours contain contour `contour` (0 outer, 1 hole, 2 island in a hole...).
size_t ContourDepth (const Entity& entity, size_t contour);
// The plan points of one contour's anchors, in storage order.
std::vector<Vec3> ContourPoints (const Entity& entity, size_t contour);
// The net plan area of a closed entity: every contour's signed shoelace area summed, so holes
// subtract. Positive for a valid extrusion or compound profile.
double SignedAreaXY (const Entity& entity);
// Even-odd: inside an odd number of contours (anchors joined straight).
bool InsideXY (const Entity& entity, Vec3 point);
// A compound entity built from rings: the first is the outer boundary, the rest lie inside it.
// Each ring is re-wound to its depth's direction. Ids are new; heights are kept as given.
Id AddCompound (Document& doc, const std::vector<std::vector<Vec3>>& rings, Kind kind, double height,
                const std::string& role = {});
// Replaces an entity's geometry by `rings` (same contract as AddCompound), keeping the entity's
// id, kind, role, membership and height, and giving every vertex, segment and contour a new id.
// One ring makes it simple. Refuses (unchanged) when the rings do not form a valid profile.
bool SetRings (Document& doc, Id entity, const std::vector<std::vector<Vec3>>& rings, std::string& error);
// Checks the contour table of one entity: counts, ids, closed, at least three anchors each,
// contours 1.. strictly inside contour 0, no two crossing or touching, windings by depth.
bool ValidateContours (const Entity& entity, std::string& error);

size_t FaceCount (const Entity& entity);
// Corner points of a face, counter-clockwise seen from outside. A compound cap is its outer
// contour only; FaceRings gives every ring.
std::vector<Vec3> FacePolygon (const Entity& entity, size_t face);
// Every ring of a face: one for a side or a simple cap, one per contour for a compound cap
// (the first the outer), each oriented as the face is seen from outside. Even-odd fill.
std::vector<std::vector<Vec3>> FaceRings (const Entity& entity, size_t face);
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
// One ring's shoelace area. For a whole entity use SignedAreaXY (const Entity&).
double SignedAreaXY (const std::vector<Vertex>& vertices);

} // namespace geomsrv::edit
#endif
