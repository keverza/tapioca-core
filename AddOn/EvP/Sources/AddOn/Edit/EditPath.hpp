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

// ---- vertical profile -------------------------------------------------------------

// A grade break: an elevation at a station where the grade changes, smoothed by a
// symmetric parabolic vertical curve `curve` metres long centred on it. A fixed break
// is a reference elevation (an entrance threshold, a street level) that edits keep.
// An anchored break follows the station of a control point as the plan changes;
// `binding` names the host item a fixed elevation came from, for re-checking later.
struct GradeBreak {
    Id id = kNoId;
    double station = 0.0, z = 0.0, curve = 0.0;
    bool fixed = false;
    Id anchor = kNoId;
    std::string binding;
    bool operator== (const GradeBreak&) const = default;
};
// Breaks ascend from station 0 to `length`, the end breaks carry no curve, and
// neighbouring curves do not overlap.
bool ValidateProfile (const std::vector<GradeBreak>& breaks, double length, std::string& error);
// Elevation and grade (a fraction: 0.05 is 5 %) at a station, curves included.
bool Elevation (const std::vector<GradeBreak>& breaks, double station, double& z, double* grade = nullptr);
// Grade of the tangent from break i to break i + 1, as a fraction.
double TangentGrade (const std::vector<GradeBreak>& breaks, size_t i);

bool SetBreakElevation (std::vector<GradeBreak>& breaks, Id id, double z, std::string& error);
// Sets the tangent from break i to i + 1 to `percent`. The fixed end stays put; with
// neither fixed the start stays, as the user's first click does.
bool SetGrade (std::vector<GradeBreak>& breaks, size_t i, double percent, std::string& error);
// A new break on the existing tangent, so the profile's shape does not change.
bool InsertBreak (std::vector<GradeBreak>& breaks, double station, Id id, std::string& error);
bool DeleteBreak (std::vector<GradeBreak>& breaks, Id id, std::string& error);
bool MoveBreak (std::vector<GradeBreak>& breaks, Id id, double station, std::string& error);
bool SetCurve (std::vector<GradeBreak>& breaks, Id id, double length, std::string& error);
// The curve length a minimum K value asks for: K metres per percent of grade change.
double CurveForK (const std::vector<GradeBreak>& breaks, size_t i, double k);
// After the plan changes length: the end break moves to the new end, anchored breaks
// go to their anchors' stations, and the rest keep their share of the length.
bool Restation (std::vector<GradeBreak>& breaks, double oldLength, double newLength,
                const std::vector<std::pair<Id, double>>& anchorStations, std::string& error);
// Breaks that follow sampled heights within `tolerance` metres (Douglas-Peucker),
// the starting profile for a path laid on terrain. Ids are handed out from `nextId`.
std::vector<GradeBreak> ProfileFromSamples (const std::vector<std::pair<double, double>>& samples, double tolerance,
                                            Id& nextId);

} // namespace geomsrv::edit
#endif
