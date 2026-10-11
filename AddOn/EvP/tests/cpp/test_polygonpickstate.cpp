// NativeCommands/PolygonPickState — the SDK-free rules of Tapioca.PickPolygon.
//
// Every case here is one a live Archicad session cannot be asked to reproduce on
// demand: a cancel that arrives before the posted pick starts, a physically
// running input that must refuse a new start even across a project close, a
// callback that fires after Reset, a stale model snapshot at any of the three
// phases, a hover normal from a DIFFERENT component or an invalid normal.
//
// KEEP THIS FILE ACAPI-FREE. It drives the state directly, the same way
// SurfaceInspectorCommands.cpp does around the SDK.

#include <gtest/gtest.h>

#include "NativeCommands/PolygonPickState.hpp"

#include <cmath>
#include <limits>

using geomsrv::polygonpick::Abort;
using geomsrv::polygonpick::BeginStatus;
using geomsrv::polygonpick::FacePick;
using geomsrv::polygonpick::InputStart;
using geomsrv::polygonpick::Outcome;
using geomsrv::polygonpick::PolygonPickState;
using geomsrv::polygonpick::Sighting;
using geomsrv::polygonpick::Status;
using geomsrv::polygonpick::Token;
using geomsrv::polygonpick::WorldIdentity;

namespace {

WorldIdentity World (uint64_t captureStamp, uint64_t snapshotId)
{
    WorldIdentity world;
    world.captureStamp = captureStamp;
    world.haveSnapshot = snapshotId != 0;
    world.snapshotId = snapshotId;
    return world;
}

FacePick MakePick (const std::string& guid, int dbElemIdx, int body, unsigned long ipgon)
{
    FacePick pick;
    pick.found = true;
    pick.guid = guid;
    pick.dbElemIdx = dbElemIdx;
    pick.bodyIdx = body;
    pick.ipgon = ipgon;
    pick.pos[0] = 1.0;
    pick.pos[1] = 2.0;
    pick.pos[2] = 3.0;
    pick.neigID = 42;
    pick.neigInIndex = 2;
    pick.neigFlags = 4;
    return pick;
}

Sighting MakeSighting (const std::string& guid, int dbElemIdx, int body, unsigned long ipgon, double nx, double ny,
                       double nz)
{
    Sighting sighting;
    sighting.valid = true;
    sighting.guid = guid;
    sighting.dbElemIdx = dbElemIdx;
    sighting.bodyIdx = body;
    sighting.ipgon = ipgon;
    sighting.normal[0] = nx;
    sighting.normal[1] = ny;
    sighting.normal[2] = nz;
    return sighting;
}

// The full happy path: a hover sighting whose identity MATCHES the committed pick
// exactly and whose normal is finite and non-zero contributes the normal.
TEST (PolygonPickState, matchingCallbackSightingContributesTheNormal)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    ASSERT_EQ (state.Begin ("pick-1", world, token), BeginStatus::Accepted);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);

    state.OnComponent (token, MakeSighting ("GUID-A", 3, 0, 5, 9.0, 0.0, 1.0));
    state.Complete (token, MakePick ("GUID-A", 3, 0, 5), world);

    const Outcome outcome = state.Poll ("pick-1", world);
    ASSERT_EQ (outcome.status, Status::Done);
    EXPECT_TRUE (outcome.pick.found);
    EXPECT_EQ (outcome.pick.guid, "GUID-A");
    EXPECT_EQ (outcome.pick.dbElemIdx, 3);
    EXPECT_EQ (outcome.pick.bodyIdx, 0);
    EXPECT_EQ (outcome.pick.ipgon, 5ul);
    EXPECT_TRUE (outcome.callbackSawComponent);
    EXPECT_TRUE (outcome.callbackMatchedPick);
    EXPECT_TRUE (outcome.pick.haveNormal);
    EXPECT_DOUBLE_EQ (outcome.pick.normal[0], 9.0);
    EXPECT_EQ (outcome.sessionGeneration, token.generation);
    EXPECT_EQ (outcome.expected.snapshotId, 77ull);
}

