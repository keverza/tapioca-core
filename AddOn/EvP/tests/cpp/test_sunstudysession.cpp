// L2 offline tests for SunStudy/SunStudySession.
//
// The session is where the two promises live that the rest of the feature rests
// on: that a study advances in small bounded steps rather than blocking, and
// that changing a display setting costs nothing. Both are properties of
// invalidation, so most of this file is about what Sync does and does not throw
// away.

#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudySession.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace evp::sunstudy;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

std::shared_ptr<const Snapshot> MakeRoofSnapshot (uint64_t id = 21)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = id;
    Mesh mesh;
    mesh.guid = "roof";
    mesh.vertices = { -1.0, -1.0, 5.0, 1.0, -1.0, 5.0, 1.0, 1.0, 5.0, -1.0, 1.0, 5.0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snap->meshes.push_back (std::move (mesh));
    return snap;
}

SunSeries MakeDay (int steps, int timestepMinutes = 60)
{
    std::vector<SunStep> raw;
    for (int i = 0; i < steps; ++i) {
        SunStep step;
        step.time = TimeOfDay { 6 + i, 0 };
        step.altitudeDegrees = 30.0;
        step.direction[0] = 0.0;
        step.direction[1] = 0.0;
        step.direction[2] = 1.0;
        raw.push_back (step);
    }
    return SunSeries::FromSteps (raw, timestepMinutes);
}

struct Fixture {
    CpuTraversal traversal { std::make_shared<QueryEngine> (MakeRoofSnapshot ()) };
    std::vector<double> positions { 0.0, 0.0, 0.0, 10.0, 0.0, 0.0 };

    SampleSet Samples ()
    {
        SampleSet set;
        set.positions = positions.data ();
        set.count = 2;
        return set;
    }
};

StudyInputs Inputs (uint64_t geometry, uint64_t sun, uint64_t grid)
{
    StudyInputs inputs;
    inputs.geometryVersion = geometry;
    inputs.sunVersion = sun;
    inputs.gridVersion = grid;
    return inputs;
}

} // namespace

// ---------------------------------------------------------------------------
// advancing
// ---------------------------------------------------------------------------

TEST (SunStudySession, AdvancesInBoundedSlicesRatherThanBlocking)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (5);
    session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());

    EXPECT_FALSE (session.Progress ().converged);
    EXPECT_EQ (session.Progress ().totalSteps, 5u);

    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 2u);
    EXPECT_EQ (session.Progress ().resolvedSteps, 2u);
    EXPECT_FALSE (session.Progress ().converged);

    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 2u);
    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 1u) << "stops at the end of the day";
    EXPECT_TRUE (session.Progress ().converged);

    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 0u) << "converged: nothing left to do";
}

TEST (SunStudySession, ResultsAreCorrectOnceConverged)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (4);
    session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());

    while (session.Advance (fixture.traversal, 1, 0.001, 0.0, 1) > 0) {
    }

    ASSERT_TRUE (session.Progress ().converged);
    const std::vector<double> hours = session.SunHours ();
    ASSERT_EQ (hours.size (), 2u);
    EXPECT_NEAR (hours[0], 0.0, 1e-12) << "under the roof all day";
    EXPECT_NEAR (hours[1], 4.0, 1e-12) << "in the open all day";
}

TEST (SunStudySession, AZeroBudgetDoesNothingButIsNotAnError)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (3);
    session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());

    EXPECT_EQ (session.Advance (fixture.traversal, 0, 0.001, 0.0, 1), 0u);
    EXPECT_EQ (session.Progress ().resolvedSteps, 0u);
}

// ---------------------------------------------------------------------------
// selective invalidation — the point of the class
// ---------------------------------------------------------------------------

// ⚠️ THE BEHAVIOUR THE WHOLE DESIGN EXISTS FOR. A threshold, palette, display
// mode or filter change touches none of the three versions, so re-syncing with
// the same inputs must keep every resolved step.
TEST (SunStudySession, UnchangedInputsKeepEveryResolvedStep)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (5);
    const StudyInputs inputs = Inputs (1, day.Version (), 1);

    session.Sync (inputs, day, fixture.Samples ());
    session.Advance (fixture.traversal, 3, 0.001, 0.0, 1);
    const uint64_t generation = session.Generation ();
    ASSERT_EQ (session.Progress ().resolvedSteps, 3u);

    // The user drags a threshold slider: same three versions, ten times over.
    for (int i = 0; i < 10; ++i)
        session.Sync (inputs, day, fixture.Samples ());

    EXPECT_EQ (session.Progress ().resolvedSteps, 3u) << "a display change must not recompute";
    EXPECT_EQ (session.Generation (), generation) << "and must not retire work in flight";
}

