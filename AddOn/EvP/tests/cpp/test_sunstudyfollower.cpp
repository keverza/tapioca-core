// Tests for SunStudy/SunStudyFollower — staleness, debounce and which result is
// allowed to become visible.
//
// ⚠️ EVERY CASE HERE IS A BELIEVABLE WRONG PICTURE. A study shown after the model
// moved is a heat map of a building that no longer exists; a study restarted on
// every notification of one edit burns the machine for nothing; a result that
// completes one snapshot late and is accepted paints the old building's sunlight
// onto the new one. None of them looks like an error in a viewport, and all of
// them are decided by TIME and IDENTITY — which is exactly what a live session
// cannot reproduce on demand. That is why the whole rule is a pure state machine
// over a caller-supplied clock.

#include <string>

#include "SunStudy/SunStudyFollower.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

SunStudyDependencySignature Sig (uint64_t geometry, uint64_t sun = 7, uint64_t sampling = 11)
{
    SunStudyDependencySignature s;
    s.geometry = geometry;
    s.sun = sun;
    s.sampling = sampling;
    return s;
}

// A follower with a study already on screen, computed against snapshot 41.
SunStudyFollower Adopted (int64_t now = 1000)
{
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), now);
    return follower;
}

} // namespace

// ---------------------------------------------------------------------------
// what must NOT make a study stale
// ---------------------------------------------------------------------------

TEST (SunStudyFollower, NavigationLeavesTheStudyCurrent)
{
    // ⚠️ THE REGRESSION THIS FILE EXISTS FOR. Archicad raises its environment
    // notification on every orbit, and the viewer already paid once for treating
    // that as a model change -- 37 full re-extractions in 100 seconds. The study
    // must not learn the same habit: the dependency signature has no camera,
    // projection, viewport or selection in it, so observing the same world a
    // hundred times changes nothing.
    SunStudyFollower follower = Adopted ();
    for (int tick = 0; tick < 100; ++tick) {
        EXPECT_FALSE (follower.Observe (Sig (41), 1000 + tick * 50));
        EXPECT_TRUE (follower.IsFullyCurrent ());
    }
    EXPECT_EQ (follower.State (), SunStudyFollowState::Current);
    EXPECT_EQ (follower.Reruns (), 0u);
    EXPECT_FALSE (follower.ShouldStart (1000 + 100000));
}

TEST (SunStudyFollower, AnUndoneEditBecomesCurrentAgainWithoutRecomputing)
{
    SunStudyFollower follower = Adopted ();
    EXPECT_TRUE (follower.Observe (Sig (42), 1100));
    EXPECT_FALSE (follower.IsFullyCurrent ());

    // ⚠️ COMPARED AGAINST THE STUDY'S OWN SIGNATURE, NOT THE LAST OBSERVATION.
    // An element moved and undone puts the world back where the study measured
    // it; recomputing would be work for an identical answer, and comparing
    // against the previous observation would report it stale forever.
    EXPECT_TRUE (follower.Observe (Sig (41), 1200));
    EXPECT_EQ (follower.State (), SunStudyFollowState::Current);
    EXPECT_TRUE (follower.IsFullyCurrent ());
    EXPECT_EQ (follower.Reruns (), 0u);
}

// ---------------------------------------------------------------------------
// what must
// ---------------------------------------------------------------------------

TEST (SunStudyFollower, GeometrySunAndSamplingEachMakeItStaleAndSayWhich)
{
    {
        SunStudyFollower follower = Adopted ();
        EXPECT_TRUE (follower.Observe (Sig (42), 1100));
        EXPECT_EQ (follower.DirtyReason (), SunStudyDirtyReason::Geometry);
    }
    {
        SunStudyFollower follower = Adopted ();
        EXPECT_TRUE (follower.Observe (Sig (41, 8), 1100));
        EXPECT_EQ (follower.DirtyReason (), SunStudyDirtyReason::Sun);
    }
    {
        SunStudyFollower follower = Adopted ();
        EXPECT_TRUE (follower.Observe (Sig (41, 7, 12), 1100));
        EXPECT_EQ (follower.DirtyReason (), SunStudyDirtyReason::Sampling);
    }
}

