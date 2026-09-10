// Grasshopper/HostState.cpp — the Grasshopper worker's lifecycle rules.
//
// This is the one part of PLAT-RHINO-INSIDE P0 that can be proved without
// Archicad, without a worker process and without Rhino, and it is also the part
// whose failures are the most expensive: every rule here exists because breaking
// it means a SECOND worker process, a host that reports Running after a failed
// start, or a worker message served by an add-on that is already unloading. None
// of those three announces itself — the first two look like a slow start, and
// the third is an access violation on Archicad's quit path.
//
// So a live run gets to spend its attention on what only a live run can answer
// (does the worker spawn, does Rhino start in it, does Archicad stay responsive
// while it solves) rather than on re-deriving state transitions by clicking a
// menu.

#include "Grasshopper/HostState.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using evp::grasshopper::HostLifecycle;
using evp::grasshopper::HostState;
using evp::grasshopper::StartDecision;

TEST (GrasshopperHostState, StartsFromNotStarted)
{
    HostLifecycle lifecycle;
    EXPECT_EQ (HostState::NotStarted, lifecycle.State ());
    EXPECT_FALSE (lifecycle.IsRunning ());
    EXPECT_FALSE (lifecycle.AcceptsMessages ());

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Starting, lifecycle.State ());

    // ⚠️ NOT YET. A half-built host must not accept worker messages: the process
    // may already be up and talking, but the native side has not finished
    // recording what it owns.
    EXPECT_FALSE (lifecycle.AcceptsMessages ());

    ASSERT_TRUE (lifecycle.CompleteStart (1));
    EXPECT_EQ (HostState::Running, lifecycle.State ());
    EXPECT_TRUE (lifecycle.IsRunning ());
    EXPECT_TRUE (lifecycle.AcceptsMessages ());
}

TEST (GrasshopperHostState, SecondStartWhileStartingIsRefused)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (StartDecision::InProgress, lifecycle.BeginStart ());
    EXPECT_EQ (StartDecision::InProgress, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Starting, lifecycle.State ());
}

TEST (GrasshopperHostState, StartIsIdempotentOnceRunning)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    ASSERT_TRUE (lifecycle.CompleteStart (1));

    // The menu command's ordinary second click. It must be told "already
    // running" and NOT be handed the right to spawn another worker.
    EXPECT_EQ (StartDecision::AlreadyRunning, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Running, lifecycle.State ());
}

TEST (GrasshopperHostState, FailedStartIsNotRunningAndMayBeRetried)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    ASSERT_TRUE (lifecycle.Fail (1, "no Rhino installation"));

    EXPECT_EQ (HostState::Failed, lifecycle.State ());
    EXPECT_FALSE (lifecycle.IsRunning ());
    EXPECT_FALSE (lifecycle.AcceptsMessages ());
    EXPECT_EQ ("no Rhino installation", lifecycle.LastError ());

    // A start that never produced a worker left nothing to collide with, so the
    // user who installs Rhino and clicks again gets a real attempt.
    EXPECT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_TRUE (lifecycle.LastError ().empty ());
}

TEST (GrasshopperHostState, WorkerMessagesAreRefusedFromTheMomentAStopBegins)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    ASSERT_TRUE (lifecycle.CompleteStart (1));
    ASSERT_TRUE (lifecycle.AcceptsMessages ());

    ASSERT_TRUE (lifecycle.BeginStop ());
    // The whole point: not after CompleteStop, at BeginStop. Teardown is where
    // a late callback lands, and it starts here.
    EXPECT_FALSE (lifecycle.AcceptsMessages ());
    EXPECT_EQ (HostState::Stopping, lifecycle.State ());

    lifecycle.CompleteStop ();
    EXPECT_EQ (HostState::Stopped, lifecycle.State ());
    EXPECT_FALSE (lifecycle.AcceptsMessages ());
}

TEST (GrasshopperHostState, StopIsSafeWhenNothingIsRunning)
{
    HostLifecycle lifecycle;
    // FreeData calls Stop unconditionally, including on a session where the
    // menu item was never touched.
    EXPECT_FALSE (lifecycle.BeginStop ());
    EXPECT_EQ (HostState::NotStarted, lifecycle.State ());

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    ASSERT_TRUE (lifecycle.Fail (1, "runtime missing"));
    EXPECT_FALSE (lifecycle.BeginStop ());
    EXPECT_EQ (HostState::Failed, lifecycle.State ());
}

