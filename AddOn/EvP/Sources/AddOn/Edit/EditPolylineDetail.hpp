#ifndef EVP_EDIT_EDITPOLYLINE_DETAIL_HPP
#define EVP_EDIT_EDITPOLYLINE_DETAIL_HPP

#include "Edit/EditDocument.hpp"
#include <utility>
#include <vector>

namespace geomsrv::edit::polyline_detail {
// Shared straight-chord plan intersection and edge enumeration for polyline edits.
// Curve-bearing callers must either refine the curve first or refuse explicitly.
bool CrossPlan (Vec3 a, Vec3 b, Vec3 c, Vec3 d, double& t, double& s);
std::vector<std::pair<Vec3, Vec3>> EdgesOf (const Entity& target);
} // namespace geomsrv::edit::polyline_detail

#endif