TEST (SunStudyFollower, StalenessIsImmediateButTheCacheKeepsDrawing)
{
    // ⚠️ THE OVERLAY MUST NOT VANISH BECAUSE ONE WALL MOVED. Blanking a whole
    // building's analysis to express "part of this is stale" throws away the
    // correct majority of it at the moment the user is reading it. Staleness is
    // a property of a REGION. So the study stops being FULLY CURRENT at once,
    // and goes on being RENDERABLE throughout.
    SunStudyFollower follower = Adopted ();
    follower.Observe (Sig (42), 1100);

    EXPECT_FALSE (follower.IsFullyCurrent ()); // at once
    EXPECT_TRUE (follower.HasRenderableCache ());
    EXPECT_TRUE (follower.HasDirtyRegions ());
    EXPECT_FALSE (follower.IsComputing ());
    EXPECT_TRUE (follower.IsDirty ());
    EXPECT_FALSE (follower.ShouldStart (1100));                           // not yet
    EXPECT_FALSE (follower.ShouldStart (1100 + kSunStudyDebounceMs - 1)); // still not
    EXPECT_TRUE (follower.ShouldStart (1100 + kSunStudyDebounceMs));      // now
}

// ---------------------------------------------------------------------------
// the debounce
// ---------------------------------------------------------------------------

TEST (SunStudyFollower, TwentyNotificationsInOneEditBurstProduceOneRun)
{
    // ⚠️ A QUIET PERIOD, NOT A RATE LIMIT. One Archicad user action produces
    // several notifications, and dragging an element produces a stream; a rate
    // limit would start a study in the middle of the drag and another when it
    // ended. The timer RESETS on every relevant change.
    SunStudyFollower follower = Adopted ();

    int64_t now = 1000;
    for (int edit = 0; edit < 20; ++edit) {
        now += 50; // faster than the debounce
        follower.Observe (Sig (42 + uint64_t (edit)), now);
        EXPECT_FALSE (follower.ShouldStart (now)) << "started mid-burst at edit " << edit;
    }

    // Quiet.
    EXPECT_FALSE (follower.ShouldStart (now + kSunStudyDebounceMs - 1));
    EXPECT_TRUE (follower.ShouldStart (now + kSunStudyDebounceMs));

    follower.NoteStarted (Sig (61), now + kSunStudyDebounceMs);
    EXPECT_EQ (follower.Reruns (), 1u);
}

TEST (SunStudyFollower, RepeatedObservationsOfTheSameWorldDoNotPostponeTheStart)
{
    // ⚠️ THE REGRESSION FOR A LIVE DEADLOCK, AND THE SHAPE OF THE TEST IS THE
    // POINT. The driver POLLS: it calls Observe on every 200 ms tick whether
    // anything happened or not. An Observe that restarted the quiet period on
    // every call -- rather than only when the world had actually moved again --
    // pushed the deadline out five times a second for ever. ShouldStart never
    // became true, the study sat in DirtyVisible indefinitely, and the overlay
    // stayed up showing stale values that were never replaced.
    //
    // Every other test in this file called Observe only when the signature HAD
    // changed, which is precisely the case where the broken and the correct
    // behaviour agree. That is why they all passed while the live run hung.
    SunStudyFollower follower = Adopted ();

    follower.Observe (Sig (42), 1000); // the edit
    ASSERT_TRUE (follower.HasDirtyRegions ());

    // Now poll, as the driver does, with nothing changing.
    for (int64_t tick = 1; tick <= 10; ++tick)
        follower.Observe (Sig (42), 1000 + tick * 50);

    // 500 ms of quiet have passed; the replacement is overdue, not postponed.
    EXPECT_TRUE (follower.ShouldStart (1500));
    EXPECT_EQ (follower.MillisecondsUntilStart (1500), 0);
}

