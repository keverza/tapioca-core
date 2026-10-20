#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"

#include <cstddef>

static_assert (sizeof (TapiocaPlane) == 72);
static_assert (sizeof (TapiocaPlaneFrame) == 120);
static_assert (sizeof (TapiocaPlaneRequest) == 88);
static_assert (sizeof (TapiocaPlaneOutput) == 24);
static_assert (offsetof (TapiocaPlaneRequest, points) == 72);
int main ()
{
    using namespace tapioca::geometry;
    const PlaneValue plane { { 0, 0, 5 }, { 0, 0, 7 }, {} };
    PlaneProjection result;
    std::string error;
    const std::array<double, 6> points { 1, 2, 9, 3, 4, -7 };
    if (!ProjectToPlane (plane, points, result, error) || !error.empty () ||
        result.points != std::vector<double> { 1, 2, 5, 3, 4, 5 } || result.plane.normal != plane.normal ||
        result.u != std::array<double, 3> { 1, 0, 0 })
        return 1;
    if (!ProjectToPlane (result.plane, result.points, result, error) ||
        result.points != std::vector<double> { 1, 2, 5, 3, 4, 5 })
        return 2;
    if (ProjectToPlane (plane, std::array<double, 1> { 1 }, result, error) || error.empty () ||
        result.points.size () != 6)
        return 3;
    if (!ProjectToPlane (plane, {}, result, error) || !result.points.empty () || result.plane.normal != plane.normal)
        return 4;
    return 0;
}
