#ifndef EVP_EDIT_EDITPATH_HPP
#define EVP_EDIT_EDITPATH_HPP

// A path is a definition, not a polyline: a horizontal alignment (plan control points
// with corner radii), a vertical profile (grade breaks along it) and cross-section
// ranges (width, crossfall). The three are independent, so moving the plan line never
// rewrites a height and changing a grade never moves the plan line. Every position
// along a path is a station: metres of plan length from its start.
#include "Edit/EditTypes.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

// ---- horizontal alignment ---------------------------------------------------------

// A plan control point. Its corner is rounded by a circular arc of `radius` that is
// tangent to both legs; 0 keeps a sharp corner. Z is ignored: heights are the profile's.
struct ControlPoint {
    Id id = kNoId;
    Vec3 p;
    double radius = 0.0;
    bool lockXY = false;
    bool operator== (const ControlPoint&) const = default;
};

enum class ElementKind : uint8_t { Line, Arc };
struct AlignmentElement {
    ElementKind kind = ElementKind::Line;
    double start = 0.0, length = 0.0;                   // stations
    Vec3 a, b;                                          // start and end points
    Vec3 centre;                                        // arcs
    double radius = 0.0, startAngle = 0.0, sweep = 0.0; // sweep > 0 turns left
};
struct AlignmentGeometry {
    std::vector<AlignmentElement> elements;
    double length = 0.0;
    // Station of each control point: 0 and the length at the ends, the middle of its
    // arc (or the corner itself) between.
    std::vector<double> pointStations;
};
// Refuses fewer than two points, coincident points, a path that doubles back, and
// radii whose tangents do not fit on their legs.
bool BuildAlignment (const std::vector<ControlPoint>& points, AlignmentGeometry& out, std::string& error);

struct Placement {
    Vec3 point;             // plan position, z = 0
    Vec3 tangent;           // unit, plan
    Vec3 left;              // unit, plan, tangent turned a quarter left
    double curvature = 0.0; // 1 / radius, positive turning left
};
// False outside [0, length].
bool Locate (const AlignmentGeometry& geometry, double station, Placement& out);
// The station nearest a plan point and its signed offset (positive to the left).
bool StationOf (const AlignmentGeometry& geometry, Vec3 point, double& station, double& offset);
// Stations where an element starts or ends, ascending.
std::vector<double> ElementStations (const AlignmentGeometry& geometry);

} // namespace geomsrv::edit
#endif
