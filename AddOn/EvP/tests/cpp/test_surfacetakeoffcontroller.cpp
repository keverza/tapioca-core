#include "Geometry/SurfaceTakeoffController.hpp"
#include "NativeCommands/SurfaceTakeoffSeedInput.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace geomsrv;
using namespace geomsrv::surfacetakeoff;
constexpr auto kEventBound = std::chrono::seconds (10);

class SeedInputFixture : public testing::Test {
  protected:
    Request request;
    polygonpick::PolygonPickState state;
    polygonpick::WorldIdentity world { 7, true, 5 };
    const uint64_t lifecycle = state.Generation ();
    polygonpick::Token token;
    std::vector<std::string> prompts, cancelled;
    bool stopped = false;
    std::function<void ()> onWait;
    SeedPicker picker { [this] (const std::string& prompt, std::string& id, std::string& error) {
                           const std::string candidate = "owned-" + std::to_string (prompts.size () + 1);
                           const auto started = state.Begin (candidate, world, token, lifecycle);
                           if (started != polygonpick::BeginStatus::Accepted) {
                               error = "picker refused";
                               return false;
                           }
                           id = candidate;
                           prompts.push_back (prompt);
                           EXPECT_EQ (state.BeginInput (token, world), polygonpick::InputStart::Started);
                           return true;
                       },
                        [this] (const std::string& id) { return state.Poll (id, world); },
                        [this] (const std::string& id) {
                            cancelled.push_back (id);
                            polygonpick::Token owned;
                            state.RequestCancel (id, owned);
                        },
                        [this] {
                            return stopped || world.captureStamp != request.captureStamp ||
                                   world.snapshotId != request.snapshotId;
                        },
                        [this] { onWait (); } };

    void SetUp () override
    {
        request.snapshotId = world.snapshotId;
        request.captureStamp = world.captureStamp;
        request.targetGuids = { "WALL" };
    }

    void CommitFace (bool match = true)
    {
        polygonpick::Sighting sighting;
        sighting.valid = true;
        sighting.guid = "wall";
        sighting.ipgon = match ? 42 : 10;
        sighting.normal[2] = 1;
        state.OnComponent (token, sighting);
        polygonpick::FacePick pick;
        pick.found = true;
        pick.guid = sighting.guid;
        pick.ipgon = 42;
        pick.pos[0] = double (prompts.size ());
        state.Complete (token, pick, world);
        state.EndInput (token);
    }
};

TEST_F (SeedInputFixture, SequencesCallbackMatchedFacesAndKeepsOnlyWorldProvenance)
{
    onWait = [this] { CommitFace (); };
    const auto input = CollectExteriorSeeds (request, 2, picker);
    ASSERT_TRUE (input.error.empty ()) << input.error;
    ASSERT_EQ (input.seeds.size (), 2u);
    EXPECT_EQ (prompts, (std::vector<std::string> { "Pick EXTERIOR facade 1/2", "Pick EXTERIOR facade 2/2" }));
    EXPECT_EQ (input.seeds[0].guid, "WALL");
    EXPECT_EQ (input.seeds[0].normal, (std::vector<double> { 0, 0, 1 }));
    EXPECT_EQ (input.seeds[1].point, (std::vector<double> { 2, 0, 0 }));
    EXPECT_TRUE (cancelled.empty ());
}

TEST_F (SeedInputFixture, EscapeDiscardsPreviouslyAcceptedSeeds)
{
    onWait = [this] {
        if (prompts.size () == 1) {
            CommitFace ();
            return;
        }
        state.Cancelled (token, -1, "the user cancelled the pick");
        state.EndInput (token);
    };
    const auto input = CollectExteriorSeeds (request, 2, picker);
    EXPECT_TRUE (input.cancelled);
    EXPECT_TRUE (input.seeds.empty ());
    EXPECT_EQ (input.error, "Exterior pick cancelled: the user cancelled the pick");
    EXPECT_TRUE (cancelled.empty ());
}

