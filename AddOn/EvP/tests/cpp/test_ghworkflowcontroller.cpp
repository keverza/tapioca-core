// Grasshopper/GhWorkflowController.cpp — the Archicad end of one workflow
// session: which inputs, when to solve, and which result may be published.
//
// This is the sharpest offline case on the whole GH path, and the reason is
// what its failures LOOK like. A framing bug shows up as "Grasshopper did not
// work". A revision bug does not show up at all: the panel is responsive, the
// viewport has geometry in it, the Apply button is enabled, and the geometry is
// simply from a request the user superseded three drags ago. There is nothing on
// screen to notice.
//
// It also cannot be produced live on demand. The race needs a solve to finish
// AFTER a newer request was sent, which depends on how long a definition happens
// to take — so the rule is proved here, with the clock and the message order in
// the test's hands, and a live run is left to prove that the wire works at all.

#include "Grasshopper/GhWorkflowController.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp::grasshopper;

namespace {

// Records what the controller sent, decoded back into the payloads it built.
// Decoding rather than comparing bytes: the claim under test is "it sent a solve
// for revision 4", and a byte comparison would restate the codec instead.
class Recorder {
  public:
    struct Sent {
        protocol::MessageType type = protocol::MessageType::Hello;
        std::vector<uint8_t> payload;
    };

    GhWorkflowController::Sender Sender ()
    {
        return [this] (protocol::MessageType type, const std::vector<uint8_t>& payload, std::string& error) {
            if (fail) {
                error = "the pipe is closed";
                return false;
            }
            sent.push_back ({ type, payload });
            return true;
        };
    }

    size_t Count (protocol::MessageType type) const
    {
        size_t count = 0;
        for (const Sent& entry : sent) {
            if (entry.type == type)
                ++count;
        }
        return count;
    }

    // The last message of a type, decoded. Fails the test rather than returning
    // a default: a missing message is the bug, not a zero.
    bool LastSolve (protocol::SolvePayload& out) const
    {
        for (size_t index = sent.size (); index > 0; --index) {
            const Sent& entry = sent[index - 1];
            if (entry.type != protocol::MessageType::Solve)
                continue;
            std::string error;
            return protocol::DecodeSolvePayload (entry.payload.data (), entry.payload.size (), out, error);
        }
        return false;
    }

    bool LastInputs (protocol::SetInputsPayload& out) const
    {
        for (size_t index = sent.size (); index > 0; --index) {
            const Sent& entry = sent[index - 1];
            if (entry.type != protocol::MessageType::SetInputs)
                continue;
            std::string error;
            return protocol::DecodeSetInputsPayload (entry.payload.data (), entry.payload.size (), out, error);
        }
        return false;
    }

    // The order the two halves of a snapshot went out in, as their types.
    std::vector<protocol::MessageType> Order () const
    {
        std::vector<protocol::MessageType> types;
        for (const Sent& entry : sent)
            types.push_back (entry.type);
        return types;
    }

    void Clear ()
    {
        sent.clear ();
    }

    std::vector<Sent> sent;
    bool fail = false;
};

// A controller with a worker up and one session open, which is the state every
// test below except the lifecycle ones starts from.
uint32_t OpenOne (GhWorkflowController& controller, Recorder& recorder, uint32_t generation = 3)
{
    controller.SetSender (recorder.Sender ());
    controller.OnHostGeneration (generation);
    std::string error;
    const uint32_t session = controller.OpenSession (protocol::SessionMode::Headless, error);
    recorder.Clear ();
    return session;
}

std::vector<protocol::SessionInputValue> Inputs (const std::string& value)
{
    std::vector<protocol::SessionInputValue> inputs;
    inputs.push_back ({ "height", value });
    return inputs;
}

protocol::SolutionResultPayload ResultFor (uint32_t generation, uint32_t session, uint32_t requestRevision,
                                           uint32_t solutionRevision)
{
    protocol::SolutionResultPayload result;
    result.envelope.hostGeneration = generation;
    result.envelope.sessionId = session;
    result.envelope.requestRevision = requestRevision;
    result.solutionRevision = solutionRevision;
    result.definitionIdentity = "facade.gh";
    return result;
}

} // namespace