// A same-GUID sighting from a DIFFERENT body index must not donate its normal.
TEST (PolygonPickState, sameGuidDifferentDbElemIdxNormalIsNotAttached)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-2", world, token);
    state.BeginInput (token, world);
    state.OnComponent (token, MakeSighting ("GUID-A", 1, 0, 5, 1.0, 0.0, 0.0));
    state.Complete (token, MakePick ("GUID-A", 2, 0, 5), world); // committed dbElemIdx 2, sighting 1

    const Outcome outcome = state.Poll ("pick-2", world);
    ASSERT_EQ (outcome.status, Status::Done);
    EXPECT_TRUE (outcome.callbackSawComponent);
    EXPECT_FALSE (outcome.callbackMatchedPick);
    EXPECT_FALSE (outcome.pick.haveNormal);
}

// A stale hover that moved to another polygon must not donate its normal.
TEST (PolygonPickState, staleHoverNormalIsNeverAttachedToAnotherFace)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-3", world, token);
    state.BeginInput (token, world);
    state.OnComponent (token, MakeSighting ("GUID-A", 3, 0, 5, 9.0, 0.0, 0.0));
    state.Complete (token, MakePick ("GUID-A", 3, 0, 9), world); // committed ipgon 9, hovered 5

    const Outcome outcome = state.Poll ("pick-3", world);
    ASSERT_EQ (outcome.status, Status::Done);
    EXPECT_FALSE (outcome.callbackMatchedPick);
    EXPECT_FALSE (outcome.pick.haveNormal);
}

// Zero and non-finite normals are dropped even on an exact identity match.
TEST (PolygonPickState, invalidNormalsAreNotAttached)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);

    Token zero;
    state.Begin ("pick-zero", world, zero);
    state.BeginInput (zero, world);
    state.OnComponent (zero, MakeSighting ("GUID-A", 3, 0, 5, 0.0, 0.0, 0.0));
    state.Complete (zero, MakePick ("GUID-A", 3, 0, 5), world);
    state.EndInput (zero);
    const Outcome zeroOut = state.Poll ("pick-zero", world);
    EXPECT_FALSE (zeroOut.callbackMatchedPick);
    EXPECT_FALSE (zeroOut.pick.haveNormal);

    Token nan;
    ASSERT_EQ (state.Begin ("pick-nan", world, nan), BeginStatus::Accepted);
    ASSERT_EQ (state.BeginInput (nan, world), InputStart::Started);
    state.OnComponent (nan, MakeSighting ("GUID-A", 3, 0, 5, std::numeric_limits<double>::quiet_NaN (), 0.0, 0.0));
    state.Complete (nan, MakePick ("GUID-A", 3, 0, 5), world);
    const Outcome nanOut = state.Poll ("pick-nan", world);
    EXPECT_FALSE (nanOut.callbackMatchedPick);
    EXPECT_FALSE (nanOut.pick.haveNormal);
}

TEST (PolygonPickState, cancellationAfterTheLastAbortCheckStillWinsAtCompletion)
{
    PolygonPickState state;
    const auto world = World (100, 77);
    Token token;
    ASSERT_EQ (state.Begin ("late-cancel", world, token), BeginStatus::Accepted);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);
    ASSERT_EQ (state.ShouldAbort (token), Abort::None);
    Token cancellation;
    ASSERT_TRUE (state.RequestCancel (token.requestId, cancellation));
    state.Complete (token, MakePick ("GUID-A", 3, 0, 5), world);
    state.EndInput (token);
    EXPECT_EQ (state.Poll (token.requestId, world).status, Status::Cancelled);
}

// A click on empty space is a DONE result with found=false, not a failure.
TEST (PolygonPickState, backgroundClickIsADonePickWithNoFace)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-bg", world, token);
    state.BeginInput (token, world);
    FacePick empty;
    empty.found = false;
    state.Complete (token, empty, world);

    const Outcome outcome = state.Poll ("pick-bg", world);
    ASSERT_EQ (outcome.status, Status::Done);
    EXPECT_FALSE (outcome.pick.found);
}

