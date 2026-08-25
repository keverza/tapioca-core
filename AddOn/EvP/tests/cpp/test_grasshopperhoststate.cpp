// Grasshopper/HostState.cpp — the Rhino.Inside host's lifecycle rules.
//
// This is the one part of PLAT-RHINO-INSIDE slice 0 that can be proved without
// Archicad, without Rhino and without a CLR, and it is also the part whose
// failures are the most expensive: every rule here exists because breaking it
// means a SECOND RhinoCore in the process, a host that reports Running after a
// failed start, or a managed callback arriving in an add-on that is already
// unloading. None of those three announces itself — the first two look like a
// slow start, and the third is an access violation on Archicad's quit path with
// a stack in someone else's runtime.
//
// So the live probe gets to spend its attention on what only a live run can
// answer (does RhinoCore construct, does Grasshopper load, does Archicad stay
// responsive) rather than on re-deriving state transitions by clicking a menu.

#include "Grasshopper/HostState.hpp"

#include <gtest/gtest.h>

#include <atomic>
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
    EXPECT_FALSE (lifecycle.AcceptsCallbacks ());

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Starting, lifecycle.State ());

    // ⚠️ NOT YET. A half-built host must not accept callbacks: the managed side
    // exists but the native side has not finished recording what it owns.
    EXPECT_FALSE (lifecycle.AcceptsCallbacks ());

    lifecycle.CompleteStart ();
    EXPECT_EQ (HostState::Running, lifecycle.State ());
    EXPECT_TRUE (lifecycle.IsRunning ());
    EXPECT_TRUE (lifecycle.AcceptsCallbacks ());
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
    lifecycle.CompleteStart ();

    // The menu command's ordinary second click. It must be told "already
    // running" and NOT be handed the right to build another core.
    EXPECT_EQ (StartDecision::AlreadyRunning, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Running, lifecycle.State ());
}

TEST (GrasshopperHostState, FailedStartIsNotRunningAndMayBeRetried)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    lifecycle.FailStart ("no Rhino installation");

    EXPECT_EQ (HostState::Failed, lifecycle.State ());
    EXPECT_FALSE (lifecycle.IsRunning ());
    EXPECT_FALSE (lifecycle.AcceptsCallbacks ());
    EXPECT_EQ ("no Rhino installation", lifecycle.LastError ());

    // A start that never produced a core left nothing to collide with, so the
    // user who installs Rhino and clicks again gets a real attempt.
    EXPECT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    EXPECT_TRUE (lifecycle.LastError ().empty ());
}

TEST (GrasshopperHostState, CallbacksAreRefusedFromTheMomentAStopBegins)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    lifecycle.CompleteStart ();
    ASSERT_TRUE (lifecycle.AcceptsCallbacks ());

    ASSERT_TRUE (lifecycle.BeginStop ());
    // The whole point: not after CompleteStop, at BeginStop. Teardown is where
    // a late callback lands, and it starts here.
    EXPECT_FALSE (lifecycle.AcceptsCallbacks ());
    EXPECT_EQ (HostState::Stopping, lifecycle.State ());

    lifecycle.CompleteStop ();
    EXPECT_EQ (HostState::Stopped, lifecycle.State ());
    EXPECT_FALSE (lifecycle.AcceptsCallbacks ());
}

TEST (GrasshopperHostState, StopIsSafeWhenNothingIsRunning)
{
    HostLifecycle lifecycle;
    // FreeData calls Stop unconditionally, including on a session where the
    // menu item was never touched.
    EXPECT_FALSE (lifecycle.BeginStop ());
    EXPECT_EQ (HostState::NotStarted, lifecycle.State ());

    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    lifecycle.FailStart ("runtime missing");
    EXPECT_FALSE (lifecycle.BeginStop ());
    EXPECT_EQ (HostState::Failed, lifecycle.State ());
}

TEST (GrasshopperHostState, RestartAfterStopIsRefused)
{
    HostLifecycle lifecycle;
    ASSERT_EQ (StartDecision::Proceed, lifecycle.BeginStart ());
    lifecycle.CompleteStart ();
    ASSERT_TRUE (lifecycle.BeginStop ());
    lifecycle.CompleteStop ();

    // Same-process RhinoCore reconstruction is unmeasured, and the handoff says
    // an Archicad restart is the fallback. Encoded here rather than left to the
    // caller, because the caller is a menu item a user will click twice.
    EXPECT_EQ (StartDecision::Terminal, lifecycle.BeginStart ());
    EXPECT_EQ (HostState::Stopped, lifecycle.State ());
    EXPECT_EQ (StartDecision::Terminal, lifecycle.BeginStart ());
}

TEST (GrasshopperHostState, ExactlyOneOfManyConcurrentStartsProceeds)
{
    // The reason this class holds a mutex rather than a bool. Two threads can
    // reach the host: the menu command, and (later) any worker that goes
    // through the gate. Two Proceeds means two RhinoCores.
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

TEST (GrasshopperHostState, EveryStateHasAName)
{
    // Describe feeds the log and the user-facing report; an unnamed state would
    // surface as "unknown" in exactly the diagnostic someone is reading.
    const HostState states[] = { HostState::NotStarted, HostState::Starting, HostState::Running,
                                 HostState::Stopping,   HostState::Stopped,  HostState::Failed };
    for (size_t index = 0; index < sizeof (states) / sizeof (states[0]); ++index)
        EXPECT_STRNE ("unknown", evp::grasshopper::DescribeHostState (states[index]));
}
