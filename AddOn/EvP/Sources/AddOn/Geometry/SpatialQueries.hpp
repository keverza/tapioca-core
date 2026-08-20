#ifndef GEOMETRYSERVER_SPATIALQUERIES_HPP
#define GEOMETRYSERVER_SPATIALQUERIES_HPP

#include "Mesh.hpp"
#include <vector>
#include <string>

// Broadphase spatial queries over a snapshot's per-element AABBs. Return the
// GUIDs of candidate elements. Pure C++ — run on HTTP worker threads.
// (Linear over element boxes for now; a BVH can slot in behind this API later.)
namespace geomsrv {

std::vector<std::string> QueryBox    (const Snapshot& snap, const double mn[3], const double mx[3]);
std::vector<std::string> QuerySphere (const Snapshot& snap, const double c[3], double radius);

// polyXY = [x0,y0, x1,y1, ...] (>= 3 points); tested as a prism extruded in Z
// over [zmin, zmax]. An element matches if its AABB's Z overlaps the range and
// its XY footprint rectangle intersects the polygon.
std::vector<std::string> QueryPolygon (const Snapshot& snap, const std::vector<double>& polyXY,
                                       double zmin, double zmax);

} // namespace geomsrv

#endif
