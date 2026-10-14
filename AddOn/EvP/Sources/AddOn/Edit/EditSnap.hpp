#ifndef EVP_EDIT_EDITSNAP_HPP
#define EVP_EDIT_EDITSNAP_HPP

// Snapping for every editor tool. It runs inside the move: the tool asks for a snap
// with the raw cursor and gets back the point, what it snapped to, the transient
// guide lines to draw and a hint for the cursor, so drawing the feedback never
// needs a second pass. Host geometry arrives as a context snapshot taken by the
// host; nothing here reads the model.
#include "Edit/EditDocument.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::edit {

enum SnapMode : uint32_t {
    kSnapEndpoint = 1u << 0,
    kSnapMidpoint = 1u << 1,
    kSnapIntersection = 1u << 2,
    kSnapCentre = 1u << 3,
    kSnapPerpendicular = 1u << 4,
    kSnapNearest = 1u << 5,
    kSnapAxis = 1u << 6,
    kSnapExtension = 1u << 7,
    kSnapGrid = 1u << 8,
    kSnapTerrain = 1u << 9,
    kSnapAll = (1u << 10) - 1,
};

// Ordered by priority: within the tolerance, an earlier kind always wins over a
// later one, and the nearest candidate wins within a kind.
enum class SnapKind : uint8_t {
    None,
    Endpoint,
    Midpoint,
    Intersection,
    Centre,
    Perpendicular,
    Nearest,
    Axis,
    Extension,
    Grid,
    Terrain, // nothing else in reach: the cursor dropped onto the terrain
};
const char* SnapKindName (SnapKind kind);

struct Plane {
    Vec3 origin;
    Vec3 normal { 0.0, 0.0, 1.0 };
};
// In-plane unit axes: world X and Y for a horizontal plane.
void PlaneAxes (const Plane& plane, Vec3& u, Vec3& v);
Vec3 ProjectToPlane (const Plane& plane, Vec3 p);

struct Guide {
    Vec3 from, to;
    SnapKind kind = SnapKind::None;
};

// What the host knows about the model around the edit, in world metres.
struct SnapContext {
    struct Point {
        Vec3 p;
        std::string source;
    };
    struct Segment {
        Vec3 a, b;
        std::string source;
    };
    std::vector<Point> points;
    std::vector<Segment> segments;
    // Terrain height at a plan position; false where there is no terrain.
    std::function<bool (double x, double y, double& z)> height;
};

struct SnapQuery {
    Vec3 cursor;                // on the working plane
    double tolerance = 0.1;     // metres: the host converts its pixel radius
    std::optional<Vec3> anchor; // the gesture's start, for perpendicular and axis guides
    uint32_t modes = kSnapAll;
    Plane plane;                 // distances are measured in this plane's projection
    bool keepPlane = true;       // project the snapped point back onto the plane
    double grid = 0.0;           // spacing in metres; 0 disables the grid
    double extensionReach = 0.0; // 0 means 25 tolerances
    double pathStep = 1.0;       // metres between path samples snapped along
    // Moving items must not snap to themselves: a vertex ref also drops the
    // segments that touch it, an entity ref drops the whole entity.
    std::vector<Ref> exclude;
};

struct SnapResult {
    bool snapped = false;
    Vec3 point;
    SnapKind kind = SnapKind::None;
    Ref source;                // the document item, when the target is the editor's own
    std::string contextSource; // the host item, when it is not
    std::vector<Guide> guides;
    std::string hint;
};

SnapResult Snap (const Document& doc, const SnapContext& context, const SnapQuery& query);

} // namespace geomsrv::edit
#endif