TEST_F (SeedInputFixture, StopCancelsOnlyTheOwnedInput)
{
    onWait = [this] { stopped = true; };
    const auto input = CollectExteriorSeeds (request, 1, picker);
    EXPECT_TRUE (input.cancelled);
    EXPECT_TRUE (input.seeds.empty ());
    EXPECT_EQ (cancelled, (std::vector<std::string> { "owned-1" }));
    EXPECT_EQ (state.ShouldAbort (token), polygonpick::Abort::CancelRequested);
}

TEST_F (SeedInputFixture, CaptureReplacementDropsACompletedFaceBeforeAdoption)
{
    onWait = [this] {
        CommitFace ();
        ++world.snapshotId;
    };
    const auto input = CollectExteriorSeeds (request, 1, picker);
    EXPECT_TRUE (input.seeds.empty ());
    EXPECT_FALSE (input.error.empty ());
    EXPECT_EQ (cancelled, (std::vector<std::string> { "owned-1" }));
}

TEST_F (SeedInputFixture, ProjectResetForbidsPostingTheNextFace)
{
    onWait = [this] { CommitFace (); };
    const auto poll = picker.poll;
    picker.poll = [this, poll] (const std::string& id) {
        auto outcome = poll (id);
        if (outcome.status == polygonpick::Status::Done)
            state.Reset ();
        return outcome;
    };
    const auto input = CollectExteriorSeeds (request, 2, picker);
    EXPECT_TRUE (input.seeds.empty ());
    EXPECT_EQ (input.error, "picker refused");
    EXPECT_EQ (prompts.size (), 1u);
}

TEST_F (SeedInputFixture, RejectsUnmatchedCallback)
{
    onWait = [this] { CommitFace (false); };
    const auto input = CollectExteriorSeeds (request, 1, picker);
    EXPECT_TRUE (input.seeds.empty ());
    EXPECT_FALSE (input.cancelled);
    EXPECT_NE (input.error.find ("no matching callback"), std::string::npos);
}

Snapshot ScopeFixture ()
{
    Snapshot snapshot;
    snapshot.id = 5;
    snapshot.captureStamp = 7;
    snapshot.completeModel = true;
    snapshot.meshes = { { "WALL", 1 },    { "SLAB", 2 },    { "door", 13 }, { "window", 14 },
                        { "object", 15 }, { "COLUMN", 17 }, { "BEAM", 19 } };
    snapshot.surfaceAttributes = { { 100, { "Red", "" } }, { 200, { "Yellow", "" } } };
    return snapshot;
}

TEST (SurfaceTakeoffPreparation, ResolvesEveryCoverPolicyIndependentlyOfTargets)
{
    const auto snapshot = ScopeFixture ();
    const std::map<std::string, std::vector<std::string>> expectations {
        { "constructionWithOpenings", { "BEAM", "COLUMN", "DOOR", "OBJECT", "SLAB", "WALL", "WINDOW" } },
        { "construction", { "BEAM", "COLUMN", "OBJECT", "SLAB", "WALL" } },
        { "wallsSlabsBeams", { "BEAM", "OBJECT", "SLAB", "WALL" } },
        { "slabs", { "OBJECT", "SLAB" } },
        { "wallsSlabs", { "OBJECT", "SLAB", "WALL" } },
        { "extraOnly", { "OBJECT" } }
    };
    std::string error;
    for (const auto& [policy, covers] : expectations) {
        SCOPED_TRACE (policy);
        const auto request = PrepareRequest (snapshot, "allElementFaces", policy, { "object" }, {}, error);
        ASSERT_TRUE (request) << error;
        EXPECT_EQ (request->coverGuids, covers);
        EXPECT_EQ (request->targetGuids, (std::vector<std::string> { "BEAM", "COLUMN", "SLAB", "WALL" }));
        EXPECT_TRUE (request->surfaceAttributeIndices.empty ());
    }
    const auto walls = PrepareRequest (snapshot, "allWallFaces", "slabs", { "slab", "OBJECT" },
                                       { "Yellow", "", "Red", "Yellow" }, error);
    const auto seeded = PrepareRequest (snapshot, "seededFacade", "slabs", { "object" }, {}, error);
    ASSERT_TRUE (walls && seeded) << error;
    EXPECT_EQ (walls->targetGuids, (std::vector<std::string> { "WALL" }));
    EXPECT_EQ (seeded->targetGuids, walls->targetGuids);
    EXPECT_EQ (walls->surfaceAttributeIndices, (std::vector<int32_t> { 200, 100 }));
    EXPECT_EQ (walls->coverScopeId, seeded->coverScopeId);
    const auto fewerCovers = PrepareRequest (snapshot, "allWallFaces", "slabs", {}, {}, error);
    ASSERT_TRUE (fewerCovers);
    EXPECT_NE (walls->coverScopeId, fewerCovers->coverScopeId);
}

