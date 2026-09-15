#include "Annotation/DimensionGeometry.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace {

namespace annotation = geomsrv::annotation;

TEST (DimensionGeometry, ResolvesExplicitVerticalPlaneWitnessGapAndOverhang)
{
    annotation::DimensionStyle style;
    style.dimensionOffset = 0.4;
    style.witnessStartGap = 0.05;
    style.witnessOverhang = 0.1;
    annotation::AlignedDimensionInput input;
    input.first = { 1.0, 2.0, 0.0 };
    input.second = { 1.0, 2.0, 3.0 };
    input.planes.explicitNormal = annotation::Point3 { 0.0, 1.0, 0.0 };
    input.planes.preferredOffsetDirection = annotation::Point3 { 1.0, 0.0, 0.0 };

    const auto result = annotation::ResolveAlignedDimensionGeometry (input, style);

    ASSERT_TRUE (result.has_value ());
    EXPECT_DOUBLE_EQ (result->measurement, 3.0);
    EXPECT_DOUBLE_EQ (result->dimensionFirst.x, 1.4);
    EXPECT_DOUBLE_EQ (result->dimensionSecond.z, 3.0);
    EXPECT_DOUBLE_EQ (result->witnessFirstStart.x, 1.05);
    EXPECT_DOUBLE_EQ (result->witnessFirstEnd.x, 1.5);
}

TEST (DimensionGeometry, UsesPlanePriorityAndPreferredDirection)
{
    annotation::DimensionStyle style;
    annotation::AlignedDimensionInput input;
    input.first = { 0.0, 0.0, 0.0 };
    input.second = { 2.0, 0.0, 0.0 };
    input.planes.explicitNormal = annotation::Point3 { 0.0, 0.0, 1.0 };
    input.planes.geometryNormal = annotation::Point3 { 0.0, 1.0, 0.0 };
    input.planes.preferredOffsetDirection = annotation::Point3 { 0.0, -1.0, 0.0 };

    const auto result = annotation::ResolveAlignedDimensionGeometry (input, style);

    ASSERT_TRUE (result.has_value ());
    EXPECT_DOUBLE_EQ (result->planeNormal.z, 1.0);
    EXPECT_LT (result->offsetDirection.y, 0.0);
}

TEST (DimensionGeometry, FallsThroughParallelPlaneAndRejectsDegenerateInput)
{
    annotation::DimensionStyle style;
    annotation::AlignedDimensionInput input;
    input.first = { 0.0, 0.0, 0.0 };
    input.second = { 1.0, 0.0, 0.0 };
    input.planes.explicitNormal = annotation::Point3 { 1.0, 0.0, 0.0 };
    input.planes.declaredNormal = annotation::Point3 { 0.0, 0.0, 1.0 };
    ASSERT_TRUE (annotation::ResolveAlignedDimensionGeometry (input, style).has_value ());

    input.second = input.first;
    EXPECT_FALSE (annotation::ResolveAlignedDimensionGeometry (input, style).has_value ());
    input.second = { 1.0, 0.0, 0.0 };
    input.planes.declaredNormal.reset ();
    input.planes.cameraFacingNormal.reset ();
    EXPECT_FALSE (annotation::ResolveAlignedDimensionGeometry (input, style).has_value ());
}

TEST (DimensionGeometry, ResolvesCenteredAndOutsideFitDeterministically)
{
    annotation::DimensionStyle style;
    const auto centered = annotation::ResolveDimensionFit ({ 200.0, 60.0, 0.0, 0.0, 1.0 }, style);
    ASSERT_TRUE (centered.has_value ());
    EXPECT_EQ (centered->textMode, annotation::DimensionTextMode::Centered);
    EXPECT_EQ (centered->arrowMode, annotation::DimensionArrowMode::Inward);
    EXPECT_LT (centered->gapStartPx, centered->gapEndPx);

    const auto outside = annotation::ResolveDimensionFit ({ 40.0, 60.0, 2.0, 9.0, 1.0 }, style);
    ASSERT_TRUE (outside.has_value ());
    EXPECT_EQ (outside->textMode, annotation::DimensionTextMode::OutsideBefore);
    EXPECT_EQ (outside->arrowMode, annotation::DimensionArrowMode::Outward);
    EXPECT_GT (outside->extensionBeforePx, 0.0);
    EXPECT_DOUBLE_EQ (outside->extensionAfterPx, 0.0);
}

TEST (DimensionGeometry, ResolvesAngularPlaneValueAndDirectionInModelSpace)
{
    annotation::AngularDimensionInput input;
    input.vertex = { 0.0, 0.0, 0.0 };
    input.firstRayPoint = { 2.0, 0.0, 0.0 };
    input.secondRayPoint = { 0.0, 3.0, 0.0 };
    input.planes.explicitNormal = annotation::Point3 { 0.0, 0.0, -1.0 };

    const auto result = annotation::ResolveAngularDimensionGeometry (input);

    ASSERT_TRUE (result.has_value ());
    EXPECT_NEAR (result->angleRadians, 1.5707963267948966, 1e-12);
    EXPECT_LT (result->planeNormal.z, 0.0);
    input.reverse = true;
    const auto reversed = annotation::ResolveAngularDimensionGeometry (input);
    ASSERT_TRUE (reversed.has_value ());
    EXPECT_DOUBLE_EQ (reversed->firstDirection.y, 1.0);
}

TEST (DimensionGeometry, ResolvesStraightAngleWithDeclaredPlaneAndRejectsDegenerateRays)
{
    annotation::AngularDimensionInput input;
    input.vertex = { 0.0, 0.0, 0.0 };
    input.firstRayPoint = { 1.0, 0.0, 0.0 };
    input.secondRayPoint = { -1.0, 0.0, 0.0 };
    input.planes.declaredNormal = annotation::Point3 { 0.0, 0.0, 1.0 };
    const auto straight = annotation::ResolveAngularDimensionGeometry (input);
    ASSERT_TRUE (straight.has_value ());
    EXPECT_NEAR (straight->angleRadians, 3.14159265358979323846, 1e-12);

    input.secondRayPoint = input.vertex;
    EXPECT_FALSE (annotation::ResolveAngularDimensionGeometry (input).has_value ());
    input.secondRayPoint = input.firstRayPoint;
    EXPECT_FALSE (annotation::ResolveAngularDimensionGeometry (input).has_value ());
}

TEST (DimensionGeometry, RejectsInvalidStyleAndFitValues)
{
    annotation::DimensionStyle style;
    style.capAbovePixels = style.hideBelowPixels - 1.0;
    EXPECT_FALSE (annotation::IsValid (style));
    EXPECT_FALSE (annotation::ResolveDimensionFit ({ 10.0, 5.0, 0.0, 0.0, 1.0 }, style).has_value ());

    style = {};
    EXPECT_FALSE (annotation::ResolveDimensionFit (
                      { 10.0, std::numeric_limits<double>::quiet_NaN (), 0.0, 0.0, 1.0 }, style)
                      .has_value ());
}

} // namespace
