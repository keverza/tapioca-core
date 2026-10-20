// G1/S2b oracle baseline only: production curves remain C++ until Rust parity.
#include "Geometry/Curves.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace {
namespace geo = geomsrv::engine;
constexpr double kPi = 3.14159265358979323846;

void ExpectPoint (geo::Vector3 actual, geo::Vector3 expected, double tolerance = 1.0e-12)
{
    EXPECT_NEAR (actual.x, expected.x, tolerance);
    EXPECT_NEAR (actual.y, expected.y, tolerance);
    EXPECT_NEAR (actual.z, expected.z, tolerance);
}
} // namespace

TEST (Curves, HorizontalArcHasStableFrameAndPositiveSweep)
{
    std::vector<geo::Vector3> points;
    std::string error;
    ASSERT_TRUE (geo::MakeArc ({ 10, 20, 30 }, { 0, 0, 5 }, 2, 0, kPi * 0.5, 8, points, error)) << error;
    ASSERT_EQ (points.size (), 9u);
    // The documented implementation chooses n cross X as the first frame axis.
    ExpectPoint (points.front (), { 10, 22, 30 });
    ExpectPoint (points.back (), { 8, 20, 30 });
    for (const auto& point : points) {
        EXPECT_NEAR (std::hypot (point.x - 10, point.y - 20), 2, 1.0e-12);
        EXPECT_DOUBLE_EQ (point.z, 30);
    }
}

TEST (Curves, ObliqueArcIsOnItsPlaneAtTheRequestedRadius)
{
    const geo::Vector3 centre { 4, 5, 6 };
    std::vector<geo::Vector3> points;
    std::string error;
    ASSERT_TRUE (geo::MakeArc (centre, { 0, 3, 4 }, 7, 0, kPi * 0.5, 16, points, error)) << error;
    ASSERT_EQ (points.size (), 17u);
    ExpectPoint (points.front (), { 11, 5, 6 });
    ExpectPoint (points.back (), { 4, 10.6, 1.8 });
    for (const auto& point : points) {
        const double x = point.x - centre.x, y = point.y - centre.y, z = point.z - centre.z;
        EXPECT_NEAR (3 * y + 4 * z, 0, 1.0e-12);
        EXPECT_NEAR (std::sqrt (x * x + y * y + z * z), 7, 1.0e-12);
    }
}

TEST (Curves, ArcSweepSignFullCircleAndSegmentBounds)
{
    for (int count : { geo::kMinArcSegments, geo::kMaxArcSegments }) {
        for (double sign : { -1.0, 1.0 }) {
            std::vector<geo::Vector3> points;
            std::string error;
            ASSERT_TRUE (geo::MakeArc ({ 0, 0, 0 }, { 0, 0, 1 }, 3, 0, sign * 2 * kPi, count, points, error)) << error;
            ASSERT_EQ (points.size (), static_cast<size_t> (count) + 1);
            ExpectPoint (points.front (), { 0, 3, 0 });
            ExpectPoint (points.back (), points.front ());
        }
    }
    std::vector<geo::Vector3> points;
    std::string error;
    ASSERT_TRUE (geo::MakeArc ({ 0, 0, 0 }, { 0, 0, 1 }, 3, kPi * 0.5, -kPi * 0.5, 2, points, error));
    ExpectPoint (points.front (), { -3, 0, 0 });
    ExpectPoint (points.back (), { 0, 3, 0 });
}

TEST (Curves, InvalidArcParametersRefuseWithoutReplacingPriorPoints)
{
    for (int kind = 0; kind < 8; ++kind) {
        double radius = 1, start = 0, sweep = 1;
        int count = 8;
        geo::Vector3 normal { 0, 0, 1 };
        if (kind == 0)
            radius = 0;
        if (kind == 1)
            radius = std::numeric_limits<double>::infinity ();
        if (kind == 2)
            start = std::numeric_limits<double>::quiet_NaN ();
        if (kind == 3)
            sweep = std::numeric_limits<double>::infinity ();
        if (kind == 4)
            sweep = 0;
        if (kind == 5)
            count = geo::kMinArcSegments - 1;
        if (kind == 6)
            count = geo::kMaxArcSegments + 1;
        if (kind == 7)
            normal = {};
        std::vector<geo::Vector3> points { { 77, 88, 99 } };
        std::string error;
        EXPECT_FALSE (geo::MakeArc ({}, normal, radius, start, sweep, count, points, error));
        EXPECT_FALSE (error.empty ());
        ASSERT_EQ (points.size (), 1u);
        ExpectPoint (points.front (), { 77, 88, 99 });
    }
}