TEST (SunStudyFollower, APollingCallerStillGetsTheBurstBehaviour)
{
    // The other half of the same rule: polls must not postpone, but real changes
    // must. A drag produces both, interleaved.
    SunStudyFollower follower = Adopted ();

    int64_t now = 1000;
    for (int edit = 0; edit < 5; ++edit) {
        now += 50;
        follower.Observe (Sig (42 + uint64_t (edit)), now);      // a real change
        follower.Observe (Sig (42 + uint64_t (edit)), now + 10); // and a poll
        follower.Observe (Sig (42 + uint64_t (edit)), now + 20); // and another
        EXPECT_FALSE (follower.ShouldStart (now + 20)) << "started mid-drag at edit " << edit;
    }

    // The last real change was at `now`; the polls after it must not have moved
    // the deadline.
    EXPECT_FALSE (follower.ShouldStart (now + kSunStudyDebounceMs - 1));
    EXPECT_TRUE (follower.ShouldStart (now + kSunStudyDebounceMs));
}

// ---------------------------------------------------------------------------
// the whole loop, driven the way the driver drives it
//
// ⚠️ THE TWO TESTS BELOW ARE THE ONES THAT WOULD HAVE CAUGHT BOTH LIVE
// FAULTS. Every other test in this file calls Observe only at the interesting
// moments; the driver calls it on a 200 ms TIMER, whether anything happened or
// not. That difference hid a deadlock (the countdown restarted for ever, no run
// ever started) and then a runaway (every tick superseded the run in flight, so
// 100 studies started in 62 seconds and none advanced a single slice). Both look
// identical from the interesting moments alone.
// ---------------------------------------------------------------------------

namespace {

// One tick of SunStudyFollowerDriver::Tick, with nothing left out.
void DriverTick (SunStudyFollower& follower, const SunStudyDependencySignature& world, int64_t now,
                 uint64_t& generation, int& startsOut, int& slicesOut)
{
    follower.Observe (world, now);
    switch (follower.State ()) {
        case SunStudyFollowState::DirtyVisible:
            if (follower.ShouldStart (now)) {
                generation = follower.NoteStarted (world, now);
                ++startsOut;
            }
            break;
        case SunStudyFollowState::UpdatingVisible:
            ++slicesOut;
            break;
        default:
            break;
    }
}

} // namespace

TEST (SunStudyFollower, AnEditDrivenAtTheTimerRateStartsExactlyOneRunAndLetsItAdvance)
{
    // ⚠️ THE LIVE RUNAWAY, PINNED. Before the run target existed, Observe
    // compared the world against the STUDY -- which a replacement is by
    // definition not equal to yet -- so every tick after the start found the
    // model stale again, bumped the generation and threw the run away. The
    // counters below are the whole assertion: one start, many slices.
    SunStudyFollower follower = Adopted ();

    int starts = 0;
    int slices = 0;
    uint64_t generation = 0;
    const SunStudyDependencySignature edited = Sig (42);

    // Five seconds of the driver's 200 ms timer over a model that changed once.
    for (int64_t now = 1000; now <= 6000; now += 200)
        DriverTick (follower, edited, now, generation, starts, slices);

    EXPECT_EQ (starts, 1) << "the follower superseded its own replacement";
    EXPECT_GT (slices, 20) << "the run never got to advance";
    EXPECT_EQ (follower.State (), SunStudyFollowState::UpdatingVisible);
    // And the result it eventually produces is still acceptable.
    EXPECT_TRUE (follower.NoteCompleted (generation, "study-b", edited, 6200));
    EXPECT_TRUE (follower.IsFullyCurrent ());
}

TEST (SunStudyFollower, ASecondEditDuringTheRunStillSupersedesIt)
{
    // ⚠️ THE PROPERTY THE FIX MUST NOT COST. Judging observations against the
    // run's target rather than the study's makes an UNCHANGED world agreement --
    // it must not make a CHANGED one agreement too, or a result computed for a
    // building that has since moved would be displayed on the new one.
    SunStudyFollower follower = Adopted ();

    int starts = 0;
    int slices = 0;
    uint64_t generation = 0;

    for (int64_t now = 1000; now <= 2000; now += 200)
        DriverTick (follower, Sig (42), now, generation, starts, slices);
    ASSERT_EQ (starts, 1);
    const uint64_t firstRun = generation;

    // The model moves again while that run is in flight.
    for (int64_t now = 2200; now <= 3400; now += 200)
        DriverTick (follower, Sig (43), now, generation, starts, slices);

    EXPECT_EQ (starts, 2) << "the second edit did not produce a replacement";
    EXPECT_NE (generation, firstRun) << "the superseded run kept its generation";
    EXPECT_FALSE (follower.NoteCompleted (firstRun, "study-b", Sig (42), 3500))
        << "a result for the pre-move building was accepted";
}

