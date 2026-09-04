// L2 offline tests for SunStudy/SunStudyOcclusion.
//
// The scene is arranged so every expected answer is known from arithmetic rather
// than from a previous run: one flat occluder, samples either under it or beside
// it, and a sun straight overhead. What the tests defend is the accumulator's
// contract — the bitset, the back-face cull, idempotent re-resolution, and the
// rule that an incomplete study must be recognisable as incomplete.

#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace evp::sunstudy;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

// A single quad at z = 5 covering x,y in [-1, 1].
std::shared_ptr<const Snapshot> MakeRoofSnapshot ()
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 11;

    Mesh mesh;
    mesh.guid = "roof";
    mesh.vertices = { -1.0, -1.0, 5.0, 1.0, -1.0, 5.0, 1.0, 1.0, 5.0, -1.0, 1.0, 5.0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snap->meshes.push_back (std::move (mesh));
    return snap;
}

// Sample 0 sits under the roof, sample 1 well outside it.
struct Scene {
    std::shared_ptr<CpuTraversal> traversal;
    std::vector<double> positions { 0.0, 0.0, 0.0, 10.0, 0.0, 0.0 };
    std::vector<double> up { 0.0, 0.0, 1.0, 0.0, 0.0, 1.0 };
    std::vector<double> down { 0.0, 0.0, -1.0, 0.0, 0.0, -1.0 };

    Scene () : traversal (std::make_shared<CpuTraversal> (std::make_shared<QueryEngine> (MakeRoofSnapshot ())))
    {
    }

    SampleSet Samples (const double* normals = nullptr) const
    {
        SampleSet set;
        set.positions = positions.data ();
        set.normals = normals;
        set.count = 2;
        return set;
    }
};

const double kSunUp[3] = { 0.0, 0.0, 1.0 };

} // namespace

TEST (SunStudyOcclusion, ShadowedAndSunlitSamplesAreDistinguished)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 1);

    ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 0, kSunUp, 0.001, 0.0, 1));

    EXPECT_FALSE (accumulator.Lit (0, 0)) << "under the roof";
    EXPECT_TRUE (accumulator.Lit (1, 0)) << "beside the roof";
    EXPECT_TRUE (accumulator.Complete ());
}

// ⚠️ A SURFACE TURNED AWAY FROM THE SUN IS SELF-SHADOWED WHATEVER THE GEOMETRY
// DOES. Without this, the back of a wall counts the sun striking its front.
TEST (SunStudyOcclusion, BackFacingSamplesAreNeverLit)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 1);

    // Both normals point down; the sun is straight up.
    ASSERT_TRUE (
        accumulator.AccumulateStep (*scene.traversal, scene.Samples (scene.down.data ()), 0, kSunUp, 0.001, 0.0, 1));

    EXPECT_FALSE (accumulator.Lit (0, 0));
    EXPECT_FALSE (accumulator.Lit (1, 0)) << "unobstructed but facing away";

    // Turn them to face the sun and the unobstructed one lights up.
    accumulator.Reset ();
    ASSERT_TRUE (
        accumulator.AccumulateStep (*scene.traversal, scene.Samples (scene.up.data ()), 0, kSunUp, 0.001, 0.0, 1));
    EXPECT_FALSE (accumulator.Lit (0, 0));
    EXPECT_TRUE (accumulator.Lit (1, 0));
}

TEST (SunStudyOcclusion, WithoutNormalsEverySampleFacesTheSun)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 1);

    // A bare ground grid: no orientation, so only geometry decides.
    ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (nullptr), 0, kSunUp, 0.001, 0.0, 1));
    EXPECT_TRUE (accumulator.Lit (1, 0));
}

TEST (SunStudyOcclusion, HoursCountResolvedStepsOnly)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 4);

    for (size_t step = 0; step < 4; ++step)
        ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), step, kSunUp, 0.001, 0.0, 1));

    EXPECT_EQ (accumulator.LitStepCount (0), 0u);
    EXPECT_EQ (accumulator.LitStepCount (1), 4u);

    const std::vector<double> hours = accumulator.SunHours (0.25);
    ASSERT_EQ (hours.size (), 2u);
    EXPECT_NEAR (hours[0], 0.0, 1e-12);
    EXPECT_NEAR (hours[1], 1.0, 1e-12);
}

// ⚠️ TOO LOW IS INDISTINGUISHABLE FROM A SHADIER SITE. An incomplete study must
// be recognisable through Complete(), never inferred from the numbers.
TEST (SunStudyOcclusion, AnIncompleteStudyReportsItselfIncomplete)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 4);

    ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 0, kSunUp, 0.001, 0.0, 1));
    ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 1, kSunUp, 0.001, 0.0, 1));

    EXPECT_FALSE (accumulator.Complete ());
    EXPECT_EQ (accumulator.ResolvedStepCount (), 2u);
    EXPECT_TRUE (accumulator.StepResolved (0));
    EXPECT_FALSE (accumulator.StepResolved (2));

    // The value is honest for what it covers, and lower than the finished one.
    EXPECT_NEAR (accumulator.SunHours (0.25)[1], 0.5, 1e-12);
}

// A caller that loses track of which steps it has done must not be able to
// corrupt the total by redoing one.
TEST (SunStudyOcclusion, ReResolvingAStepIsIdempotent)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 3);

    for (int repeat = 0; repeat < 3; ++repeat)
        ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 1, kSunUp, 0.001, 0.0, 1));

    EXPECT_EQ (accumulator.ResolvedStepCount (), 1u);
    EXPECT_EQ (accumulator.LitStepCount (1), 1u) << "one step, however many times it was run";
}

