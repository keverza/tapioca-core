#ifndef EVP_GEOMETRY_FILLET2D_HPP
#define EVP_GEOMETRY_FILLET2D_HPP

// Pure metre-based line/arc construction after Boolean region assembly. No host,
// editor IDs, meshes or Clipper dependency. Radii are intent, never edited in place.
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace geomsrv::fillet2d {
struct Point {
    double x = 0, y = 0;
    bool operator== (const Point&) const = default;
};
struct Settings {
    double lengthTolerance = 1e-7;
    double angleTolerance = 1e-10;
    double edgeClearance = 1e-4;
    double chordError = 1e-3;
    size_t maxArcSegments = 4096;
    size_t maxIntersectionPairs = 2000000;
    size_t maxFitPasses = 40;
};
struct Request {
    double radius = 0;
    double limit = std::numeric_limits<double>::infinity ();
};
struct InputRing {
    std::vector<Point> points;    // implicit closure, no repeated last vertex
    std::vector<Request> corners; // empty means all sharp; otherwise one per point
};
enum class Kind { Line, Arc };
struct Edge {
    Kind kind = Kind::Line;
    Point from, to, centre;
    double radius = 0, startAngle = 0, sweep = 0;
    // Original input vertices which contributed this edge; source indices survive
    // orientation normalization. Arcs have the same source at both ends.
    size_t source = 0, nextSource = 0;
};
struct Corner {
    size_t source = 0;
    double requestedRadius = 0, radius = 0, maxRadius = 0, tangentDistance = 0;
};
struct Ring {
    bool hole = false;
    std::vector<Edge> edges; // outer CCW, holes CW, material always on the left
    std::vector<Corner> corners;
};
struct Region {
    std::vector<Ring> rings; // same ring identities/order as the input
};

Point At (const Edge& edge, double t);
// Signed arc-length parameterized tangent, in ring traversal order.
Point Tangent (const Edge& edge, double t);
// Atomic: invalid inputs, exhausted safety budgets or an unresolved topology
// conflict return false without replacing `out`. Fitting only decreases radii.
bool Build (const std::vector<InputRing>& input, const Settings& settings, Region& out, std::string& error);
// Sampling is an output operation, not the construction history. Refuses rather
// than violating chord error if its explicit segment budget is insufficient.
bool Tessellate (const Region& region, const Settings& settings, std::vector<std::vector<Point>>& out,
                 std::string& error);
} // namespace geomsrv::fillet2d
#endif