TEST (SurfaceTakeoffPreparation, RefusesInvalidOrIncompletePreparedScopes)
{
    auto snapshot = ScopeFixture ();
    std::string error;
    EXPECT_FALSE (PrepareRequest (snapshot, "seededFacade", "slabs", { "absent" }, {}, error));
    EXPECT_NE (error.find ("cover missing or ambiguous"), std::string::npos);
    EXPECT_FALSE (PrepareRequest (snapshot, "seededFacade", "slabs", {}, { "Unknown" }, error));
    EXPECT_NE (error.find ("absent"), std::string::npos);
    snapshot.surfaceAttributes[300] = { "Red", "" };
    EXPECT_FALSE (PrepareRequest (snapshot, "seededFacade", "slabs", {}, { "Red" }, error));
    EXPECT_NE (error.find ("ambiguous"), std::string::npos);
    snapshot.meshes.push_back ({ "wall", 1 });
    EXPECT_FALSE (PrepareRequest (snapshot, "seededFacade", "slabs", {}, {}, error));
    EXPECT_NE (error.find ("Target missing or ambiguous"), std::string::npos);
    snapshot.completeModel = false;
    EXPECT_FALSE (PrepareRequest (snapshot, "seededFacade", "slabs", {}, {}, error));
    EXPECT_NE (error.find ("unfiltered"), std::string::npos);
}

std::shared_ptr<const Snapshot> Capture (uint64_t id, uint64_t stamp)
{
    auto snapshot = std::make_shared<Snapshot> ();
    snapshot->id = id;
    uint64_t currentStamp = MeshStore::Get ().CaptureStamp ();
    while (currentStamp < stamp) {
        MeshStore::Get ().BumpCaptureStamp ();
        ++currentStamp;
    }
    snapshot->captureStamp = currentStamp;
    snapshot->scope = "controller-test";
    MeshStore::Get ().Publish (snapshot);
    return snapshot;
}

ControllerOutcome Success ()
{
    return { true, std::make_shared<int> (1), {} };
}

ControllerOutcome Failure (const char* text)
{
    return { false, {}, text };
}

ControllerView Wait (SurfaceTakeoffController& controller, ControllerState state)
{
    const auto deadline = std::chrono::steady_clock::now () + kEventBound;
    for (;;) {
        auto view = controller.Poll ();
        if (view.state == state)
            return view;
        if (std::chrono::steady_clock::now () >= deadline) {
            ADD_FAILURE () << "Timed out waiting for " << ControllerStateName (state) << ", actual "
                           << ControllerStateName (view.state) << ": " << view.error;
            return view;
        }
        std::this_thread::yield ();
    }
}

struct Gate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future ().share ();
    std::future<void> Entered ()
    {
        return entered.get_future ();
    }
    void Open ()
    {
        release.set_value ();
    }
};

class ControllerFixture : public testing::Test {
  protected:
    SurfaceTakeoffController controller;
    std::string error;
    void TearDown () override
    {
        controller.Shutdown ();
        MeshStore::Get ().Release ();
    }
};
} // namespace