// A queued cancel wins before the pick runs, and a stray Complete cannot revive it.
TEST (PolygonPickState, queuedCancelWinsAndALateCompleteIsIgnored)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    ASSERT_EQ (state.Begin ("pick-4", world, token), BeginStatus::Accepted);

    Token cancelToken;
    EXPECT_TRUE (state.RequestCancel ("pick-4", cancelToken));
    EXPECT_EQ (cancelToken.serial, token.serial);
    EXPECT_EQ (cancelToken.generation, token.generation);

    // The queued start never entered the SDK, and GetPoint is never called.
    EXPECT_EQ (state.BeginInput (token, world), InputStart::Cancelled);
    EXPECT_FALSE (state.InputInFlight ());
    state.Complete (token, MakePick ("GUID-A", 0, 0, 5), world); // ignored

    const Outcome outcome = state.Poll ("pick-4", world);
    ASSERT_EQ (outcome.status, Status::Cancelled);
    EXPECT_TRUE (outcome.cancelRequested);
    EXPECT_FALSE (outcome.haveSdkCode);
}

// The cancel race the old ShouldAbort-then-MarkRunning pair left open: a cancel
// that arrives between the check and the input must still stop GetPoint.
TEST (PolygonPickState, cancelBetweenCheckAndInputStopsTheInput)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-race", world, token);

    Token cancelToken;
    ASSERT_TRUE (state.RequestCancel ("pick-race", cancelToken)); // arrives first
    EXPECT_EQ (state.BeginInput (token, world), InputStart::Cancelled);
    EXPECT_FALSE (state.InputInFlight ()); // so GetPoint is not called
}

// A cancel that arrives while GetPoint is running is reported after it returns.
TEST (PolygonPickState, cancelDuringRunningIsReportedAfterInput)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-run", world, token);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);

    Token cancelToken;
    EXPECT_TRUE (state.RequestCancel ("pick-run", cancelToken)); // Running: flag only
    EXPECT_EQ (state.ShouldAbort (token), Abort::CancelRequested);
    state.EndInput (token);
    state.Cancelled (token, 0, "cancelled while running");

    const Outcome outcome = state.Poll ("pick-run", world);
    ASSERT_EQ (outcome.status, Status::Cancelled);
}

// Begin refuses a second request while one is pending.
TEST (PolygonPickState, beginRefusesWhileARequestIsPending)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token first, second;
    ASSERT_EQ (state.Begin ("pick-A", world, first), BeginStatus::Accepted);
    EXPECT_EQ (state.Begin ("pick-B", world, second), BeginStatus::BusyRequest);
    EXPECT_TRUE (state.IsActive (first));
    EXPECT_FALSE (state.IsActive (second));
}

// Begin refuses while GetPoint is physically in flight, even across a Reset, until
// the input actually returns.
TEST (PolygonPickState, physicalSingleFlightSurvivesReset)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token running;
    ASSERT_EQ (state.Begin ("pick-run", world, running), BeginStatus::Accepted);
    ASSERT_EQ (state.BeginInput (running, world), InputStart::Started);
    EXPECT_TRUE (state.InputInFlight ());
    EXPECT_TRUE (state.IsRunning (running));

    Token blocked;
    EXPECT_EQ (state.Begin ("pick-next", world, blocked), BeginStatus::BusyInput);

    state.Reset (); // project close during the input: flips the generation, not the input
    EXPECT_TRUE (state.InputInFlight ());
    EXPECT_EQ (state.Begin ("pick-next", world, blocked), BeginStatus::BusyInput);

    state.EndInput (running);
    EXPECT_FALSE (state.InputInFlight ());
    EXPECT_EQ (state.Begin ("pick-next", world, blocked), BeginStatus::Accepted);
}

