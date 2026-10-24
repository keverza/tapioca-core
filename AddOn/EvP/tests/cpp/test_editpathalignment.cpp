#include "Edit/EditPath.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
constexpr double kPi = 3.14159265358979323846;

std::vector<ed::ControlPoint> Corner (double radius)
{
    return { { 1, { 0, 0, 0 } }, { 2, { 10, 0, 0 }, radius }, { 3, { 10, 10, 0 } } };
}
} // namespace

TEST (EditPathAlignment, RoundedCornerIsLineArcLine)
{
    ed::AlignmentGeometry g;
    std::string error;
    ASSERT_TRUE (ed::BuildAlignment (Corner (2.0), g, error)) << error;
    ASSERT_EQ (g.elements.size (), 3u);
    EXPECT_EQ (g.elements[1].kind, ed::ElementKind::Arc);
    EXPECT_NEAR (g.elements[0].length, 8.0, 1e-12);
    EXPECT_NEAR (g.elements[1].length, kPi, 1e-12);
    EXPECT_NEAR (g.length, 16.0 + kPi, 1e-12);
    EXPECT_NEAR (g.pointStations[1], 8.0 + kPi / 2, 1e-12);
    EXPECT_NEAR (g.pointStations[2], g.length, 1e-12);
}

TEST (EditPathAlignment, LocateAlongTheArcIsTangent)
{
    ed::AlignmentGeometry g;
    std::string error;
    ASSERT_TRUE (ed::BuildAlignment (Corner (2.0), g, error));
    ed::Placement p;
    ASSERT_TRUE (ed::Locate (g, 8.0 + kPi / 2, p));
    EXPECT_NEAR (p.point.x, 8.0 + std::sqrt (2.0), 1e-9);
    EXPECT_NEAR (p.point.y, 2.0 - std::sqrt (2.0), 1e-9);
    EXPECT_NEAR (p.tangent.x, std::sqrt (0.5), 1e-9);
    EXPECT_NEAR (p.tangent.y, std::sqrt (0.5), 1e-9);
    EXPECT_NEAR (p.curvature, 0.5, 1e-12); // a left turn
    EXPECT_NEAR (p.left.x, -std::sqrt (0.5), 1e-9);
    ASSERT_TRUE (ed::Locate (g, g.length, p));
    EXPECT_NEAR (p.point.y, 10.0, 1e-9);
    EXPECT_FALSE (ed::Locate (g, g.length + 1.0, p));
}

TEST (EditPathAlignment, StationAndSignedOffsetOfAPoint)
{
    ed::AlignmentGeometry g;
    std::string error;
    ASSERT_TRUE (ed::BuildAlignment (Corner (2.0), g, error));
    double s = 0, offset = 0;
    ASSERT_TRUE (ed::StationOf (g, { 12, 5, 7 }, s, offset));
    EXPECT_NEAR (s, 8.0 + kPi + 3.0, 1e-9);
    EXPECT_NEAR (offset, -2.0, 1e-9); // right of a path heading north
    ASSERT_TRUE (ed::StationOf (g, { 4, 1, 0 }, s, offset));
    EXPECT_NEAR (s, 4.0, 1e-9);
    EXPECT_NEAR (offset, 1.0, 1e-9);
    // Inside the bend: nearest to the arc.
    ASSERT_TRUE (ed::StationOf (g, { 8, 2, 0 }, s, offset));
    EXPECT_NEAR (offset, 2.0, 1e-9);
}

TEST (EditPathAlignment, RightTurnsSweepNegative)
{
    ed::AlignmentGeometry g;
    std::string error;
    ASSERT_TRUE (
        ed::BuildAlignment ({ { 1, { 0, 0, 0 } }, { 2, { 10, 0, 0 }, 3.0 }, { 3, { 10, -10, 0 } } }, g, error));
    EXPECT_LT (g.elements[1].sweep, 0.0);
    ed::Placement p;
    ASSERT_TRUE (ed::Locate (g, g.length - 1.0, p));
    EXPECT_NEAR (p.tangent.y, -1.0, 1e-9);
    EXPECT_NEAR (p.point.x, 10.0, 1e-9);
}

TEST (EditPathAlignment, SharpCornersAndStraightPoints)
{
    ed::AlignmentGeometry g;
    std::string error;
    ASSERT_TRUE (ed::BuildAlignment (Corner (0.0), g, error));
    EXPECT_EQ (g.elements.size (), 2u);
    EXPECT_NEAR (g.length, 20.0, 1e-12);
    // A collinear point with a radius has no corner to round.
    ASSERT_TRUE (ed::BuildAlignment ({ { 1, { 0, 0, 0 } }, { 2, { 5, 0, 0 }, 4.0 }, { 3, { 10, 0, 0 } } }, g, error));
    EXPECT_NEAR (g.length, 10.0, 1e-12);
    EXPECT_NEAR (g.pointStations[1], 5.0, 1e-12);
}

TEST (EditPathAlignment, ARadiusTooLargeForItsLegsIsFittedNotRefused)
{
    ed::AlignmentGeometry g;
    std::string error;
    // 10 m legs at a right angle: a 10 m radius is the most that fits.
    ASSERT_TRUE (ed::BuildAlignment (Corner (20.0), g, error)) << error;
    ASSERT_EQ (g.fittedRadius.size (), 3u);
    EXPECT_NEAR (g.fittedRadius[1], 10.0, 1e-9);
    EXPECT_EQ (g.elements.size (), 1u) << "the arc takes both legs whole";
    EXPECT_NEAR (g.elements[0].radius, 10.0, 1e-9);
    ASSERT_TRUE (ed::BuildAlignment (Corner (2.0), g, error));
    EXPECT_DOUBLE_EQ (g.fittedRadius[1], 2.0) << "a radius that fits is kept";
    // Two bends sharing a 4 m leg, each asking for a 3 m tangent: both shrink to 2 m.
    ASSERT_TRUE (ed::BuildAlignment (
        { { 1, { 0, 0, 0 } }, { 2, { 10, 0, 0 }, 3.0 }, { 3, { 10, 4, 0 }, 3.0 }, { 4, { 0, 4, 0 } } }, g, error))
        << error;
    EXPECT_NEAR (g.fittedRadius[1], 2.0, 1e-9);
    EXPECT_NEAR (g.fittedRadius[2], 2.0, 1e-9);
}

TEST (EditPathAlignment, RefusesWhatCannotBeBuilt)
{
    ed::AlignmentGeometry g;
    std::string error;
    EXPECT_FALSE (ed::BuildAlignment ({ { 1, { 0, 0, 0 } }, { 2, { 10, 0, 0 } }, { 3, { 0, 0, 0 } } }, g, error));
    EXPECT_NE (error.find ("doubles back"), std::string::npos);
    EXPECT_FALSE (ed::BuildAlignment ({ { 1, { 0, 0, 0 } }, { 2, { 0, 0, 5 } } }, g, error));
    EXPECT_FALSE (ed::BuildAlignment ({ { 1, { 0, 0, 0 } } }, g, error));
}

TEST (EditPathAlignment, ElementStationsBracketTheElements)
{
    ed::AlignmentGeometry g;
    std::string error;
    ASSERT_TRUE (ed::BuildAlignment (Corner (2.0), g, error));
    const auto s = ed::ElementStations (g);
    ASSERT_EQ (s.size (), 4u);
    EXPECT_NEAR (s[1], 8.0, 1e-12);
    EXPECT_NEAR (s[3], g.length, 1e-12);
}