TEST (Curves, PolylineLengthAndCumulativeShareTheSameThreeDimensionalWalk)
{
    const std::vector<geo::Vector3> points { { 1, 2, 3 }, { 4, 6, 3 }, { 4, 6, 15 } };
    std::vector<double> cumulative { 77 };
    EXPECT_DOUBLE_EQ (geo::PolylineLength (points, cumulative), 17);
    EXPECT_EQ (cumulative, (std::vector<double> { 0, 5, 17 }));
    EXPECT_DOUBLE_EQ (geo::PolylineLength ({}, cumulative), 0);
    EXPECT_TRUE (cumulative.empty ());
    EXPECT_DOUBLE_EQ (geo::PolylineLength ({ { 7, 8, 9 } }, cumulative), 0);
    EXPECT_EQ (cumulative, (std::vector<double> { 0 }));
}

TEST (Curves, SamplingUsesArcLengthRatherThanVertexIndexAndClamps)
{
    const std::vector<geo::Vector3> points { { 0, 0, 0 }, { 9, 0, 0 }, { 9, 3, 0 } };
    const double parameters[] { -1, 0.5, 0.75, 0.875, 2 };
    const geo::Vector3 expected[] { { 0, 0, 0 }, { 6, 0, 0 }, { 9, 0, 0 }, { 9, 1.5, 0 }, { 9, 3, 0 } };
    for (size_t i = 0; i < 5; ++i) {
        geo::Vector3 point, tangent;
        std::string error;
        ASSERT_TRUE (geo::PointOnPolyline (points, parameters[i], point, tangent, error)) << error;
        ExpectPoint (point, expected[i]);
        // At an exact join the incoming segment wins, not a smoothed tangent.
        ExpectPoint (tangent, i < 3 ? geo::Vector3 { 1, 0, 0 } : geo::Vector3 { 0, 1, 0 });
    }
}

TEST (Curves, CoincidentPointsAndRepeatedLeadingVertexHaveExplicitLegacyTangents)
{
    geo::Vector3 point, tangent;
    std::string error;
    const geo::Vector3 survey { 1000000000.125, -200000000.25, 8000.5 };
    ASSERT_TRUE (geo::PointOnPolyline ({ survey, survey, survey }, 0.7, point, tangent, error));
    ExpectPoint (point, survey);
    ExpectPoint (tangent, { 1, 0, 0 });
    const std::vector<geo::Vector3> repeated { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 5, 0 } };
    ASSERT_TRUE (geo::PointOnPolyline (repeated, 0, point, tangent, error));
    ExpectPoint (tangent, { 1, 0, 0 }); // Zero first segment uses the legacy fallback.
    ASSERT_TRUE (geo::PointOnPolyline (repeated, 0.5, point, tangent, error));
    ExpectPoint (point, { 0, 2.5, 0 });
    ExpectPoint (tangent, { 0, 1, 0 });
}

TEST (Curves, InvalidSamplingRefusesWithoutReplacingPriorOutputs)
{
    for (bool shortInput : { false, true }) {
        const std::vector<geo::Vector3> points = shortInput ? std::vector<geo::Vector3> { { 0, 0, 0 } }
                                                            : std::vector<geo::Vector3> { { 0, 0, 0 }, { 1, 0, 0 } };
        geo::Vector3 point { 77, 88, 99 }, tangent { 7, 8, 9 };
        std::string error;
        EXPECT_FALSE (geo::PointOnPolyline (points, std::numeric_limits<double>::quiet_NaN (), point, tangent, error));
        EXPECT_FALSE (error.empty ());
        ExpectPoint (point, { 77, 88, 99 });
        ExpectPoint (tangent, { 7, 8, 9 });
    }
}

TEST (Curves, DivideCountMeansSegmentsAndCanExcludeBothEnds)
{
    const std::vector<geo::Vector3> points { { 0, 0, 0 }, { 9, 0, 0 }, { 9, 3, 0 } };
    std::vector<geo::Vector3> included, excluded;
    std::string error;
    ASSERT_TRUE (geo::DividePolyline (points, 4, true, included, error));
    ASSERT_TRUE (geo::DividePolyline (points, 4, false, excluded, error));
    ASSERT_EQ (included.size (), 5u);
    ASSERT_EQ (excluded.size (), 3u);
    ExpectPoint (included.front (), points.front ());
    ExpectPoint (included.back (), points.back ());
    for (size_t i = 0; i < excluded.size (); ++i) {
        ExpectPoint (excluded[i], { 3.0 * static_cast<double> (i + 1), 0, 0 });
        ExpectPoint (included[i + 1], excluded[i]);
    }
    ASSERT_TRUE (geo::DividePolyline (points, 1, false, excluded, error));
    EXPECT_TRUE (excluded.empty ());
}

TEST (Curves, InvalidDivisionRefusesWithoutReplacingPriorPoints)
{
    const std::vector<geo::Vector3> points { { 0, 0, 0 }, { 1, 0, 0 } };
    for (int count : { 0, 100001 }) {
        std::vector<geo::Vector3> result { { 77, 88, 99 } };
        std::string error;
        EXPECT_FALSE (geo::DividePolyline (points, count, true, result, error));
        EXPECT_FALSE (error.empty ());
        ASSERT_EQ (result.size (), 1u);
        ExpectPoint (result.front (), { 77, 88, 99 });
    }
}