// A callback after Reset must not mutate the next request.
TEST (PolygonPickState, callbackAfterResetIsIgnored)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-old", world, token);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);

    state.Reset ();
    state.OnComponent (token, MakeSighting ("GUID-A", 0, 0, 5, 1.0, 0.0, 0.0)); // must be dropped
    state.EndInput (token);

    Token fresh;
    ASSERT_EQ (state.Begin ("pick-new", world, fresh), BeginStatus::Accepted);
    EXPECT_FALSE (state.Peek ("pick-new").callbackSawComponent);
}

// Reset invalidates the in-flight token for the post-input checks.
TEST (PolygonPickState, resetInvalidatesInFlightToken)
{
    PolygonPickState state;
    const uint64_t before = state.Generation ();
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-7", world, token);
    state.BeginInput (token, world);

    state.Reset ();

    EXPECT_GT (state.Generation (), before);
    EXPECT_EQ (state.ShouldAbort (token), Abort::LifecycleChanged);
    EXPECT_FALSE (state.IsActive (token));
}

// Stale model/snapshot at the QUEUED phase: BeginInput fails before GetPoint.
TEST (PolygonPickState, staleModelAtQueuedPhaseFailsBeforeInput)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-queued", world, token);
    ASSERT_EQ (state.BeginInput (token, World (101, 77)), InputStart::StaleModel);
    EXPECT_FALSE (state.InputInFlight ());

    const Outcome outcome = state.Poll ("pick-queued", World (101, 77));
    ASSERT_EQ (outcome.status, Status::Failed);
    EXPECT_FALSE (outcome.haveSdkCode);
    EXPECT_NE (outcome.reason.find ("changed"), std::string::npos);
}

// Stale model/snapshot at the INPUT phase: Complete refuses a Done result.
TEST (PolygonPickState, staleModelAtInputPhaseFailsThePick)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-input", world, token);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);
    state.Complete (token, MakePick ("GUID-A", 0, 0, 5), World (102, 77));

    const Outcome outcome = state.Poll ("pick-input", World (102, 77));
    ASSERT_EQ (outcome.status, Status::Failed);
    EXPECT_NE (outcome.reason.find ("changed"), std::string::npos);
}

// Stale model/snapshot at the POLL phase: a Done result goes out as Failed.
TEST (PolygonPickState, staleModelAtPollPhaseTurnsDoneIntoFailed)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-poll", world, token);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);
    state.Complete (token, MakePick ("GUID-A", 0, 0, 5), world);
    ASSERT_EQ (state.Peek ("pick-poll").status, Status::Done);

    const Outcome outcome = state.Poll ("pick-poll", World (103, 77));
    ASSERT_EQ (outcome.status, Status::Failed);
    EXPECT_NE (outcome.reason.find ("changed"), std::string::npos);
}

// A terminal outcome is answered once; a late result for the consumed token is a
// no-op, and a second poll is Idle.
TEST (PolygonPickState, pollConsumesATerminalOutcomeExactlyOnce)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-6", world, token);
    state.BeginInput (token, world);
    state.Complete (token, MakePick ("GUID-A", 0, 0, 5), world);

    EXPECT_EQ (state.Poll ("pick-6", world).status, Status::Done);
    EXPECT_EQ (state.ShouldAbort (token), Abort::Superseded);
    EXPECT_EQ (state.Poll ("pick-6", world).status, Status::Idle);

    state.Complete (token, MakePick ("GUID-B", 0, 0, 5), world); // ignored
    EXPECT_EQ (state.Poll ("pick-6", world).status, Status::Idle);
}

TEST (PolygonPickState, pollOfAnUnknownRequestIsIdle)
{
    PolygonPickState state;
    EXPECT_EQ (state.Poll ("nope", World (1, 1)).status, Status::Idle);
    EXPECT_EQ (state.Peek ("nope").status, Status::Idle);
}

// Cancelling a terminal request must not mutate it.
TEST (PolygonPickState, terminalCancellationDoesNotMutate)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-done", world, token);
    state.BeginInput (token, world);
    state.Complete (token, MakePick ("GUID-A", 0, 0, 5), world);

    Token ignored;
    EXPECT_FALSE (state.RequestCancel ("pick-done", ignored));
    EXPECT_FALSE (state.Peek ("pick-done").cancelRequested);
    EXPECT_EQ (state.Poll ("pick-done", world).status, Status::Done);
}

