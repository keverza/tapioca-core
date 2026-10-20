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
    const std::vector<geomsrv::engine::Transform> chain { transform, transform };
    geomsrv::engine::Transform composed;
    if (!ComposeTransforms (chain, composed, error) || composed.m[0][0] != 1 || composed.m[0][3] != 0 ||
        !error.empty ())
        return 5;
    static_assert (sizeof (TapiocaTransformBuilderRequest) == 64);
    static_assert (offsetof (TapiocaTransformBuilderRequest, origin) == 8);
    static_assert (offsetof (TapiocaTransformBuilderRequest, vector) == 32);
    static_assert (offsetof (TapiocaTransformBuilderRequest, radians) == 56);
    const geomsrv::engine::Vector3 origin { 10, 20, 30 }, vector { -2, 3, 0.5 };
    if (!BuildTransform ({ TransformBuilderKind::Scaling, origin, vector }, composed, error) ||
        composed.m[0][0] != -2 || composed.m[0][3] != 30 || !error.empty ())
        return 6;
    const auto saved = composed;
    if (BuildTransform ({ static_cast<TransformBuilderKind> (99), origin, vector }, composed, error) ||
        composed.m[0][3] != saved.m[0][3] || error.empty ())
        return 7;
    std::cout << "Tapioca affine borrow/publication smoke passed\n";
    return 0;
}