TEST (SunStudySession, AGeometryChangeDiscardsEverything)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (5);

    session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());
    session.Advance (fixture.traversal, 3, 0.001, 0.0, 1);
    const uint64_t generation = session.Generation ();

    session.Sync (Inputs (2, day.Version (), 1), day, fixture.Samples ());
    EXPECT_EQ (session.Progress ().resolvedSteps, 0u);
    EXPECT_GT (session.Generation (), generation) << "work in flight has to be retirable";
}

TEST (SunStudySession, ASunChangeDiscardsTheAccumulatorAndAdoptsTheNewDay)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries shortDay = MakeDay (3);
    session.Sync (Inputs (1, shortDay.Version (), 1), shortDay, fixture.Samples ());
    while (session.Advance (fixture.traversal, 1, 0.001, 0.0, 1) > 0) {
    }
    ASSERT_TRUE (session.Progress ().converged);

    // The user picks a different date: more steps, and none of them resolved.
    const SunSeries longDay = MakeDay (8);
    ASSERT_NE (longDay.Version (), shortDay.Version ());
    session.Sync (Inputs (1, longDay.Version (), 1), longDay, fixture.Samples ());

    EXPECT_FALSE (session.Progress ().converged);
    EXPECT_EQ (session.Progress ().resolvedSteps, 0u);
    EXPECT_EQ (session.Progress ().totalSteps, 8u);
}

TEST (SunStudySession, AGridChangeDiscardsEverything)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (4);

    session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());
    session.Advance (fixture.traversal, 2, 0.001, 0.0, 1);

    session.Sync (Inputs (1, day.Version (), 99), day, fixture.Samples ());
    EXPECT_EQ (session.Progress ().resolvedSteps, 0u);
}

// ⚠️ THE SEATBELT. A caller that forgets to bump gridVersion but hands over a
// different number of samples would otherwise index a stale accumulator.
TEST (SunStudySession, AChangedSampleCountResetsEvenWithoutAVersionBump)
{
    Fixture fixture;
    SunStudySession session;
    const SunSeries day = MakeDay (4);
    const StudyInputs inputs = Inputs (1, day.Version (), 1);

    session.Sync (inputs, day, fixture.Samples ());
    session.Advance (fixture.traversal, 4, 0.001, 0.0, 1);
    ASSERT_TRUE (session.Progress ().converged);

    std::vector<double> more { 0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 20.0, 0.0, 0.0 };
    SampleSet grown;
    grown.positions = more.data ();
    grown.count = 3;

    session.Sync (inputs, day, grown); // same versions, different count
    EXPECT_EQ (session.Progress ().sampleCount, 3u);
    EXPECT_EQ (session.Progress ().resolvedSteps, 0u);
    EXPECT_EQ (session.SunHours ().size (), 3u);
}

// ---------------------------------------------------------------------------
// empty vs converged
// ---------------------------------------------------------------------------

// ⚠️ BOTH PRODUCE ZEROES AND THEY MEAN DIFFERENT THINGS. Calling the empty case
// converged would let a caller publish "0 hours everywhere" for a model that was
// never analysed.
TEST (SunStudySession, AnEmptyStudyIsNotAConvergedOne)
{
    Fixture fixture;
    SunStudySession session;

    // No sun above the horizon.
    std::vector<SunStep> night (1);
    night[0].altitudeDegrees = -20.0;
    night[0].direction[2] = 1.0;
    const SunSeries polarNight = SunSeries::FromSteps (night, 60);
    ASSERT_TRUE (polarNight.Empty ());

    session.Sync (Inputs (1, polarNight.Version (), 1), polarNight, fixture.Samples ());
    EXPECT_TRUE (session.Progress ().empty);
    EXPECT_FALSE (session.Progress ().converged);
    EXPECT_EQ (session.Advance (fixture.traversal, 10, 0.001, 0.0, 1), 0u);

    // No samples.
    SunStudySession other;
    const SunSeries day = MakeDay (3);
    SampleSet none;
    none.count = 0;
    other.Sync (Inputs (1, day.Version (), 1), day, none);
    EXPECT_TRUE (other.Progress ().empty);
    EXPECT_FALSE (other.Progress ().converged);
}