TEST (GrasshopperHostState, RestartAfterStopIsAllowed)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    ASSERT_TRUE (lifecycle.CompleteStart (1));
    ASSERT_TRUE (lifecycle.BeginStop ());
    lifecycle.CompleteStop ();

    // ⚠️ THIS IS THE TEST THAT REVERSED WITH THE PROCESS BOUNDARY, AND IT IS THE
    // WHOLE POINT OF IT. In process a stop was TERMINAL: hostfxr_close does not
    // unload a CLR, RhinoCore could not be reconstructed, and the only honest
    // answer to a second click was "restart Archicad". Out of process every one
    // of those constraints belongs to the worker, which is expendable — killing
    // it and spawning another is the recovery primitive the whole design is for.
    EXPECT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Starting, lifecycle.State ());
}

TEST (GrasshopperHostState, EachStartGetsItsOwnGeneration)
{
    // The generation is what tells "the worker died and came back" apart from
    // "the worker never died" in a log that carries both halves of a session.
    HostLifecycle lifecycle;
    EXPECT_EQ (0u, lifecycle.Generation ());

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (1u, lifecycle.Generation ());
    ASSERT_TRUE (lifecycle.CompleteStart (1));

    // A start that is refused must NOT consume a generation: a second menu click
    // on a running worker is not a new worker.
    EXPECT_EQ (StartDecision::AlreadyRunning, lifecycle.BeginStart ());
    EXPECT_EQ (1u, lifecycle.Generation ());

    ASSERT_TRUE (lifecycle.BeginStop ());
    lifecycle.CompleteStop ();
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (2u, lifecycle.Generation ());

    // Including a start that fails: a worker that died on its way up still owns
    // a distinct generation, or its log lines merge with its successor's.
    ASSERT_TRUE (lifecycle.Fail (2, "worker exited during startup"));
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (3u, lifecycle.Generation ());
}

TEST (GrasshopperHostState, ExactlyOneOfManyConcurrentStartsProceeds)
{
    // The reason this class holds a mutex rather than a bool. Two threads can
    // reach the host: the menu command, and the supervisor that restarts a dead
    // worker. Two Proceeds means two worker processes.
    HostLifecycle lifecycle;
    std::atomic<int> proceeds (0);
    std::vector<std::thread> threads;
    for (int index = 0; index < 16; ++index) {
        threads.push_back (std::thread ([&lifecycle, &proceeds] () {
            if (lifecycle.BeginStart () == StartDecision::Proceed)
                proceeds.fetch_add (1);
        }));
    }
    for (size_t index = 0; index < threads.size (); ++index)
        threads[index].join ();

    EXPECT_EQ (1, proceeds.load ());
    EXPECT_EQ (HostState::Starting, lifecycle.State ());
}

TEST (GrasshopperHostState, StopDuringStartRevokesLateReadyCallback)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    const uint32_t generation = lifecycle.Generation ();

    ASSERT_TRUE (lifecycle.BeginStop ());
    EXPECT_FALSE (lifecycle.AcceptsMessages ());
    EXPECT_FALSE (lifecycle.CompleteStart (generation));
    lifecycle.CompleteStop ();
    EXPECT_EQ (HostState::Stopped, lifecycle.State ());
}

TEST (GrasshopperHostState, WorkerDeathAfterReadyIsARecoverableFailure)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    const uint32_t generation = lifecycle.Generation ();
    ASSERT_TRUE (lifecycle.CompleteStart (generation));

    ASSERT_TRUE (lifecycle.Fail (generation, "worker exited"));
    EXPECT_EQ (HostState::Failed, lifecycle.State ());
    EXPECT_FALSE (lifecycle.AcceptsMessages ());
    EXPECT_EQ ("worker exited", lifecycle.LastError ());

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (generation + 1, lifecycle.Generation ());
}

TEST (GrasshopperHostState, OldGenerationCannotAffectReplacement)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    const uint32_t oldGeneration = lifecycle.Generation ();
    ASSERT_TRUE (lifecycle.Fail (oldGeneration, "first worker exited"));

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    const uint32_t newGeneration = lifecycle.Generation ();
    ASSERT_NE (oldGeneration, newGeneration);

    EXPECT_FALSE (lifecycle.CompleteStart (oldGeneration));
    EXPECT_FALSE (lifecycle.Fail (oldGeneration, "late disconnect"));
    EXPECT_EQ (HostState::Starting, lifecycle.State ());
    EXPECT_TRUE (lifecycle.LastError ().empty ());
    EXPECT_TRUE (lifecycle.CompleteStart (newGeneration));
    EXPECT_EQ (HostState::Running, lifecycle.State ());
}

TEST (GrasshopperHostState, EveryStateHasAName)
{
    // Describe feeds the log and the user-facing report; an unnamed state would
    // surface as "unknown" in exactly the diagnostic someone is reading.
    const HostState states[] = { HostState::NotStarted, HostState::Starting, HostState::Running,
                                 HostState::Stopping,   HostState::Stopped,  HostState::Failed };
    for (size_t index = 0; index < sizeof (states) / sizeof (states[0]); ++index)
        EXPECT_STRNE ("unknown", evp::grasshopper::DescribeHostState (states[index]));
}