TEST (GhWorkflowController, ASessionCannotBeOpenedWithoutAHost)
{
    GhWorkflowController controller;
    Recorder recorder;
    controller.SetSender (recorder.Sender ());

    std::string error;
    EXPECT_EQ (0u, controller.OpenSession (protocol::SessionMode::Headless, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (0u, recorder.sent.size ());
}

TEST (GhWorkflowController, TheHostAssignsSessionIdsAndNeverReusesOne)
{
    // The host owns the numbering because the host owns the store the results
    // land in. Reuse would let a late message from a dead worker address a live
    // session -- exactly what the generation and the id exist together to stop.
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t first = OpenOne (controller, recorder);
    ASSERT_NE (0u, first);

    std::string error;
    ASSERT_TRUE (controller.CloseSession (error)) << error;
    const uint32_t second = controller.OpenSession (protocol::SessionMode::Headless, error);
    ASSERT_NE (0u, second);
    EXPECT_NE (first, second);
}

TEST (GhWorkflowController, ASecondSessionIsRefusedRatherThanReplacingTheFirst)
{
    GhWorkflowController controller;
    Recorder recorder;
    ASSERT_NE (0u, OpenOne (controller, recorder));

    std::string error;
    EXPECT_EQ (0u, controller.OpenSession (protocol::SessionMode::Headless, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GhWorkflowController, AFailedOpenBurnsNoSessionId)
{
    GhWorkflowController controller;
    Recorder recorder;
    controller.SetSender (recorder.Sender ());
    controller.OnHostGeneration (2);
    recorder.fail = true;

    std::string error;
    EXPECT_EQ (0u, controller.OpenSession (protocol::SessionMode::Headless, error));
    EXPECT_EQ (0u, controller.SessionId ());

    recorder.fail = false;
    const uint32_t opened = controller.OpenSession (protocol::SessionMode::Headless, error);
    EXPECT_EQ (1u, opened) << "a failed send must not consume the id it was going to use";
}

TEST (GhWorkflowController, InputChangesDoNotSolveUntilTheySettle)
{
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);
    controller.SetDebounce (100);

    // A drag: the value keeps changing, and every change restarts the window.
    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    EXPECT_FALSE (controller.Tick (1000)); // starts the clock
    EXPECT_FALSE (controller.Tick (1050));
    controller.SetInputs (Inputs ("2"), protocol::SolveWantsData);
    EXPECT_FALSE (controller.Tick (1060));
    EXPECT_FALSE (controller.Tick (1150));
    controller.SetInputs (Inputs ("3"), protocol::SolveWantsData);
    EXPECT_FALSE (controller.Tick (1160));

    EXPECT_EQ (0u, recorder.Count (protocol::MessageType::Solve)) << "a drag that never stops moving must never solve";

    // The user lets go.
    EXPECT_FALSE (controller.Tick (1200));
    EXPECT_TRUE (controller.Tick (1300));

    EXPECT_EQ (1u, recorder.Count (protocol::MessageType::Solve)) << "letting go must solve exactly once";

    protocol::SetInputsPayload inputs;
    ASSERT_TRUE (recorder.LastInputs (inputs));
    ASSERT_EQ (1u, inputs.inputs.size ());
    EXPECT_EQ ("3", inputs.inputs[0].value) << "the solve must carry the NEWEST value, not the first";
}

TEST (GhWorkflowController, TheSnapshotGoesOutBeforeTheSolveThatReadsIt)
{
    // A solve that overtook its own inputs would solve the PREVIOUS ones and
    // stamp the new revision on the answer: a wrong result that looks current.
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);
    controller.SetDebounce (10);

    controller.SetInputs (Inputs ("7"), protocol::SolveWantsData);
    controller.Tick (100);
    ASSERT_TRUE (controller.Tick (200));

    const std::vector<protocol::MessageType> order = recorder.Order ();
    ASSERT_EQ (2u, order.size ());
    EXPECT_EQ (protocol::MessageType::SetInputs, order[0]);
    EXPECT_EQ (protocol::MessageType::Solve, order[1]);
}

TEST (GhWorkflowController, BothHalvesOfOneSnapshotCarryOneRevision)
{
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);

    std::string error;
    controller.SetInputs (Inputs ("7"), protocol::SolveWantsPreview);
    ASSERT_TRUE (controller.SolveNow (error)) << error;

    protocol::SetInputsPayload inputs;
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastInputs (inputs));
    ASSERT_TRUE (recorder.LastSolve (solve));
    EXPECT_EQ (inputs.envelope.requestRevision, solve.envelope.requestRevision);
    EXPECT_NE (0u, solve.envelope.requestRevision);
    EXPECT_EQ ((uint32_t) protocol::SolveWantsPreview, solve.wants);
}

