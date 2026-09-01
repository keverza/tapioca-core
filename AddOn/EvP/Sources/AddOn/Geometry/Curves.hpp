#ifndef EVP_GEOMETRY_CURVES_HPP
#define EVP_GEOMETRY_CURVES_HPP

// Curves, as polylines.
//
// ⚠️ THERE IS NO CURVE TYPE, AND THAT IS THE MVP DECISION THIS FILE RECORDS. An
// arc is tessellated into a polyline at the moment it is made, so everything
// downstream - divide, offset, extrude, transform, preview - sees one kind of
// thing and needs no special case. The cost is that the arc's exactness is lost
// at creation and its segment count becomes a modelling decision rather than a
// display one, which is the honest trade at this stage: a real curve type has to
// carry through the serializer, the bridge schema and every consumer, and half
// of one would be worse than none.
//
// ⚠️ SAMPLING IS BY ARC LENGTH, NOT BY VERTEX INDEX. "The midpoint of this
// curve" has to mean the same place whether the polyline was drawn with evenly
// spaced points or not; indexing gives an answer that drifts towards wherever
// the points happen to be dense. Divide and PointAt both walk the accumulated
// length for that reason.
//
// Pure: no ACAPI, no GPU, no DevKit. World metres, Z up.

#include "Geometry/GeometryEngine.hpp"

#include <string>
#include <vector>

namespace geomsrv::engine {

constexpr int kMinArcSegments = 2;
constexpr int kMaxArcSegments = 4096;

// An arc in the plane through `centre` with normal `normal`, swept from
// `startRadians` by `sweepRadians`. A full circle is a sweep of 2*pi, and the
// closing point is emitted, so the result is a closed polyline by position.
bool MakeArc (const Vector3& centre, const Vector3& normal, double radius, double startRadians, double sweepRadians,
              int segments, std::vector<Vector3>& points, std::string& error);

// The polyline's total length, and the cumulative length at each vertex. Both
// come from one walk so they cannot disagree.
double PolylineLength (const std::vector<Vector3>& points, std::vector<double>& cumulative);

// The point at `t` along the curve by ARC LENGTH, t in 0..1, clamped. `tangent`
// is the unit direction there; it is the segment's direction, not a smoothed
// one, because the polyline IS the curve here - see the header.
bool PointOnPolyline (const std::vector<Vector3>& points, double t, Vector3& point, Vector3& tangent,
                      std::string& error);

// `count` points along the curve. Includes both ends when `includeEnds`, which
// is what "divide into N segments" means - N segments is N+1 points, and getting
// that off by one puts a facade panel half a bay out.
bool DividePolyline (const std::vector<Vector3>& points, int count, bool includeEnds, std::vector<Vector3>& result,
                     std::string& error);

} // namespace geomsrv::engine

#endif
