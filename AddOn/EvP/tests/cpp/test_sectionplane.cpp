// ArchViz/SectionPlane -- the viewer's movable section plane (issue #14 P2).
//
// Every way of getting the plane wrong draws a plausible model: a sign error keeps the wrong
// half, an axis slip cuts the wrong direction, and a disabled plane that still clips hides a
// sliver at the bounds. So the keep/cut side is pinned here with real points.

#include "ArchViz/SectionPlane.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace geomsrv::archviz;

namespace {

const float kMin[3] = { -10.0f, 0.0f, 0.0f };
const float kMax[3] = { 10.0f, 40.0f, 12.0f };

float Distance (const float plane[4], float x, float y, float z)
{
    return plane[0] * x + plane[1] * y + plane[2] * z + plane[3];
}

} // namespace

TEST (SectionPlane, DisabledIsTheNoSectionPlane)
{
    float plane[4];
    SectionPlaneEquation (SectionPlaneState {}, kMin, kMax, plane);
    EXPECT_FLOAT_EQ (plane[0], 0.0f);
    EXPECT_FLOAT_EQ (plane[1], 0.0f);
    EXPECT_FLOAT_EQ (plane[2], 0.0f);
    EXPECT_FLOAT_EQ (plane[3], 1.0f);
    EXPECT_GT (Distance (plane, 1.0e6f, -1.0e6f, 1.0e6f), 0.0f);
}

TEST (SectionPlane, HorizontalCutKeepsTheLowSideAndFlipKeepsTheHigh)
{
    SectionPlaneState state;
    state.enabled = true;
    state.axis = SectionAxis::Z;
    state.position = 0.25f; // z = 3
    float plane[4];
    SectionPlaneEquation (state, kMin, kMax, plane);
    EXPECT_GT (Distance (plane, 0.0f, 0.0f, 1.0f), 0.0f);  // ground floor kept
    EXPECT_LT (Distance (plane, 0.0f, 0.0f, 10.0f), 0.0f); // roof cut away
    EXPECT_NEAR (Distance (plane, 5.0f, 7.0f, 3.0f), 0.0f, 1.0e-5f);

    state.flipped = true;
    SectionPlaneEquation (state, kMin, kMax, plane);
    EXPECT_LT (Distance (plane, 0.0f, 0.0f, 1.0f), 0.0f);
    EXPECT_GT (Distance (plane, 0.0f, 0.0f, 10.0f), 0.0f);
}

TEST (SectionPlane, VerticalCutsUseTheirOwnAxisOnly)
{
    SectionPlaneState state;
    state.enabled = true;
    state.position = 0.5f;
    float plane[4];

    state.axis = SectionAxis::X; // x = 0
    SectionPlaneEquation (state, kMin, kMax, plane);
    EXPECT_GT (Distance (plane, -1.0f, 39.0f, 11.0f), 0.0f);
    EXPECT_LT (Distance (plane, 1.0f, 0.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ (plane[1], 0.0f);
    EXPECT_FLOAT_EQ (plane[2], 0.0f);

    state.axis = SectionAxis::Y; // y = 20
    SectionPlaneEquation (state, kMin, kMax, plane);
    EXPECT_GT (Distance (plane, 9.0f, 19.0f, 0.0f), 0.0f);
    EXPECT_LT (Distance (plane, -9.0f, 21.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ (SectionPlaneCoordinate (state, kMin, kMax), 20.0f);
}

TEST (SectionPlane, EndsOfTheRangeKeepOrCutTheWholeModel)
{
    SectionPlaneState state;
    state.enabled = true;
    float plane[4];
    state.position = 1.0f;
    SectionPlaneEquation (state, kMin, kMax, plane);
    EXPECT_GE (Distance (plane, 0.0f, 0.0f, kMax[2]), 0.0f); // the top face itself is kept
    state.position = 7.0f;                                   // clamped to 1
    SectionPlaneEquation (state, kMin, kMax, plane);
    EXPECT_GE (Distance (plane, 0.0f, 0.0f, kMax[2]), 0.0f);
    state.position = std::numeric_limits<float>::quiet_NaN (); // treated as the middle
    EXPECT_FLOAT_EQ (SectionPlaneCoordinate (state, kMin, kMax), 6.0f);
}

TEST (SectionPlane, FlatBoundsCutNothing)
{
    SectionPlaneState state;
    state.enabled = true;
    const float flatMin[3] = { 0.0f, 0.0f, 5.0f };
    const float flatMax[3] = { 10.0f, 10.0f, 5.0f }; // no extent along Z
    float plane[4];
    SectionPlaneEquation (state, flatMin, flatMax, plane);
    EXPECT_FLOAT_EQ (plane[2], 0.0f);
    EXPECT_FLOAT_EQ (plane[3], 1.0f);
}