TEST_F (ControllerFixture, StandaloneCompletionPublishesCurrentResultWithoutOverlay)
{
    const auto snapshot = Capture (19, 7);
    const auto expected = Success ();
    ASSERT_TRUE (controller.Submit ({ snapshot, [expected] (const std::atomic<bool>&) { return expected; } }, error));
    const auto view = Wait (controller, ControllerState::Current);
    ASSERT_TRUE (view.lastGood.has_value ());
    EXPECT_EQ (view.lastGood->payload, expected.payload);
    EXPECT_EQ (view.lastGood->snapshotId, 19u);
    EXPECT_EQ (view.lastGood->captureStamp, snapshot->captureStamp);
    EXPECT_EQ (view.lastGood->generation, view.generation);
    EXPECT_TRUE (view.error.empty ());
}

TEST_F (ControllerFixture, SuccessWithoutPayloadRefusesAndPreservesLastGood)
{
    const auto snapshot = Capture (19, 7);
    const auto emptySuccess = [] (const std::atomic<bool>&) { return ControllerOutcome { true, {}, {} }; };
    ASSERT_TRUE (controller.Submit ({ snapshot, emptySuccess }, error));
    const auto refused = Wait (controller, ControllerState::Refused);
    EXPECT_FALSE (refused.lastGood.has_value ());
    EXPECT_EQ (refused.error, "Surface calculation returned no result.");

    const auto expected = Success ();
    ASSERT_TRUE (controller.Submit ({ snapshot, [expected] (const std::atomic<bool>&) { return expected; } }, error));
    ASSERT_EQ (Wait (controller, ControllerState::Current).state, ControllerState::Current);
    ASSERT_TRUE (controller.Submit ({ snapshot, emptySuccess }, error));
    const auto stale = Wait (controller, ControllerState::Stale);
    ASSERT_TRUE (stale.lastGood.has_value ());
    EXPECT_EQ (stale.lastGood->payload, expected.payload);
    EXPECT_NE (stale.lastGood->generation, stale.generation);
}

TEST_F (ControllerFixture, SupersededCompletionCannotPublishOverReplacement)
{
    const auto snapshot = Capture (19, 7);
    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    const auto first = Success (), second = Success ();
    ASSERT_TRUE (controller.Submit ({ snapshot,
                                      [gate, first] (const std::atomic<bool>&) {
                                          gate->entered.set_value ();
                                          gate->released.wait ();
                                          return first;
                                      } },
                                    error));
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    ASSERT_TRUE (controller.Submit ({ snapshot, [second] (const std::atomic<bool>&) { return second; } }, error));
    gate->Open ();
    const auto view = Wait (controller, ControllerState::Current);
    ASSERT_TRUE (view.lastGood.has_value ());
    EXPECT_EQ (view.lastGood->payload, second.payload);
    EXPECT_NE (view.lastGood->payload, first.payload);
}

TEST_F (ControllerFixture, CancelDiscardsQueuedWorkWithoutExecutingIt)
{
    const auto snapshot = Capture (19, 7);
    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    std::atomic<size_t> queuedCalls { 0 };
    const uint64_t firstGeneration = controller.Submit ({ snapshot,
                                                          [gate] (const std::atomic<bool>&) {
                                                              gate->entered.set_value ();
                                                              gate->released.wait ();
                                                              return Success ();
                                                          } },
                                                        error);
    ASSERT_GT (firstGeneration, 0u);
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    const uint64_t queuedGeneration = controller.Submit ({ snapshot,
                                                           [&queuedCalls] (const std::atomic<bool>&) {
                                                               ++queuedCalls;
                                                               return Success ();
                                                           } },
                                                         error);
    EXPECT_GT (queuedGeneration, firstGeneration);
    controller.Cancel ();
    EXPECT_EQ (controller.Poll ().state, ControllerState::Cancelled);
    gate->Open ();
    EXPECT_EQ (Wait (controller, ControllerState::Cancelled).state, ControllerState::Cancelled);
    EXPECT_EQ (queuedCalls.load (), 0u);
}

