#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/Transforms.hpp"

#include <iostream>

int main ()
{
    using namespace tapioca::geometry;
    static_assert (sizeof (TapiocaAffineTransform) == 96);
    static_assert (sizeof (TapiocaAffineRequest) == 120);
    static_assert (sizeof (TapiocaAffineOutput) == 24);
    geomsrv::engine::Transform transform;
    transform.m[0][0] = -1;
    transform.m[0][3] = 10;
    const std::vector<double> input { 1, 2, 3 };
    std::vector<double> result { 77 };
    bool flipped = false;
    std::string error;
    if (!TransformVectors ({ transform, VectorTransformMode::Point, input }, result, flipped, error) ||
        result != std::vector<double> { 9, 2, 3 } || !flipped || !error.empty ())
        return 1;
    if (!TransformVectors ({ transform, VectorTransformMode::Direction, input }, result, flipped, error) ||
        result != std::vector<double> { -1, 2, 3 })
        return 2;
    const auto before = result;
    if (TransformVectors ({ transform, static_cast<VectorTransformMode> (99), input }, result, flipped, error) ||
        result != before || !flipped || error.empty ())
        return 3;
    if (input != std::vector<double> { 1, 2, 3 })
        return 4;
    std::cout << "Tapioca affine borrow/publication smoke passed\n";
    return 0;
}