TEST (GhWorkflowController, AnExplicitPressDoesNotWaitForTheSettleWindow)
{
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);
    controller.SetDebounce (5000);

    std::string error;
    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    ASSERT_TRUE (controller.SolveNow (error)) << error;
    EXPECT_EQ (1u, recorder.Count (protocol::MessageType::Solve));
}

TEST (GhWorkflowController, ADefinitionWithNoInputsStillSolvesOnAPress)
{
    // Refusing here would make an input-less definition unrunnable; it holds its
    // own saved values and has nothing to send.
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);

    std::string error;
    ASSERT_TRUE (controller.SolveNow (error)) << error;
    EXPECT_EQ (1u, recorder.Count (protocol::MessageType::Solve));
}

TEST (GhWorkflowController, TheNewestRequestedRevisionWins)
{
    // The rule §7 exists for, and the one nothing on screen would reveal.
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    std::string error;
    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload first;
    ASSERT_TRUE (recorder.LastSolve (first));

    controller.SetInputs (Inputs ("2"), protocol::SolveWantsData);
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload second;
    ASSERT_TRUE (recorder.LastSolve (second));
    ASSERT_LT (first.envelope.requestRevision, second.envelope.requestRevision);

    // The OLD solve finishes first, which is the ordinary case when a cheap
    // second edit follows an expensive first one.
    EXPECT_TRUE (controller.OnSolutionResult (ResultFor (3, session, first.envelope.requestRevision, 1)));
    StoredSolution stored;
    EXPECT_FALSE (controller.CurrentSolution (stored)) << "a superseded solve must not become the current solution";
    EXPECT_FALSE (controller.Status ().hasCurrentSolution);

    // Then the new one.
    EXPECT_TRUE (controller.OnSolutionResult (ResultFor (3, session, second.envelope.requestRevision, 2)));
    ASSERT_TRUE (controller.CurrentSolution (stored));
    EXPECT_EQ (2u, stored.solutionRevision);
}

TEST (GhWorkflowController, TheResultForTheOutstandingRequestIsPublished)
{
    // The boundary the staleness comparison sits on: `<`, not `<=`. Getting it
    // wrong would discard every result the controller ever asked for.
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    std::string error;
    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastSolve (solve));

    ASSERT_TRUE (controller.OnSolutionResult (ResultFor (3, session, solve.envelope.requestRevision, 1)));
    StoredSolution stored;
    ASSERT_TRUE (controller.CurrentSolution (stored));
    EXPECT_EQ (solve.envelope.requestRevision, stored.requestRevision);
}

TEST (GhWorkflowController, AResultFromAnotherGenerationOrSessionIsRefused)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder, 3);

    std::string error;
    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastSolve (solve));

    EXPECT_FALSE (controller.OnSolutionResult (ResultFor (2, session, solve.envelope.requestRevision, 1)))
        << "a result from a dead worker generation must not be accepted";
    EXPECT_FALSE (controller.OnSolutionResult (ResultFor (3, session + 99, solve.envelope.requestRevision, 1)))
        << "a result for another session must not be accepted";

    StoredSolution stored;
    EXPECT_FALSE (controller.CurrentSolution (stored));
}