TEST_F (ControllerFixture, ClearDiscardsQueuedWorkAndIsImmediatelyEmpty)
{
    const auto snapshot = Capture (19, 7);
    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    std::atomic<size_t> queuedCalls { 0 };
    ASSERT_TRUE (controller.Submit ({ snapshot,
                                      [gate] (const std::atomic<bool>&) {
                                          gate->entered.set_value ();
                                          gate->released.wait ();
                                          return Success ();
                                      } },
                                    error));
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    ASSERT_TRUE (controller.Submit ({ snapshot,
                                      [&queuedCalls] (const std::atomic<bool>&) {
                                          ++queuedCalls;
                                          return Success ();
                                      } },
                                    error));
    controller.Clear ();
    const auto cleared = controller.Poll ();
    EXPECT_EQ (cleared.state, ControllerState::Empty);
    EXPECT_FALSE (cleared.lastGood.has_value ());
    gate->Open ();
    EXPECT_EQ (Wait (controller, ControllerState::Empty).state, ControllerState::Empty);
    EXPECT_EQ (queuedCalls.load (), 0u);
}

TEST_F (ControllerFixture, SupersededCallerCannotClearItsSuccessor)
{
    const auto snapshot = Capture (19, 7);
    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    const auto first = Success (), second = Success ();
    const uint64_t firstGeneration = controller.Submit ({ snapshot,
                                                          [gate, first] (const std::atomic<bool>&) {
                                                              gate->entered.set_value ();
                                                              gate->released.wait ();
                                                              return first;
                                                          } },
                                                        error);
    ASSERT_GT (firstGeneration, 0u);
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    const uint64_t secondGeneration =
        controller.Submit ({ snapshot, [second] (const std::atomic<bool>&) { return second; } }, error);
    ASSERT_GT (secondGeneration, firstGeneration);
    EXPECT_FALSE (controller.Clear (firstGeneration));
    gate->Open ();
    const auto view = Wait (controller, ControllerState::Current);
    ASSERT_TRUE (view.lastGood.has_value ());
    EXPECT_EQ (view.lastGood->generation, secondGeneration);
    EXPECT_EQ (view.lastGood->payload, second.payload);
}

TEST_F (ControllerFixture, PublicationCommitAndCancelShareOneGenerationGate)
{
    const auto snapshot = Capture (19, 7);
    ASSERT_TRUE (controller.Submit ({ snapshot, [] (const std::atomic<bool>&) { return Success (); } }, error));
    const auto current = Wait (controller, ControllerState::Current);
    const uint64_t generation = current.generation;
    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    auto committed = std::async (std::launch::async, [&] {
        std::string commitError;
        return controller.CommitPublication (
            generation,
            [&] (std::string&) {
                gate->entered.set_value ();
                gate->released.wait ();
                return true;
            },
            commitError);
    });
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    auto cancelled = std::async (std::launch::async, [&] { return controller.Cancel (generation); });
    EXPECT_EQ (cancelled.wait_for (std::chrono::milliseconds (20)), std::future_status::timeout);
    gate->Open ();
    ASSERT_EQ (committed.wait_for (kEventBound), std::future_status::ready);
    EXPECT_TRUE (committed.get ());
    ASSERT_EQ (cancelled.wait_for (kEventBound), std::future_status::ready);
    EXPECT_TRUE (cancelled.get ());
    std::atomic<bool> called { false };
    std::string commitError;
    EXPECT_FALSE (controller.CommitPublication (
        generation,
        [&] (std::string&) {
            called.store (true);
            return true;
        },
        commitError));
    EXPECT_FALSE (called.load ());
}

TEST_F (ControllerFixture, CaptureReplacementCannotPublishLateResult)
{
    const auto snapshot = Capture (19, 7);
    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    ASSERT_TRUE (controller.Submit ({ snapshot,
                                      [gate] (const std::atomic<bool>&) {
                                          gate->entered.set_value ();
                                          gate->released.wait ();
                                          return Success ();
                                      } },
                                    error));
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    Capture (20, 8);
    gate->Open ();
    const auto view = Wait (controller, ControllerState::Stale);
    EXPECT_FALSE (view.lastGood.has_value ());
    EXPECT_NE (view.error.find ("Capture changed"), std::string::npos);
}

