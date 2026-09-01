#ifndef EVP_GEOMETRY_TRANSFORMS_HPP
#define EVP_GEOMETRY_TRANSFORMS_HPP

// Affine transforms: move, rotate, scale, mirror, and composition.
//
// ⚠️ ONE TRANSFORM TYPE FOR ALL FOUR NODES, AND FOR THE ARRAYS. Move, Rotate,
// Scale, Mirror and both Array nodes are the same operation with a different
// matrix, and writing each one against points, polylines, polygons and meshes
// separately would be twenty implementations of one idea - nineteen of which
// would be the ones nobody tested with a mesh.
//
// ⚠️ DIRECTIONS AND POSITIONS TRANSFORM DIFFERENTLY, AND A NORMAL DIFFERENTLY
// AGAIN. A position takes the translation; a direction does not; a normal takes
// the INVERSE TRANSPOSE of the linear part, which only matters once a non-uniform
// scale is in play - at which point a normal transformed as a direction stops
// being perpendicular to its surface and the shading goes visibly wrong on the
// scaled axis. All three are separate calls here so the caller has to choose.
//
// ⚠️ AND A MIRROR FLIPS WINDING. A reflected mesh whose triangles keep their
// original order is inside-out: it looks correct until something culls back
// faces, and then half the solid disappears. `FlipsOrientation` is what a mesh
// transform asks before deciding to reverse its triangles.
//
// Pure: no ACAPI, no GPU, no DevKit. World metres, Z up.

#include "Geometry/GeometryEngine.hpp"

#include <string>

namespace geomsrv::engine {

// A 3x4 affine transform, row-major: three rows of (linear 3x3 | translation).
// Not a 4x4 - the bottom row of an affine transform is always (0,0,0,1), and
// carrying it invites somebody to write a projective transform this geometry
// pipeline cannot honour.
struct Transform {
    double m[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
};

Transform Identity ();
Transform Translation (const Vector3& by);

// Right-handed rotation about `axis` through `origin`. `axis` need not be unit;
// a zero-length axis is refused rather than silently treated as identity, since
// a rotation that quietly does nothing is a graph that looks wired wrong.
bool Rotation (const Vector3& origin, const Vector3& axis, double radians, Transform& result, std::string& error);

// Uniform or per-axis scale about `origin`. A zero factor is refused: it
// collapses geometry to a plane, which is far more often a typo than a request
// and produces degenerate triangles rather than an error.
bool Scaling (const Vector3& origin, const Vector3& factors, Transform& result, std::string& error);

// Reflection in the plane through `origin` with normal `normal`.
bool Mirroring (const Vector3& origin, const Vector3& normal, Transform& result, std::string& error);

// `first` then `second`.
Transform Compose (const Transform& second, const Transform& first);

Vector3 ApplyToPoint (const Transform& transform, const Vector3& point);
Vector3 ApplyToDirection (const Transform& transform, const Vector3& direction);

// The inverse-transpose path. See the header note: only a non-uniform scale or a
// mirror makes this differ from ApplyToDirection, and those are exactly the
// cases where using the wrong one is a shading fault nobody traces back here.
// Returns the direction unchanged when the linear part is singular - which
// cannot happen through the builders above, all of which refuse degeneracy.
Vector3 ApplyToNormal (const Transform& transform, const Vector3& normal);

// Whether this transform turns a right-handed basis into a left-handed one, i.e.
// whether a mesh transformed by it must have its triangle winding reversed.
bool FlipsOrientation (const Transform& transform);

} // namespace geomsrv::engine

#endif