TEST (GhWorkflowController, ARestartInvalidatesTheSessionAndMarksThePreviewStale)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder, 3);

    std::string error;
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastSolve (solve));
    ASSERT_TRUE (controller.OnSolutionResult (ResultFor (3, session, solve.envelope.requestRevision, 1)));
    ASSERT_TRUE (controller.Status ().hasCurrentSolution);

    controller.OnHostGeneration (4);

    EXPECT_EQ (0u, controller.SessionId ());
    EXPECT_FALSE (controller.Status ().hasCurrentSolution) << "a solution whose worker is gone is not current";

    // Kept and visible, though: §11 says the last complete preview survives so
    // the user can still see what they had.
    StoredSolution stored;
    EXPECT_TRUE (controller.CurrentSolution (stored));
    EXPECT_EQ (1u, stored.solutionRevision);
}

TEST (GhWorkflowController, ApplyReadsTheStoredRevisionAndNeverSolves)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    std::string error;
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastSolve (solve));

    protocol::SolutionResultPayload result = ResultFor (3, session, solve.envelope.requestRevision, 5);
    result.outputs.push_back ({ "area", "number", "{0}", "42.5" });
    ASSERT_TRUE (controller.OnSolutionResult (result));

    recorder.Clear ();
    StoredSolution applied;
    ASSERT_TRUE (controller.SolutionForApply (5, applied, error)) << error;
    ASSERT_EQ (1u, applied.outputs.size ());
    EXPECT_EQ ("42.5", applied.outputs[0].value);
    EXPECT_EQ (0u, recorder.sent.size ()) << "Apply must not put a single message on the wire";
}

TEST (GhWorkflowController, ApplyRefusesARevisionThatIsNoLongerCurrent)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    std::string error;
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastSolve (solve));
    ASSERT_TRUE (controller.OnSolutionResult (ResultFor (3, session, solve.envelope.requestRevision, 5)));

    StoredSolution applied;
    EXPECT_FALSE (controller.SolutionForApply (4, applied, error))
        << "applying a revision the user did not preview is the substitution the revision contract forbids";
    EXPECT_FALSE (error.empty ());
}

TEST (GhWorkflowController, ApplyRefusesAfterTheHostIsGone)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    std::string error;
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload solve;
    ASSERT_TRUE (recorder.LastSolve (solve));
    ASSERT_TRUE (controller.OnSolutionResult (ResultFor (3, session, solve.envelope.requestRevision, 5)));

    controller.OnHostGone ("the pipe closed");

    StoredSolution applied;
    EXPECT_FALSE (controller.SolutionForApply (5, applied, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GhWorkflowController, AFailedSolveKeepsThePreviousSolution)
{
    // §11: keep the last complete preview after a solve failure unless the user
    // clears it. Blanking the viewport at the moment something went wrong takes
    // away the evidence.
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    std::string error;
    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload first;
    ASSERT_TRUE (recorder.LastSolve (first));
    ASSERT_TRUE (controller.OnSolutionResult (ResultFor (3, session, first.envelope.requestRevision, 1)));

    ASSERT_TRUE (controller.SolveNow (error));
    protocol::SolvePayload second;
    ASSERT_TRUE (recorder.LastSolve (second));

    protocol::SolutionFailedPayload failed;
    failed.envelope.hostGeneration = 3;
    failed.envelope.sessionId = session;
    failed.envelope.requestRevision = second.envelope.requestRevision;
    failed.failure = protocol::FailureCode::SolveFailed;
    failed.message = "A component threw.";
    ASSERT_TRUE (controller.OnSolutionFailed (failed));

    StoredSolution stored;
    ASSERT_TRUE (controller.CurrentSolution (stored));
    EXPECT_EQ (1u, stored.solutionRevision) << "the previous solution survives a failed solve";
    EXPECT_EQ (protocol::FailureCode::SolveFailed, controller.Status ().failure);
    EXPECT_FALSE (controller.Status ().busy);
}

TEST (GhWorkflowController, LoadingADefinitionDropsThePreviousSchemaAndPendingInputs)
{
    // Carrying them over would offer the user the old controls and then send
    // values keyed by ids the new definition may not declare.
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    protocol::SchemaResultPayload schema;
    schema.envelope.hostGeneration = 3;
    schema.envelope.sessionId = session;
    schema.schemaJson = "{\"inputs\":[]}";
    ASSERT_TRUE (controller.OnSchemaResult (schema));
    ASSERT_FALSE (controller.SchemaJson ().empty ());

    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);

    std::string error;
    ASSERT_TRUE (controller.LoadDefinition ("C:\\other.gh", error)) << error;
    EXPECT_TRUE (controller.SchemaJson ().empty ());

    recorder.Clear ();
    controller.Tick (100000);
    controller.Tick (200000);
    EXPECT_EQ (0u, recorder.Count (protocol::MessageType::Solve))
        << "the previous definition's pending inputs must not solve against the new one";
}