TEST_F (ControllerFixture, FailedReplacementRetainsClearlyStaleLastGood)
{
    const auto snapshot = Capture (19, 7);
    const auto expected = Success ();
    ASSERT_TRUE (controller.Submit ({ snapshot, [expected] (const std::atomic<bool>&) { return expected; } }, error));
    ASSERT_EQ (Wait (controller, ControllerState::Current).state, ControllerState::Current);
    ASSERT_TRUE (controller.Submit (
        { snapshot, [] (const std::atomic<bool>&) { return Failure ("expected refusal"); } }, error));
    const auto view = Wait (controller, ControllerState::Stale);
    ASSERT_TRUE (view.lastGood.has_value ());
    EXPECT_EQ (view.lastGood->payload, expected.payload);
    EXPECT_EQ (view.error, "expected refusal");
}

TEST_F (ControllerFixture, ClearProjectCloseAndRestartRevokePendingAndLastGood)
{
    const auto snapshot = Capture (19, 7);
    ASSERT_TRUE (controller.Submit ({ snapshot, [] (const std::atomic<bool>&) { return Success (); } }, error));
    ASSERT_EQ (Wait (controller, ControllerState::Current).state, ControllerState::Current);

    auto gate = std::make_shared<Gate> ();
    auto entered = gate->Entered ();
    ASSERT_TRUE (controller.Submit ({ snapshot,
                                      [gate] (const std::atomic<bool>&) {
                                          gate->entered.set_value ();
                                          gate->released.wait ();
                                          return Success ();
                                      } },
                                    error));
    ASSERT_EQ (entered.wait_for (kEventBound), std::future_status::ready);
    controller.OnProjectClosed ();
    gate->Open ();
    const auto closed = Wait (controller, ControllerState::Empty);
    EXPECT_FALSE (closed.lastGood.has_value ());

    ASSERT_TRUE (controller.Submit ({ snapshot, [] (const std::atomic<bool>&) { return Success (); } }, error));
    EXPECT_EQ (Wait (controller, ControllerState::Current).state, ControllerState::Current);
}

TEST_F (ControllerFixture, CancelIsCooperativeAndDoesNotPublishOrRunInline)
{
    const auto snapshot = Capture (19, 7);
    std::atomic<bool> observed { false };
    std::promise<void> entered;
    auto started = entered.get_future ();
    ASSERT_TRUE (controller.Submit ({ snapshot,
                                      [&observed, &entered] (const std::atomic<bool>& cancelled) {
                                          entered.set_value ();
                                          while (!cancelled.load ())
                                              std::this_thread::yield ();
                                          observed.store (true);
                                          return Success ();
                                      } },
                                    error));
    ASSERT_EQ (started.wait_for (kEventBound), std::future_status::ready);
    controller.Cancel ();
    const auto view = Wait (controller, ControllerState::Cancelled);
    const auto observedDeadline = std::chrono::steady_clock::now () + kEventBound;
    while (!observed.load () && std::chrono::steady_clock::now () < observedDeadline)
        std::this_thread::yield ();
    EXPECT_TRUE (observed.load ());
    EXPECT_FALSE (view.lastGood.has_value ());
    controller.Shutdown ();
    controller.Shutdown ();
    EXPECT_EQ (controller.Poll ().state, ControllerState::Empty);
}

TEST_F (ControllerFixture, RefusalHasNoPartialQuantityAndClearRemovesLastGood)
{
    const auto snapshot = Capture (19, 7);
    ASSERT_TRUE (
        controller.Submit ({ snapshot, [] (const std::atomic<bool>&) { return Failure ("bad cover"); } }, error));
    auto refused = Wait (controller, ControllerState::Refused);
    EXPECT_FALSE (refused.lastGood.has_value ());
    EXPECT_EQ (refused.error, "bad cover");

    ASSERT_TRUE (controller.Submit ({ snapshot, [] (const std::atomic<bool>&) { return Success (); } }, error));
    ASSERT_EQ (Wait (controller, ControllerState::Current).state, ControllerState::Current);
    controller.Clear ();
    const auto cleared = controller.Poll ();
    EXPECT_EQ (cleared.state, ControllerState::Empty);
    EXPECT_FALSE (cleared.lastGood.has_value ());
}
