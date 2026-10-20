#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryEngine.hpp"
#include "Geometry/GeometryRustFfi.h"

#include <array>
#include <cmath>
#include <cstddef>

static_assert (sizeof (TapiocaArcRequest) == 80);
static_assert (offsetof (TapiocaArcRequest, normal) == 24);
static_assert (offsetof (TapiocaArcRequest, radius) == 48);
static_assert (offsetof (TapiocaArcRequest, segments) == 72);

int main ()
{
    const geomsrv::engine::Vector3 centre { 0, 0, 0 }, normal { 0, 0, 1 };
    const tapioca::geometry::ArcRequest request { centre, normal, 2, 0, std::acos (-1.0), 2 };
    std::vector<double> result { 77 };
    std::string error;
    if (!tapioca::geometry::MakeArc (request, result, error) || !error.empty () || result.size () != 9 ||
        std::abs (result[1] - 2) > 1e-14 || std::abs (result[3] + 2) > 1e-14 || std::abs (result[7] + 2) > 1e-14)
        return 1;
    const auto saved = result;
    auto bad = request;
    bad.radius = 0;
    if (tapioca::geometry::MakeArc (bad, result, error) || error.empty () || result != saved)
        return 2;
    const TapiocaArcRequest input { { 0, 0, 0 }, { 0, 0, 1 }, 2, 0, std::acos (-1.0), 2 };
    std::array<double, 12> points;
    points.fill (99);
    if (tapioca_make_arc (&input, points.data (), points.size (), nullptr, 0) != TAPIOCA_OK || points[9] != 99)
        return 3;
    const auto prior = points;
    if (tapioca_make_arc (&input, points.data (), 8, nullptr, 0) != TAPIOCA_ERR_ARG || points != prior)
        return 4;
    return 0;
}