// ---------------------------------------------------------------------------
// a result that arrives too late
// ---------------------------------------------------------------------------

TEST (SunStudyFollower, AResultWhoseWorldMovedWhileItRanIsDiscarded)
{
    // The case the task calls out by name: study A starts on snapshot 41, the
    // model becomes 42, A completes afterwards. A must never become visible.
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), 1000);
    follower.Observe (Sig (42), 1100);
    const uint64_t generation = follower.NoteStarted (Sig (42), 1400);
    EXPECT_EQ (follower.State (), SunStudyFollowState::UpdatingVisible);
    // ⚠️ STILL DRAWING WHILE IT RECOMPUTES. The stale cache is what the user
    // keeps looking at until its replacement lands, patch by patch.
    EXPECT_TRUE (follower.HasRenderableCache ());
    EXPECT_TRUE (follower.IsComputing ());

    // The model moves again WHILE it computes.
    follower.Observe (Sig (43), 1450);

    EXPECT_FALSE (follower.NoteCompleted (generation, "study-b", Sig (42), 1500));
    EXPECT_FALSE (follower.IsFullyCurrent ());
    EXPECT_EQ (follower.DiscardedResults (), 1u);
    // ⚠️ AND IT IS DIRTY, NOT FAILED. Nothing is broken -- the world moved -- so
    // the next quiet period must start another run, or the overlay never returns.
    EXPECT_EQ (follower.State (), SunStudyFollowState::DirtyVisible);
    EXPECT_TRUE (follower.ShouldStart (1450 + kSunStudyDebounceMs));
}

TEST (SunStudyFollower, TheRunForTheNewestWorldIsTheOneThatBecomesCurrent)
{
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), 1000);
    follower.Observe (Sig (42), 1100);
    const uint64_t stale = follower.NoteStarted (Sig (42), 1400);

    follower.Observe (Sig (43), 1450);
    const uint64_t fresh = follower.NoteStarted (Sig (43), 1800);
    EXPECT_NE (stale, fresh);

    // The superseded run lands first, as it would in a real race.
    EXPECT_FALSE (follower.NoteCompleted (stale, "study-b", Sig (42), 1900));
    EXPECT_FALSE (follower.IsFullyCurrent ());

    EXPECT_TRUE (follower.NoteCompleted (fresh, "study-c", Sig (43), 1950));
    EXPECT_TRUE (follower.IsFullyCurrent ());
    EXPECT_EQ (follower.StudyId (), "study-c");
    EXPECT_EQ (follower.StudySignature ().geometry, 43u);
}

TEST (SunStudyFollower, AResultComputedAgainstTheWrongInputsIsRefusedEvenAtTheRightGeneration)
{
    // ⚠️ THE SECOND CHECK, AND THE GENERATION ALONE WOULD NOT CATCH IT. A caller
    // that starts a run with stale parameters produces a result nothing has
    // superseded and that still does not describe the world.
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), 1000);
    follower.Observe (Sig (42), 1100);
    const uint64_t generation = follower.NoteStarted (Sig (42), 1400);

    EXPECT_FALSE (follower.NoteCompleted (generation, "study-b", Sig (41), 1500));
    EXPECT_FALSE (follower.IsFullyCurrent ());
    EXPECT_EQ (follower.DiscardedResults (), 1u);
}

TEST (SunStudyFollower, AFailureFromASupersededRunDoesNotStopTheCurrentOne)
{
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), 1000);
    follower.Observe (Sig (42), 1100);
    const uint64_t stale = follower.NoteStarted (Sig (42), 1400);
    follower.Observe (Sig (43), 1450);

    follower.NoteFailed (stale, 1500);
    EXPECT_NE (follower.State (), SunStudyFollowState::Failed);
    EXPECT_TRUE (follower.ShouldStart (1450 + kSunStudyDebounceMs));
}

