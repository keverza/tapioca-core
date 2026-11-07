// One point's year on demand (#152): the month x hour grids and the per-day
// lines from one batch of rays, culled as the day engine culls.

#include "SunStudy/SunStudyPointProfile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

using namespace evp::sunstudy;

// Blocks every ray toward the east (dir.x > 0).
class EastWall final : public ITraversal {
  public:
    void OccludeDirectional (const double*, size_t count, const double*, double, double, uint8_t* out,
                             size_t) const override
    {
        std::fill (out, out + count, uint8_t (0));
    }
    void OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out, size_t) const override
    {
        for (size_t i = 0; i < count; ++i)
            out[i] = rays[i].dir[0] > 0.0 ? 1 : 0;
        calls += 1;
    }
    uint64_t SceneVersion () const override
    {
        return 1;
    }
    mutable int calls = 0;
};

SunStep Step (int hour, double x, double y, double z)
{
    SunStep step;
    step.time = TimeOfDay { hour, 0 };
    step.direction[0] = x;
    step.direction[1] = y;
    step.direction[2] = z;
    step.altitudeDegrees = 30.0;
    return step;
}

ProfileDay Day (int month, double weight)
{
    ProfileDay day;
    day.month = month;
    day.day = 15;
    day.weightDays = weight;
    day.hoursPerStep = 1.0;
    // Morning east, noon overhead, afternoon west, evening behind (north, low).
    day.steps = { Step (8, 0.6, 0.0, 0.8), Step (12, 0.0, 0.0, 1.0), Step (16, -0.6, 0.0, 0.8),
                  Step (19, 0.0, 0.9, -0.1) };
    return day;
}

TEST (SunStudyPointProfile, GridsAreWeightedAndOrderedUpFacingLit)
{
    const EastWall traversal;
    const double position[3] = { 0, 0, 0 };
    const double roof[3] = { 0, 0, 1 };
    PointProfile profile;
    ASSERT_TRUE (ComputePointProfile (position, roof, { Day (3, 31.0), Day (6, 30.0) }, traversal, 0.001, profile));
    EXPECT_EQ (traversal.calls, 1); // one batch
    EXPECT_EQ (profile.rays, 6u);   // 3 facing steps a day; the evening sun is below the roof
    EXPECT_DOUBLE_EQ (profile.up[2][19], 31.0);
    EXPECT_DOUBLE_EQ (profile.facing[2][19], 0.0);
    EXPECT_DOUBLE_EQ (profile.facing[2][8], 31.0);
    EXPECT_DOUBLE_EQ (profile.lit[2][8], 0.0); // the east wall
    EXPECT_DOUBLE_EQ (profile.lit[5][12], 30.0);
    EXPECT_DOUBLE_EQ (profile.lit[5][16], 30.0);
    ASSERT_EQ (profile.dayLit.size (), 2u);
    EXPECT_DOUBLE_EQ (profile.dayUp[0], 4.0);
    EXPECT_DOUBLE_EQ (profile.dayFacing[0], 3.0);
    EXPECT_DOUBLE_EQ (profile.dayLit[0], 2.0);
    EXPECT_EQ (profile.dayMonth[1], 6);
    ASSERT_EQ (profile.dayStepLit.size (), 2u);
    ASSERT_EQ (profile.dayStepFacing.size (), 2u);
    EXPECT_EQ (profile.dayStepFacing[0], (std::vector<uint8_t> { 1, 1, 1, 0 }));
    EXPECT_EQ (profile.dayStepLit[0], (std::vector<uint8_t> { 0, 1, 1, 0 }));
    EXPECT_EQ (profile.dayStepLit[1], (std::vector<uint8_t> { 0, 1, 1, 0 }));
}

TEST (SunStudyPointProfile, GroundSamplesFaceEverythingAndCancellationStops)
{
    const EastWall traversal;
    const double position[3] = { 0, 0, 0 };
    PointProfile profile;
    ASSERT_TRUE (ComputePointProfile (position, nullptr, { Day (1, 1.0) }, traversal, 0.001, profile));
    EXPECT_EQ (profile.rays, 4u);
    EXPECT_DOUBLE_EQ (profile.dayLit[0], 3.0);
    EXPECT_EQ (profile.dayStepFacing[0], (std::vector<uint8_t> { 1, 1, 1, 1 }));
    EXPECT_EQ (profile.dayStepLit[0], (std::vector<uint8_t> { 0, 1, 1, 1 }));
    EXPECT_FALSE (
        ComputePointProfile (position, nullptr, { Day (1, 1.0) }, traversal, 0.001, profile, [] { return true; }));
}

// A study point snaps to the nearest sample and takes in the samples within
// its radius of THAT sample, nearest first, up to the cap.
TEST (SunStudyPointProfile, ASphereSnapsToTheNearestSampleAndTakesItsNeighbours)
{
    const std::vector<double> positions = { 0, 0, 0, 1, 0, 0, 2, 0, 0, 5, 0, 0 };
    const double clicked[3] = { 1.2, 0.1, 0.0 };
    double snapped[3] = {};
    EXPECT_EQ (SamplesInSphere (positions, clicked, 0.0, 10, snapped), (std::vector<size_t> { 1 }));
    EXPECT_DOUBLE_EQ (snapped[0], 1.0);
    EXPECT_EQ (SamplesInSphere (positions, clicked, 1.0, 10, snapped), (std::vector<size_t> { 1, 0, 2 }));
    EXPECT_EQ (SamplesInSphere (positions, clicked, 1.0, 2, snapped).size (), 2u);
    EXPECT_TRUE (SamplesInSphere ({}, clicked, 1.0, 10, snapped).empty ());
}

TEST (SunStudyPointProfile, CellRangesKeepEveryDayIncludingEmptyAndShadowedDays)
{
    std::vector<double> minimum, maximum;
    AccumulateDayCellRange ({ 2.0, 0.0, 0.0 }, true, minimum, maximum);
    EXPECT_EQ (minimum, (std::vector<double> { 2.0, 0.0, 0.0 }));
    EXPECT_EQ (maximum, minimum);
    AccumulateDayCellRange ({ 1.0, 0.0, 0.0 }, false, minimum, maximum);
    EXPECT_EQ (minimum, (std::vector<double> { 1.0, 0.0, 0.0 }));
    EXPECT_EQ (maximum, (std::vector<double> { 2.0, 0.0, 0.0 }));
    AccumulateDayCellRange ({ 3.0, 0.0, 0.0 }, false, minimum, maximum);
    EXPECT_EQ (minimum, (std::vector<double> { 1.0, 0.0, 0.0 }));
    EXPECT_EQ (maximum, (std::vector<double> { 3.0, 0.0, 0.0 }));
}

} // namespace