// The same step re-run with the sun somewhere else must REPLACE the old answer,
// not accumulate on top of it.
TEST (SunStudyOcclusion, ReResolvingWithADifferentSunReplacesTheBit)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 1);

    ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 0, kSunUp, 0.001, 0.0, 1));
    EXPECT_TRUE (accumulator.Lit (1, 0));

    // Straight down: nothing below, but sample 1's bit must be recomputed from
    // scratch rather than left set.
    const double sunDown[3] = { 0.0, 0.0, -1.0 };
    ASSERT_TRUE (
        accumulator.AccumulateStep (*scene.traversal, scene.Samples (scene.up.data ()), 0, sunDown, 0.001, 0.0, 1));
    EXPECT_FALSE (accumulator.Lit (1, 0)) << "back-facing now, so the old set bit must have been cleared";
}

TEST (SunStudyOcclusion, RangeAccumulationWalksTheSeriesInSlices)
{
    Scene scene;

    std::vector<SunStep> steps;
    for (int i = 0; i < 5; ++i) {
        SunStep step;
        step.time = TimeOfDay { 8 + i, 0 };
        step.altitudeDegrees = 30.0;
        step.direction[0] = 0.0;
        step.direction[1] = 0.0;
        step.direction[2] = 1.0;
        steps.push_back (step);
    }
    const SunSeries series = SunSeries::FromSteps (steps, 60);
    ASSERT_EQ (series.StepCount (), 5u);

    OcclusionAccumulator accumulator (2, series.StepCount ());

    // Two slices of two, then the remainder: the progressive shape.
    EXPECT_EQ (accumulator.AccumulateRange (*scene.traversal, scene.Samples (), series, 0, 2, 0.001, 0.0, 1), 2u);
    EXPECT_FALSE (accumulator.Complete ());
    EXPECT_EQ (accumulator.AccumulateRange (*scene.traversal, scene.Samples (), series, 2, 2, 0.001, 0.0, 1), 2u);
    EXPECT_FALSE (accumulator.Complete ());
    EXPECT_EQ (accumulator.AccumulateRange (*scene.traversal, scene.Samples (), series, 4, 10, 0.001, 0.0, 1), 1u)
        << "a slice larger than what remains stops at the end";
    EXPECT_TRUE (accumulator.Complete ());

    EXPECT_EQ (accumulator.LitStepCount (1), 5u);
    EXPECT_NEAR (accumulator.SunHours (series.HoursPerStep ())[1], 5.0, 1e-12);
}

// More steps than fit in one 64-bit word is the ordinary case: a 15-minute day
// is 96 steps.
TEST (SunStudyOcclusion, MoreThanSixtyFourStepsSpanMultipleWords)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 96);
    EXPECT_EQ (accumulator.WordsPerSample (), 2u);

    for (size_t step = 0; step < 96; ++step)
        ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), step, kSunUp, 0.001, 0.0, 1));

    EXPECT_TRUE (accumulator.Complete ());
    EXPECT_EQ (accumulator.LitStepCount (1), 96u);
    EXPECT_EQ (accumulator.LitStepCount (0), 0u);
    EXPECT_TRUE (accumulator.Lit (1, 0));
    EXPECT_TRUE (accumulator.Lit (1, 63)) << "last bit of word 0";
    EXPECT_TRUE (accumulator.Lit (1, 64)) << "first bit of word 1";
    EXPECT_TRUE (accumulator.Lit (1, 95));
}

TEST (SunStudyOcclusion, ResetClearsEverything)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 2);
    ASSERT_TRUE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 0, kSunUp, 0.001, 0.0, 1));
    ASSERT_EQ (accumulator.ResolvedStepCount (), 1u);

    accumulator.Reset ();
    EXPECT_EQ (accumulator.ResolvedStepCount (), 0u);
    EXPECT_FALSE (accumulator.Lit (1, 0));
    EXPECT_FALSE (accumulator.StepResolved (0));
}

TEST (SunStudyOcclusion, OutOfRangeOrMismatchedCallsChangeNothing)
{
    Scene scene;
    OcclusionAccumulator accumulator (2, 2);

    EXPECT_FALSE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 5, kSunUp, 0.001, 0.0, 1));
    EXPECT_FALSE (accumulator.AccumulateStep (*scene.traversal, scene.Samples (), 0, nullptr, 0.001, 0.0, 1));

    SampleSet wrongSize = scene.Samples ();
    wrongSize.count = 3;
    EXPECT_FALSE (accumulator.AccumulateStep (*scene.traversal, wrongSize, 0, kSunUp, 0.001, 0.0, 1));

    SampleSet noPositions = scene.Samples ();
    noPositions.positions = nullptr;
    EXPECT_FALSE (accumulator.AccumulateStep (*scene.traversal, noPositions, 0, kSunUp, 0.001, 0.0, 1));

    EXPECT_EQ (accumulator.ResolvedStepCount (), 0u);
}

TEST (SunStudyOcclusion, ThreadedAndInlineAccumulationAgree)
{
    Scene scene;
    OcclusionAccumulator inlineRun (2, 3);
    OcclusionAccumulator threadedRun (2, 3);

    for (size_t step = 0; step < 3; ++step) {
        inlineRun.AccumulateStep (*scene.traversal, scene.Samples (scene.up.data ()), step, kSunUp, 0.001, 0.0, 1);
        threadedRun.AccumulateStep (*scene.traversal, scene.Samples (scene.up.data ()), step, kSunUp, 0.001, 0.0, 8);
    }

    EXPECT_EQ (inlineRun.Bits (), threadedRun.Bits ());
}