TEST (SunStudyFollower, AFailureAtTheCurrentGenerationHoldsRatherThanSpinning)
{
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), 1000);
    follower.Observe (Sig (42), 1100);
    const uint64_t generation = follower.NoteStarted (Sig (42), 1400);

    follower.NoteFailed (generation, 1500);
    EXPECT_EQ (follower.State (), SunStudyFollowState::Failed);
    // ⚠️ A BROKEN STUDY MUST NOT RETRY FOREVER. Failed is not Dirty, so the quiet
    // timer does not fire; the world moving again is what gives it another go.
    EXPECT_FALSE (follower.ShouldStart (1500 + kSunStudyDebounceMs * 10));
    follower.Observe (Sig (44), 2000);
    EXPECT_TRUE (follower.ShouldStart (2000 + kSunStudyDebounceMs));
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

TEST (SunStudyFollower, ClearStopsDisplayAndOrphansAnythingInFlight)
{
    SunStudyFollower follower;
    follower.Adopt ("study-a", Sig (41), 1000);
    follower.Observe (Sig (42), 1100);
    const uint64_t generation = follower.NoteStarted (Sig (42), 1400);

    follower.Clear ();
    EXPECT_EQ (follower.State (), SunStudyFollowState::NoStudy);
    EXPECT_FALSE (follower.IsFullyCurrent ());
    EXPECT_FALSE (follower.IsDirty ());
    // ⚠️ THE ONLY STATE THAT STOPS THE OVERLAY. Clear is one of the four cases
    // where there is genuinely nothing to draw.
    EXPECT_FALSE (follower.HasRenderableCache ());
    // ⚠️ THE GENERATION DOES NOT RESET, so a run started before the clear can
    // never be mistaken for one started after it.
    EXPECT_FALSE (follower.NoteCompleted (generation, "study-b", Sig (42), 1500));
}

TEST (SunStudyFollower, ObservingWithNoStudyRecordsTheWorldWithoutInventingStaleness)
{
    SunStudyFollower follower;
    EXPECT_FALSE (follower.Observe (Sig (41), 1000));
    EXPECT_EQ (follower.State (), SunStudyFollowState::NoStudy);
    EXPECT_FALSE (follower.IsDirty ());
    EXPECT_FALSE (follower.ShouldStart (1000 + kSunStudyDebounceMs * 10));
}

TEST (SunStudyFollower, TheCountdownIsReportedRatherThanProbedFor)
{
    SunStudyFollower follower = Adopted ();
    // Nothing waiting.
    EXPECT_EQ (follower.MillisecondsUntilStart (1000), -1);

    follower.Observe (Sig (42), 1100);
    EXPECT_EQ (follower.MillisecondsUntilStart (1100), kSunStudyDebounceMs);
    EXPECT_EQ (follower.MillisecondsUntilStart (1100 + 100), kSunStudyDebounceMs - 100);
    EXPECT_EQ (follower.MillisecondsUntilStart (1100 + kSunStudyDebounceMs), 0);
    // ⚠️ IT NEVER GOES NEGATIVE. A diagnostic that counted down past zero would
    // read as a study that should have started and did not.
    EXPECT_EQ (follower.MillisecondsUntilStart (1100 + kSunStudyDebounceMs * 5), 0);

    // And a burst pushes it back out, in step with ShouldStart.
    follower.Observe (Sig (43), 1300);
    EXPECT_EQ (follower.MillisecondsUntilStart (1300), kSunStudyDebounceMs);
}

TEST (SunStudyFollower, TheDescriptionNamesBothSnapshots)
{
    // "drawing=false" sends a reader nowhere. The numbers say which way to look.
    SunStudyFollower follower = Adopted ();
    follower.Observe (Sig (42), 1100);
    const std::string text = follower.Describe ();
    EXPECT_NE (text.find ("42"), std::string::npos);
    EXPECT_NE (text.find ("41"), std::string::npos);
    EXPECT_NE (text.find ("geometry"), std::string::npos);
}
