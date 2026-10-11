#ifndef GEOMETRYSERVER_SURFACESOURCEBOUNDARY_HPP
#define GEOMETRYSERVER_SURFACESOURCEBOUNDARY_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Pure, SDK-free boundary matcher used by SurfaceSourceMap. It turns a set of
// planar pieces (triangles for a face, contours for a query) into canonical
// closed loops on a tolerance grid and validates the loop set. No ACAPI, no
// ModelerAPI, no clipping engine -- the failure that matters (a confident wrong
// face) is asserted offline.
namespace geomsrv {
namespace surfacemap {
namespace boundary {

// A 3D orthonormal frame in the plane: `u` and `v` are perpendicular to
// `normal`, so a point's (u,v) coordinates are its in-plane position.
struct Frame {
    double origin[3] = {};
    double normal[3] = {};
    double u[3] = {};
    double v[3] = {};
};

// A canonical closed loop in quantized grid coordinates.
using Loop = std::vector<std::pair<int64_t, int64_t>>;

// Build an orthonormal frame from a plane normal and a point on that plane.
bool MakeFrame (const double normal[3], const double origin[3], Frame& frame, std::string& reason);

// Canonical loops of `pieces` (each a flat xyz contour) projected into `frame`
// and snapped to `vertexTolerance`.
//
// Refuses, rather than guessing: a shared interior edge not traversed in
// opposite directions (duplicated/overlapping pieces), an edge shared by more
// than two pieces, an open or crossing boundary chain, a self-intersecting loop,
// a backtracking spike, a zero-area loop, and any loop set that is not exactly
// one outer contour with oppositely wound, strictly enclosed holes. Loops are
// returned sorted so two loop sets compare with `==`. Both outputs are cleared
// on entry; callers must use loops only when the function returns true.
bool BuildCanonicalLoops (const std::vector<std::vector<double>>& pieces, const Frame& frame, double vertexTolerance,
                          std::vector<Loop>& loops, std::string& reason);

} // namespace boundary
} // namespace surfacemap
} // namespace geomsrv

#endif
