#include "Geometry/GeometryEngine.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::engine;

namespace {

Polygon Rectangle (double x0, double y0, double x1, double y1, double z = 0.0)
{
    return { { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } } };
}

double Area (const Polygon& polygon)
{
    double twiceArea = 0.0;
    for (size_t i = 0; i < polygon.points.size (); ++i) {
        const Vector3& a = polygon.points[i];
        const Vector3& b = polygon.points[(i + 1) % polygon.points.size ()];
        twiceArea += a.x * b.y - b.x * a.y;
    }
    return std::abs (twiceArea) * 0.5;
}

} // namespace

TEST (GeometryEngine, GlmVectorOperationsUseDoublePrecision)
{
    const Vector3 sum = Add ({ 1.0, 2.0, 3.0 }, { 4.0, -2.0, 1.0 });
    EXPECT_DOUBLE_EQ (5.0, sum.x);
    EXPECT_DOUBLE_EQ (0.0, sum.y);
    EXPECT_DOUBLE_EQ (4.0, sum.z);
    EXPECT_DOUBLE_EQ (0.0, Dot ({ 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }));
    const Vector3 cross = Cross ({ 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 });
    EXPECT_DOUBLE_EQ (1.0, cross.z);

    Vector3 unit;
    std::string error;
    ASSERT_TRUE (Unit ({ 3.0, 4.0, 0.0 }, unit, error)) << error;
    EXPECT_DOUBLE_EQ (0.6, unit.x);
    EXPECT_DOUBLE_EQ (0.8, unit.y);
    EXPECT_FALSE (Unit ({}, unit, error));
    EXPECT_EQ ("cannot normalize a zero or non-finite vector", error);
}

TEST (GeometryEngine, ClipperBooleanReturnsEveryResultRegion)
{
    std::vector<Polygon> result;
    std::string error;
    ASSERT_TRUE (BooleanPolygons (Rectangle (0.0, 0.0, 2.0, 2.0), Rectangle (1.0, 0.0, 3.0, 2.0),
                                  PolygonOperation::Union, result, error))
        << error;
    ASSERT_EQ (1U, result.size ());
    EXPECT_NEAR (6.0, Area (result.front ()), 1.0e-9);

    ASSERT_TRUE (BooleanPolygons (Rectangle (0.0, 0.0, 3.0, 2.0), Rectangle (1.0, -1.0, 2.0, 3.0),
                                  PolygonOperation::Difference, result, error))
        << error;
    ASSERT_EQ (2U, result.size ());
    EXPECT_NEAR (4.0, Area (result[0]) + Area (result[1]), 1.0e-9);
}

TEST (GeometryEngine, ClipperOffsetPreservesElevationAndRejectsNonPlanarInput)
{
    std::vector<Polygon> result;
    std::string error;
    ASSERT_TRUE (OffsetPolygon (Rectangle (0.0, 0.0, 1.0, 1.0, 4.0), 1.0, result, error)) << error;
    ASSERT_EQ (1U, result.size ());
    EXPECT_NEAR (9.0, Area (result.front ()), 1.0e-9);
    for (const Vector3& point : result.front ().points)
        EXPECT_DOUBLE_EQ (4.0, point.z);

    Polygon tilted = Rectangle (0.0, 0.0, 1.0, 1.0);
    tilted.points.back ().z = 0.1;
    EXPECT_FALSE (OffsetPolygon (tilted, 1.0, result, error));
    EXPECT_EQ ("polygon must lie in a plane parallel to world XY", error);
}