TEST (GhWorkflowController, BusyStartsWhenTheInputChangesRatherThanWhenTheWorkerSaysSo)
{
    // The worker's Solving lags a request by a round trip. A spinner that waited
    // for it would leave the panel looking idle for the whole time the user is
    // wondering whether their edit registered.
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);

    EXPECT_FALSE (controller.Status ().busy);
    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    EXPECT_TRUE (controller.Status ().busy);
}

TEST (GhWorkflowController, AClockThatGoesBackwardsRestartsTheWindowRatherThanWedging)
{
    GhWorkflowController controller;
    Recorder recorder;
    OpenOne (controller, recorder);
    controller.SetDebounce (100);

    controller.SetInputs (Inputs ("1"), protocol::SolveWantsData);
    EXPECT_FALSE (controller.Tick (5000));
    EXPECT_FALSE (controller.Tick (10)) << "a backwards clock must restart the window, not solve immediately";
    EXPECT_FALSE (controller.Tick (50));
    EXPECT_TRUE (controller.Tick (200));
}

TEST (GhWorkflowController, AHostRestartRequiredEventClosesTheSessionLocally)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    protocol::SessionAckPayload event;
    event.envelope.hostGeneration = 3;
    event.envelope.sessionId = session;
    event.state = protocol::SessionState::HostRestartRequired;
    event.failure = protocol::FailureCode::HostCrashed;
    event.message = "The engine thread faulted.";
    ASSERT_TRUE (controller.OnSessionEvent (event));

    EXPECT_EQ (0u, controller.SessionId ())
        << "a session the worker cannot recover must stop being driven from here too";
}

TEST (GhWorkflowController, AnEventForAnotherSessionIsRefusedWithoutChangingAnything)
{
    GhWorkflowController controller;
    Recorder recorder;
    const uint32_t session = OpenOne (controller, recorder);

    protocol::SessionAckPayload event;
    event.envelope.hostGeneration = 3;
    event.envelope.sessionId = session + 7;
    event.state = protocol::SessionState::Invalid;
    event.failure = protocol::FailureCode::DefinitionInvalid;
    EXPECT_FALSE (controller.OnSessionEvent (event));
    EXPECT_EQ (protocol::FailureCode::None, controller.Status ().failure);
    EXPECT_EQ (session, controller.SessionId ());
}

TEST (GhWorkflowController, NothingIsSentWithoutASession)
{
    GhWorkflowController controller;
    Recorder recorder;
    controller.SetSender (recorder.Sender ());
    controller.OnHostGeneration (1);

    std::string error;
    EXPECT_FALSE (controller.LoadDefinition ("a.gh", error));
    EXPECT_FALSE (controller.SolveNow (error));
    EXPECT_FALSE (controller.CancelSolve (error));
    EXPECT_FALSE (controller.RequestSchema (error));
    EXPECT_FALSE (controller.RequestDiagnostics (error));
    EXPECT_EQ (0u, recorder.sent.size ());
}
