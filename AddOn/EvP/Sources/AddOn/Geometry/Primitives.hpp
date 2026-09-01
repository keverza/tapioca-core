#ifndef EVP_GEOMETRY_PRIMITIVES_HPP
#define EVP_GEOMETRY_PRIMITIVES_HPP

// Solid primitives, as meshes.
//
// The first thing a node graph needs after a number is a SHAPE. Points and
// polylines say where and how far; nothing in the catalog said "this much
// space", and a graph whose only visible output is a wire diagram cannot be
// judged against a building.
//
// ⚠️ EVERY VERTEX CARRIES ITS OWN NORMAL, WHICH MEANS A BOX HAS 24 OF THEM AND
// NOT 8. The renderer flat-shades from the normal it is given; eight shared
// corner vertices would average three face normals into a diagonal, and the box
// would render as a rounded lump with no edges. That is a PICTURE, not an error,
// and the natural reading of it is "my geometry is wrong" rather than "the
// vertices were welded". The same rule is why the sphere's seam column is
// duplicated rather than wrapped.
//
// ⚠️ AND THE WINDING IS COUNTER-CLOCKWISE SEEN FROM OUTSIDE, consistently. A
// mesh with mixed winding looks correct until something culls back faces, at
// which point half the solid disappears - again as a picture.
//
// Pure: no ACAPI, no GPU, no DevKit. Coordinates are world metres, Z up, matching
// every other mesh in this process.

#include "Geometry/GeometryEngine.hpp"
#include "Geometry/Mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::engine {

// The size limits a node applies before building. Present so that "a sphere with
// 100000 segments" is a REFUSED input with a reason rather than a multi-gigabyte
// allocation inside Archicad.
constexpr int kMinSphereSegments = 3;
constexpr int kMaxSphereSegments = 256;

// An axis-aligned box centred on `centre`.
//
// Centred rather than corner-anchored because the other primitives are: a sphere
// has no corner, and a catalog where "the point you give it" means the middle of
// one shape and the corner of another is a catalog people get wrong once each.
// A negative or zero extent is refused rather than silently made positive - a
// box of zero height is far more often a wiring mistake than a request.
bool MakeBox (const Vector3& centre, double width, double depth, double height, Mesh& mesh, std::string& error);

// A UV sphere: `segments` around the equator, half that many bands from pole to
// pole.
//
// ⚠️ ONE SEGMENT COUNT, NOT TWO. Rings and bands are not independently useful at
// this level - what a user wants is "smoother" - and two numbers is two chances
// to make an ellipsoid-looking artefact out of a sphere. Whoever needs the
// distinction can add a second parameter without changing what this one means.
bool MakeSphere (const Vector3& centre, double radius, int segments, Mesh& mesh, std::string& error);

// A planar polygon swept along a vector, capped at both ends.
//
// ⚠️ TRIANGULATED IN THE POLYGON'S OWN PLANE, NOT IN XY. A fan from the first
// vertex is only correct for a convex outline, and the outlines this catalog will
// meet are floor plates, cores and L-shaped footprints - concave by default, and
// a fan over one of those produces triangles OUTSIDE the shape. So the caps go
// through Clipper2's triangulator, and the polygon is projected onto its own
// best-fit plane first so that a vertical profile works as well as a horizontal
// one. A wall section is exactly as common as a slab here.
//
// ⚠️ THE CAPS FACE OPPOSITE WAYS. Both are the same triangulation; the near cap
// is reversed. A solid whose two ends wind the same way is inside-out at one end,
// which is invisible until something culls back faces.
bool MakeExtrusion (const std::vector<Vector3>& outline, const Vector3& direction, Mesh& mesh, std::string& error);

// A surface between two open or closed curves, quad by quad.
//
// ⚠️ THE TWO CURVES MUST HAVE THE SAME POINT COUNT, and that is REFUSED rather
// than resampled. Resampling silently is the wrong favour: which curve to
// resample onto changes the shape, so a loft that quietly picked one would give
// an answer nobody asked for and no way to see why. Divide Curve is the node that
// makes two curves match, and it is one wire.
bool MakeLoft (const std::vector<Vector3>& from, const std::vector<Vector3>& to, bool closed, Mesh& mesh,
               std::string& error);

} // namespace geomsrv::engine

#endif