// ---------------------------------------------------------------------------
// Peer ownership. The host used to conflate "the peer" with "the process I
// started", and every mode past spawn-and-kill needs them apart: the bridge is
// a named-pipe SERVER, so a Grasshopper already running in the user's own Rhino
// can complete the same handshake -- and answering a stop by terminating a
// process would then be killing their Rhino.

TEST (GrasshopperHostState, AStartClaimsSpawnedUnlessItSaysOtherwise)
{
    // Every existing caller passes nothing, and every existing caller spawns.
    evp::grasshopper::HostLifecycle lifecycle;
    ASSERT_EQ (lifecycle.BeginStart (), evp::grasshopper::StartDecision::Proceed);
    EXPECT_EQ (lifecycle.Ownership (), evp::grasshopper::PeerOwnership::Spawned);
    EXPECT_TRUE (lifecycle.OwnsPeerProcess ());
}

TEST (GrasshopperHostState, AnAttachedPeerIsNeverOursToKill)
{
    evp::grasshopper::HostLifecycle lifecycle;
    ASSERT_EQ (lifecycle.BeginStart (evp::grasshopper::PeerOwnership::Attached),
               evp::grasshopper::StartDecision::Proceed);
    EXPECT_EQ (lifecycle.Ownership (), evp::grasshopper::PeerOwnership::Attached);
    // The whole point: the one question the teardown asks.
    EXPECT_FALSE (lifecycle.OwnsPeerProcess ());

    ASSERT_TRUE (lifecycle.CompleteStart (lifecycle.Generation ()));
    EXPECT_FALSE (lifecycle.OwnsPeerProcess ()) << "a handshake does not transfer ownership";
}

TEST (GrasshopperHostState, AttachingAdvancesTheGenerationLikeAnyOtherStart)
{
    // Staleness is about which conversation a message belongs to, not about who
    // started the peer -- so an attached generation must be distinct too.
    evp::grasshopper::HostLifecycle lifecycle;
    ASSERT_EQ (lifecycle.BeginStart (), evp::grasshopper::StartDecision::Proceed);
    const uint32_t spawned = lifecycle.Generation ();
    ASSERT_TRUE (lifecycle.CompleteStart (spawned));
    ASSERT_TRUE (lifecycle.BeginStop ());
    lifecycle.CompleteStop ();

    ASSERT_EQ (lifecycle.BeginStart (evp::grasshopper::PeerOwnership::Attached),
               evp::grasshopper::StartDecision::Proceed);
    EXPECT_NE (lifecycle.Generation (), spawned);
}

TEST (GrasshopperHostState, AStoppedHostOwnsNothing)
{
    evp::grasshopper::HostLifecycle lifecycle;
    ASSERT_EQ (lifecycle.BeginStart (), evp::grasshopper::StartDecision::Proceed);
    ASSERT_TRUE (lifecycle.CompleteStart (lifecycle.Generation ()));
    ASSERT_TRUE (lifecycle.BeginStop ());
    lifecycle.CompleteStop ();

    // Leaving Spawned behind would let a later stop go looking for a process
    // this generation no longer has.
    EXPECT_EQ (lifecycle.Ownership (), evp::grasshopper::PeerOwnership::None);
    EXPECT_FALSE (lifecycle.OwnsPeerProcess ());
}

TEST (GrasshopperHostState, AFailedStartOwnsNothing)
{
    evp::grasshopper::HostLifecycle lifecycle;
    ASSERT_EQ (lifecycle.BeginStart (evp::grasshopper::PeerOwnership::Attached),
               evp::grasshopper::StartDecision::Proceed);
    ASSERT_TRUE (lifecycle.Fail (lifecycle.Generation (), "the bridge would not listen"));

    EXPECT_EQ (lifecycle.Ownership (), evp::grasshopper::PeerOwnership::None);
    EXPECT_FALSE (lifecycle.OwnsPeerProcess ());
}

TEST (GrasshopperHostState, OwnershipIsReadableForAReport)
{
    using evp::grasshopper::DescribePeerOwnership;
    using evp::grasshopper::PeerOwnership;

    EXPECT_STREQ (DescribePeerOwnership (PeerOwnership::None), "none");
    EXPECT_NE (std::string (DescribePeerOwnership (PeerOwnership::Spawned)).find ("spawned"), std::string::npos);
    EXPECT_NE (std::string (DescribePeerOwnership (PeerOwnership::Attached)).find ("attached"), std::string::npos);
}