// A posting failure is cleaned up by failing the matching token, which Poll then
// reports instead of leaving it pending forever.
TEST (PolygonPickState, postingFailureCleanupTransitionsToFailed)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-post", world, token);
    state.Fail (token, 0, "the main-thread gate is not dispatching; the pick was not started");

    const Outcome outcome = state.Poll ("pick-post", world);
    ASSERT_EQ (outcome.status, Status::Failed);
    EXPECT_FALSE (outcome.haveSdkCode);
}

// A real SDK cancellation keeps its raw code; a logical one does not invent one.
TEST (PolygonPickState, sdkCancelRetainsRawCodeAndLogicalCancelDoesNot)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);

    Token sdk;
    state.Begin ("pick-sdk", world, sdk);
    state.BeginInput (sdk, world);
    state.EndInput (sdk);
    state.Cancelled (sdk, -2130313100L, "the user cancelled the pick");
    const Outcome sdkOut = state.Poll ("pick-sdk", world);
    ASSERT_EQ (sdkOut.status, Status::Cancelled);
    EXPECT_TRUE (sdkOut.haveSdkCode);
    EXPECT_EQ (sdkOut.sdkCode, -2130313100L);

    Token logical;
    state.Begin ("pick-logical", world, logical);
    state.Cancelled (logical, 0, "superseded");
    const Outcome logicalOut = state.Poll ("pick-logical", world);
    ASSERT_EQ (logicalOut.status, Status::Cancelled);
    EXPECT_FALSE (logicalOut.haveSdkCode);
}

// OnComponent is ignored once the input has ended.
TEST (PolygonPickState, callbackAfterInputEndedIsIgnored)
{
    PolygonPickState state;
    const WorldIdentity world = World (100, 77);
    Token token;
    state.Begin ("pick-ended", world, token);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);
    state.EndInput (token);

    state.OnComponent (token, MakeSighting ("GUID-A", 0, 0, 5, 1.0, 0.0, 0.0));
    EXPECT_FALSE (state.Peek ("pick-ended").callbackSawComponent);
}

TEST (PolygonPickState, differentBodyAloneCannotSupplyThePickedNormal)
{
    PolygonPickState state;
    const auto world = World (100, 77);
    Token token;
    ASSERT_EQ (state.Begin ("body-mismatch", world, token), BeginStatus::Accepted);
    ASSERT_EQ (state.BeginInput (token, world), InputStart::Started);
    state.OnComponent (token, MakeSighting ("GUID-A", 3, 2, 5, 1, 0, 0));
    state.Complete (token, MakePick ("GUID-A", 3, 1, 5), world);
    state.EndInput (token);
    const auto outcome = state.Poll (token.requestId, world);
    ASSERT_EQ (outcome.status, Status::Done);
    EXPECT_FALSE (outcome.callbackMatchedPick);
    EXPECT_FALSE (outcome.pick.haveNormal);
}

TEST (PolygonPickState, completionAfterResetCannotPublishIntoANewRequest)
{
    PolygonPickState state;
    const auto world = World (100, 77);
    Token oldToken;
    ASSERT_EQ (state.Begin ("retired", world, oldToken), BeginStatus::Accepted);
    ASSERT_EQ (state.BeginInput (oldToken, world), InputStart::Started);
    state.Reset ();
    state.Complete (oldToken, MakePick ("GUID-A", 3, 1, 5), world);
    state.EndInput (oldToken);
    EXPECT_EQ (state.Peek (oldToken.requestId).status, Status::Idle);
    Token newToken;
    ASSERT_EQ (state.Begin ("fresh", world, newToken), BeginStatus::Accepted);
    state.Complete (oldToken, MakePick ("GUID-A", 3, 1, 5), world);
    EXPECT_EQ (state.Peek (newToken.requestId).status, Status::Pending);
    EXPECT_FALSE (state.Peek (newToken.requestId).pick.found);
}

} // namespace
