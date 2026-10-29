#include "ArchViz/AnalysisPath.hpp"

#include <gtest/gtest.h>
#include <cmath>

using namespace geomsrv::archviz::analysispath;

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST (AnalysisPath, AnOpenPolylineIsEvaluatedByLengthAlongIt)
{
    // An L: 3 m east, then 1 m north, at a storey 2.5 m up.
    const Path path = Build ("{guid}", { 0, 0, 3, 0, 3, 1 }, { 0, 0, 0 }, false, 2.5);
    ASSERT_EQ (path.points.size (), 3u);
    EXPECT_DOUBLE_EQ (path.Length (), 4.0);
    Point3 at, tangent;
    ASSERT_TRUE (Evaluate (path, 0.5, at, tangent));
    EXPECT_NEAR (at[0], 2.0, 1e-12);
    EXPECT_NEAR (at[1], 0.0, 1e-12);
    EXPECT_NEAR (at[2], 2.5, 1e-12);
    EXPECT_NEAR (tangent[0], 1.0, 1e-12);
    ASSERT_TRUE (Evaluate (path, 0.875, at, tangent));
    EXPECT_NEAR (at[0], 3.0, 1e-12);
    EXPECT_NEAR (at[1], 0.5, 1e-12);
    EXPECT_NEAR (tangent[1], 1.0, 1e-12) << "the second leg runs north";
    // Clamped at both ends.
    ASSERT_TRUE (Evaluate (path, -1.0, at, tangent));
    EXPECT_NEAR (at[0], 0.0, 1e-12);
    ASSERT_TRUE (Evaluate (path, 2.0, at, tangent));
    EXPECT_NEAR (at[1], 1.0, 1e-12);
}

TEST (AnalysisPath, AClosedPolylineRunsBackToItsStart)
{
    const Path path = Build ("{guid}", { 0, 0, 2, 0, 2, 2, 0, 2 }, {}, true, 0.0);
    EXPECT_DOUBLE_EQ (path.Length (), 8.0);
    Point3 at, tangent;
    ASSERT_TRUE (Evaluate (path, 0.9375, at, tangent));
    EXPECT_NEAR (at[0], 0.0, 1e-12);
    EXPECT_NEAR (at[1], 0.5, 1e-12);
    EXPECT_NEAR (tangent[1], -1.0, 1e-12) << "the closing edge runs south";
}

TEST (AnalysisPath, AnArcEdgeFollowsItsCircleCounterClockwiseForAPositiveAngle)
{
    // A quarter circle about the origin from (1, 0) to (0, 1).
    const Path path = Build ("{guid}", { 1, 0, 0, 1 }, { 0.5 * kPi, 0 }, false, 0.0);
    ASSERT_GT (path.points.size (), 3u);
    for (const auto& point : path.points)
        EXPECT_NEAR (std::hypot (point[0], point[1]), 1.0, 1e-12) << "on the unit circle";
    EXPECT_NEAR (path.Length (), 0.5 * kPi, 0.005) << "close to the arc, not the chord";
    Point3 at, tangent;
    ASSERT_TRUE (Evaluate (path, 0.5, at, tangent));
    EXPECT_GT (at[0], 0.7);
    EXPECT_GT (at[1], 0.7) << "bulging away from the origin's side of the chord";
    EXPECT_EQ (path.points.back (), (Point3 { 0, 1, 0 }));
}

TEST (AnalysisPath, TooFewPointsOrNoLengthIsAnEmptyPath)
{
    Point3 at, tangent;
    EXPECT_FALSE (Evaluate (Build ("", { 1, 1 }, {}, false, 0.0), 0.5, at, tangent));
    EXPECT_FALSE (Evaluate (Build ("", { 1, 1, 1, 1 }, {}, false, 0.0), 0.5, at, tangent));
}

TEST (AnalysisPath, TheNearestPointIsFoundInPlanWhereverItIsAlong)
{
    const Path path = Build ("{guid}", { 0, 0, 10, 0, 10, 10 }, {}, false, 0.0);
    EXPECT_NEAR (Nearest (path, { 4, 1, 5 }), 0.2, 1e-12) << "height does not count";
    EXPECT_NEAR (Nearest (path, { 12, 5, 0 }), 0.75, 1e-12);
    EXPECT_NEAR (Nearest (path, { -3, -3, 0 }), 0.0, 1e-12);
    // An edited path: the same scene point maps to its new place along it.
    const Path longer = Build ("{guid}", { -10, 0, 10, 0, 10, 10 }, {}, false, 0.0);
    EXPECT_NEAR (Nearest (longer, { 4, 1, 5 }) * longer.Length (), 14.0, 1e-12);
}