TEST (SunStudySession, AdvancingBeforeSyncIsHarmless)
{
    Fixture fixture;
    SunStudySession session;
    EXPECT_EQ (session.Advance (fixture.traversal, 5, 0.001, 0.0, 1), 0u);
    EXPECT_TRUE (session.Progress ().empty);
    EXPECT_FALSE (session.Progress ().converged);
}

// One-step-per-call and all-at-once must reach the same answer: the slice size
// is the caller's budget and nothing else.
TEST (SunStudySession, SliceSizeDoesNotChangeTheAnswer)
{
    Fixture fixture;
    const SunSeries day = MakeDay (6);
    const StudyInputs inputs = Inputs (1, day.Version (), 1);

    SunStudySession drip;
    drip.Sync (inputs, day, fixture.Samples ());
    while (drip.Advance (fixture.traversal, 1, 0.001, 0.0, 1) > 0) {
    }

    SunStudySession gulp;
    gulp.Sync (inputs, day, fixture.Samples ());
    gulp.Advance (fixture.traversal, 1000, 0.001, 0.0, 1);

    ASSERT_TRUE (drip.Progress ().converged);
    ASSERT_TRUE (gulp.Progress ().converged);
    EXPECT_EQ (drip.Accumulator ().Bits (), gulp.Accumulator ().Bits ());
    EXPECT_EQ (drip.SunHours (), gulp.SunHours ());
}

// ---------------------------------------------------------------------------
// The dispatch guard
// ---------------------------------------------------------------------------

TEST (SunStudySession, AdvancingIsFalseWhenIdle)
{
    Fixture fixture;
    SunStudySession session;
    session.Sync (Inputs (1, 1, 1), MakeDay (4), fixture.Samples ());
    EXPECT_FALSE (session.Advancing ());
    session.Advance (fixture.traversal, 2, 0.001, 0.0, 1);
    EXPECT_FALSE (session.Advancing ());
}

// ⚠️ THE FAILURE THIS PREVENTS DOES NOT CRASH AND DOES NOT CORRUPT A BIT. Two
// threads advancing one session both read `nextStep_`, both claim the same
// timesteps, and the study silently resolves fewer steps than it reports --
// which converges early and publishes an hours figure that is too low.
TEST (SunStudySession, ConcurrentAdvancesDoNotBothClaimTheSameSteps)
{
    Fixture fixture;
    const SunSeries day = MakeDay (24);

    for (int attempt = 0; attempt < 40; ++attempt) {
        SunStudySession session;
        session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());

        std::atomic<size_t> total { 0 };
        std::atomic<bool> go { false };
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back ([&] () {
                while (!go.load (std::memory_order_acquire)) {
                }
                total += session.Advance (fixture.traversal, 24, 0.001, 0.0, 1);
            });
        }
        go.store (true, std::memory_order_release);
        for (std::thread& thread : threads)
            thread.join ();

        // Whoever won resolved every step; the losers were refused with 0. No
        // step is ever counted twice.
        EXPECT_EQ (total.load (), 24u);
        EXPECT_TRUE (session.Progress ().converged);
        EXPECT_EQ (session.Progress ().resolvedSteps, 24u);
    }
}

// A refusal is not a failure: the loser learns "nothing to do", which is what a
// frame loop that lost the race to a graph evaluation should conclude.
TEST (SunStudySession, TheGuardReleasesSoLaterCallsSucceed)
{
    Fixture fixture;
    const SunSeries day = MakeDay (6);
    SunStudySession session;
    session.Sync (Inputs (1, day.Version (), 1), day, fixture.Samples ());

    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 2u);
    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 2u);
    EXPECT_EQ (session.Advance (fixture.traversal, 2, 0.001, 0.0, 1), 2u);
    EXPECT_TRUE (session.Progress ().converged);
}
