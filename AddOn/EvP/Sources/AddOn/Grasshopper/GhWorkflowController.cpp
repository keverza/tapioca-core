#include "GhWorkflowController.hpp"

namespace evp {
namespace grasshopper {

namespace {

// A session that has no worker behind it cannot be driven, and saying so once
// here keeps every entry point's refusal identical.
bool RequiresLiveSession (uint32_t generation, uint32_t sessionId, std::string& error)
{
    if (generation == 0) {
        error = "No Grasshopper host is running.";
        return false;
    }
    if (sessionId == 0) {
        error = "No Grasshopper workflow session is open.";
        return false;
    }
    return true;
}

} // namespace

void GhWorkflowController::SetSender (Sender newSender)
{
    std::lock_guard<std::mutex> lock (mutex);
    sender = std::move (newSender);
}

void GhWorkflowController::SetSolutionHandler (SolutionHandler handler)
{
    std::lock_guard<std::mutex> lock (mutex);
    solutionHandler = std::move (handler);
}

void GhWorkflowController::SetStatusHandler (StatusHandler handler)
{
    std::lock_guard<std::mutex> lock (mutex);
    statusHandler = std::move (handler);
}

void GhWorkflowController::SetDebounce (uint32_t milliseconds)
{
    std::lock_guard<std::mutex> lock (mutex);
    debounceMs = milliseconds;
}

uint32_t GhWorkflowController::HostGeneration () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return hostGeneration;
}

uint32_t GhWorkflowController::SessionId () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return sessionId;
}

void GhWorkflowController::OnHostGeneration (uint32_t generation)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (generation == hostGeneration)
            return;

        hostGeneration = generation;
        // Every session belonged to the process that is gone. The id is dropped
        // rather than reused: reusing it would let a late message from the old
        // worker address the new session, which is precisely the confusion the
        // generation exists to prevent.
        sessionId = 0;
        state = protocol::SessionState::Empty;
        failure = protocol::FailureCode::None;
        message.clear ();
        schemaJson.clear ();
        pendingValid = false;
        awaitingSolution = false;
        requestRevision = 0;
        sentRevision = 0;

        // Kept and marked, not cleared: §11 wants the last complete preview to
        // survive so the user can still see what they had.
        if (hasSolution)
            solutionStale = true;

        status = StatusLocked ();
    }
    PublishStatus (status);
}

void GhWorkflowController::OnHostGone (const std::string& reason)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        sessionId = 0;
        state = protocol::SessionState::Empty;
        failure = protocol::FailureCode::HostDisconnected;
        message = reason;
        pendingValid = false;
        awaitingSolution = false;
        if (hasSolution)
            solutionStale = true;
        status = StatusLocked ();
    }
    PublishStatus (status);
}

uint32_t GhWorkflowController::OpenSession (protocol::SessionMode requestedMode, std::string& error)
{
    WorkflowStatus status;
    uint32_t opened = 0;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (hostGeneration == 0) {
            error = "No Grasshopper host is running, so a workflow session cannot be opened.";
            return 0;
        }

        // §13 opens one session at a time. Refused rather than silently closing
        // the first: a second Open is a caller that has lost track of the one it
        // has, and closing that one under it would take its loaded document with
        // it.
        if (sessionId != 0) {
            error = "This Grasshopper host already has an open workflow session.";
            return 0;
        }

        const uint32_t candidate = nextSessionId;
        protocol::OpenSessionPayload open;
        open.envelope = EnvelopeLocked (0);
        open.envelope.sessionId = candidate;
        open.mode = requestedMode;

        if (!SendLocked (protocol::MessageType::OpenSession, protocol::EncodeOpenSessionPayload (open), error))
            return 0;

        // Committed only after the send succeeded, so a failed send leaves no id
        // burned and no session the worker has never heard of.
        sessionId = candidate;
        ++nextSessionId;
        mode = requestedMode;
        state = protocol::SessionState::Empty;
        failure = protocol::FailureCode::None;
        message.clear ();
        opened = candidate;
        status = StatusLocked ();
    }
    PublishStatus (status);
    return opened;
}

bool GhWorkflowController::CloseSession (std::string& error)
{
    WorkflowStatus status;
    bool closed = false;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (sessionId == 0) {
            error = "There is no open Grasshopper workflow session to close.";
            return false;
        }

        protocol::CloseSessionPayload close;
        close.envelope = EnvelopeLocked (0);
        const bool sent =
            SendLocked (protocol::MessageType::CloseSession, protocol::EncodeCloseSessionPayload (close), error);

        // Closed locally whether or not the message got out. A session the host
        // has stopped driving is closed from the host's point of view, and a
        // worker that never heard the message is a worker about to be killed or
        // restarted -- either of which invalidates the session anyway.
        sessionId = 0;
        state = protocol::SessionState::Empty;
        pendingValid = false;
        awaitingSolution = false;
        schemaJson.clear ();
        if (hasSolution)
            solutionStale = true;
        status = StatusLocked ();
        closed = sent;
    }

    // Outside the scope on BOTH paths, for the deadlock reason above.
    PublishStatus (status);
    return closed;
}

bool GhWorkflowController::LoadDefinition (const std::string& path, std::string& error)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (!RequiresLiveSession (hostGeneration, sessionId, error))
            return false;
        if (path.empty ()) {
            error = "No definition was chosen.";
            return false;
        }

        protocol::LoadDefinitionPayload load;
        load.envelope = EnvelopeLocked (0);
        load.path = path;
        if (!SendLocked (protocol::MessageType::LoadDefinition, protocol::EncodeLoadDefinitionPayload (load), error))
            return false;

        definitionPath = path;
        state = protocol::SessionState::Loading;
        failure = protocol::FailureCode::None;
        message.clear ();
        // The schema and any pending snapshot belong to the PREVIOUS definition.
        // Carrying them into a new one would offer the user the old controls and
        // then send values keyed by ids the new definition may not declare.
        schemaJson.clear ();
        pendingValid = false;
        awaitingSolution = false;
        status = StatusLocked ();
    }
    PublishStatus (status);
    return true;
}

bool GhWorkflowController::RequestSchema (std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (!RequiresLiveSession (hostGeneration, sessionId, error))
        return false;

    protocol::GetSchemaPayload request;
    request.envelope = EnvelopeLocked (0);
    return SendLocked (protocol::MessageType::GetSchema, protocol::EncodeGetSchemaPayload (request), error);
}

void GhWorkflowController::SetInputs (const std::vector<protocol::SessionInputValue>& inputs, uint32_t wants)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        pendingInputs = inputs;
        pendingValid = true;
        pendingWants = wants;
        // The settle window restarts on every change, which is what makes it a
        // settle window rather than a rate limit: a drag that never stops moving
        // never solves, and the instant it stops it solves once.
        pendingSinceMs = 0;
        status = StatusLocked ();
    }
    PublishStatus (status);
}

bool GhWorkflowController::Tick (uint64_t nowMs)
{
    WorkflowStatus status;
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (!pendingValid || sessionId == 0)
            return false;

        if (pendingSinceMs == 0) {
            // First tick after the change starts the clock. Deliberately not
            // stamped in SetInputs: SetInputs is called from whatever thread the
            // control changed on and has no business reading a clock, and one
            // tick of latency is beneath the debounce anyway.
            pendingSinceMs = nowMs == 0 ? 1 : nowMs;
            return false;
        }

        // Guarded against a clock that went backwards rather than assuming it
        // cannot: a wrapped or reset tick count would otherwise make the window
        // enormous and the panel would look wedged.
        if (nowMs < pendingSinceMs) {
            pendingSinceMs = nowMs == 0 ? 1 : nowMs;
            return false;
        }

        if (nowMs - pendingSinceMs < (uint64_t) debounceMs)
            return false;

        std::string error;
        sent = SendSnapshotLocked (error);
        if (!sent) {
            failure = protocol::FailureCode::HostDisconnected;
            message = error;
        }
        status = StatusLocked ();
    }
    PublishStatus (status);
    return sent;
}

bool GhWorkflowController::SolveNow (std::string& error)
{
    WorkflowStatus status;
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (!RequiresLiveSession (hostGeneration, sessionId, error))
            return false;
        if (!pendingValid) {
            // A press with no snapshot still solves: the definition holds its
            // own saved values, and refusing here would make an input-less
            // definition unrunnable.
            pendingInputs.clear ();
            pendingValid = true;
        }

        sent = SendSnapshotLocked (error);
        status = StatusLocked ();
    }
    PublishStatus (status);
    return sent;
}

bool GhWorkflowController::CancelSolve (std::string& error)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (!RequiresLiveSession (hostGeneration, sessionId, error))
            return false;

        protocol::CancelSolvePayload cancel;
        cancel.envelope = EnvelopeLocked (sentRevision);
        if (!SendLocked (protocol::MessageType::CancelSolve, protocol::EncodeCancelSolvePayload (cancel), error))
            return false;

        // Not marked Cancelled here. The worker says whether the abort was
        // honoured, and Grasshopper only honours one between components -- a
        // panel that showed "cancelled" over a solve still running would be
        // lying about the one thing the user is watching.
        state = protocol::SessionState::Cancelling;
        status = StatusLocked ();
    }
    PublishStatus (status);
    return true;
}

bool GhWorkflowController::RequestDiagnostics (std::string& error)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (!RequiresLiveSession (hostGeneration, sessionId, error))
        return false;

    protocol::GetDiagnosticsPayload request;
    request.envelope = EnvelopeLocked (requestRevision);
    return SendLocked (protocol::MessageType::GetDiagnostics, protocol::EncodeGetDiagnosticsPayload (request), error);
}

bool GhWorkflowController::OnSessionEvent (const protocol::SessionAckPayload& event)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (event.envelope.hostGeneration != hostGeneration || event.envelope.sessionId != sessionId)
            return false;

        state = event.state;
        failure = event.failure;
        message = event.message;

        // A session the worker cannot recover stops being drivable here too,
        // rather than leaving the panel sending into a session that will refuse
        // everything.
        if (event.state == protocol::SessionState::HostRestartRequired) {
            sessionId = 0;
            pendingValid = false;
            awaitingSolution = false;
            if (hasSolution)
                solutionStale = true;
        }

        status = StatusLocked ();
    }
    PublishStatus (status);
    return true;
}

bool GhWorkflowController::OnSchemaResult (const protocol::SchemaResultPayload& schema)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (schema.envelope.hostGeneration != hostGeneration || schema.envelope.sessionId != sessionId)
            return false;

        schemaJson = schema.schemaJson;
        status = StatusLocked ();
    }
    PublishStatus (status);
    return true;
}

bool GhWorkflowController::OnSolutionStarted (const protocol::SolutionStartedPayload& started)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (started.envelope.hostGeneration != hostGeneration || started.envelope.sessionId != sessionId)
            return false;
        if (started.envelope.requestRevision < sentRevision)
            return false;

        state = protocol::SessionState::Solving;
        status = StatusLocked ();
    }
    PublishStatus (status);
    return true;
}

bool GhWorkflowController::OnSolutionResult (const protocol::SolutionResultPayload& result)
{
    WorkflowStatus status;
    StoredSolution published;
    bool publish = false;
    bool stale = false;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (result.envelope.hostGeneration != hostGeneration || result.envelope.sessionId != sessionId)
            return false;

        // ⚠️ THE STALENESS RULE, AND IT IS THE WHOLE REASON THIS CLASS EXISTS.
        // §7: the newest requested revision wins, and a completed stale solve is
        // collected for diagnostics but never published. `<` and not `<=`: the
        // result for the revision currently outstanding is exactly the one that
        // SHOULD be published.
        if (result.envelope.requestRevision < sentRevision) {
            state = protocol::SessionState::Loaded;
            stale = true;
            status = StatusLocked ();
        }

        // Everything after this point is the publication path, and it stays
        // inside the SAME guarded scope rather than reacquiring: a second
        // acquisition would let the generation or the session change between the
        // check above and the store below, which is the race this whole class
        // exists to close.
        if (!stale) {
            solution.hostGeneration = result.envelope.hostGeneration;
            solution.sessionId = result.envelope.sessionId;
            solution.requestRevision = result.envelope.requestRevision;
            solution.solutionRevision = result.solutionRevision;
            solution.elapsedMs = result.elapsedMs;
            solution.previewEpoch = result.previewEpoch;
            solution.previewRevision = result.previewRevision;
            solution.definitionIdentity = result.definitionIdentity;
            solution.inputSnapshotHash = result.inputSnapshotHash;
            solution.outputs = result.outputs;
            solution.diagnostics = result.diagnostics;

            hasSolution = true;
            solutionStale = false;
            awaitingSolution = false;
            state = protocol::SessionState::Published;
            failure = protocol::FailureCode::None;
            message.clear ();

            published = solution;
            publish = true;
            status = StatusLocked ();
        }
    }

    // ⚠️ EVERY PUBLISH HAPPENS OUTSIDE THE LOCK, THE STALE PATH INCLUDED.
    // PublishStatus takes the mutex to copy the handler out, so publishing from
    // inside the guarded scope self-deadlocks -- on the one path a caller is
    // least likely to exercise by hand. Caught by
    // GhWorkflowController.TheNewestRequestedRevisionWins.
    if (stale) {
        // Accepted as a message, refused as a publication. The distinction
        // matters to the caller's log: this is not a protocol fault.
        PublishStatus (status);
        return true;
    }

    if (publish) {
        SolutionHandler handler;
        {
            // Copied out and called OUTSIDE the lock, the way GhBridge does it:
            // the handler draws, and drawing may come back through this
            // controller for the values it is drawing.
            std::lock_guard<std::mutex> lock (mutex);
            handler = solutionHandler;
        }
        if (handler)
            handler (published);
    }

    PublishStatus (status);
    return true;
}

bool GhWorkflowController::OnSolutionFailed (const protocol::SolutionFailedPayload& payload)
{
    WorkflowStatus status;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (payload.envelope.hostGeneration != hostGeneration || payload.envelope.sessionId != sessionId)
            return false;
        if (payload.envelope.requestRevision < sentRevision)
            return true; // a stale failure is not news

        awaitingSolution = false;
        failure = payload.failure;
        message = payload.message;
        // Loaded, not Invalid: the definition is still loaded and still
        // solvable, and a failed solve does not make it a bad definition. The
        // exception is a definition that would not read, which arrives as a
        // session event rather than as a solve failure.
        state = protocol::SessionState::Loaded;

        // ⚠️ THE PREVIOUS SOLUTION SURVIVES A FAILED SOLVE. §11: keep the last
        // complete preview after a solve failure unless the user clears it. What
        // it does NOT survive is being called current -- it is still the
        // previous revision, and Apply still names that revision explicitly.
        status = StatusLocked ();
    }
    PublishStatus (status);
    return true;
}

WorkflowStatus GhWorkflowController::Status () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return StatusLocked ();
}

std::string GhWorkflowController::SchemaJson () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return schemaJson;
}

bool GhWorkflowController::CurrentSolution (StoredSolution& out) const
{
    std::lock_guard<std::mutex> lock (mutex);
    if (!hasSolution)
        return false;
    out = solution;
    return true;
}

bool GhWorkflowController::SolutionForApply (uint32_t wantedRevision, StoredSolution& out, std::string& error) const
{
    std::lock_guard<std::mutex> lock (mutex);
    if (!hasSolution) {
        error = "There is no Grasshopper solution to apply.";
        return false;
    }

    if (solutionStale) {
        error = "The Grasshopper session that produced this solution is gone. Solve again before applying it.";
        return false;
    }

    if (solution.solutionRevision != wantedRevision) {
        // Named rather than silently substituted. The revision is what the user
        // previewed, and applying a different one because it is the one still in
        // hand is exactly the substitution §7 exists to prevent.
        error = "That Grasshopper solution is no longer current. Solve again before applying it.";
        return false;
    }

    out = solution;
    return true;
}

protocol::SessionEnvelope GhWorkflowController::EnvelopeLocked (uint32_t revision) const
{
    protocol::SessionEnvelope envelope;
    envelope.hostGeneration = hostGeneration;
    envelope.sessionId = sessionId;
    envelope.requestRevision = revision;
    return envelope;
}

bool GhWorkflowController::SendLocked (protocol::MessageType type, const std::vector<uint8_t>& payload,
                                       std::string& error)
{
    if (!sender) {
        error = "The Grasshopper bridge is not connected.";
        return false;
    }
    return sender (type, payload, error);
}

bool GhWorkflowController::SendSnapshotLocked (std::string& error)
{
    // ⚠️ ONE REVISION FOR THE PAIR, AND THE INPUTS GO FIRST. The worker applies
    // a snapshot when it arrives and solves what it holds; a solve that
    // overtook its own inputs would solve the previous ones and stamp the new
    // revision on the answer -- a wrong result that looks perfectly current.
    // The pipe is ordered, so sending inputs first is enough.
    ++requestRevision;

    protocol::SetInputsPayload inputs;
    inputs.envelope = EnvelopeLocked (requestRevision);
    inputs.inputs = pendingInputs;
    if (!SendLocked (protocol::MessageType::SetInputs, protocol::EncodeSetInputsPayload (inputs), error))
        return false;

    protocol::SolvePayload solve;
    solve.envelope = EnvelopeLocked (requestRevision);
    solve.wants = pendingWants;
    if (!SendLocked (protocol::MessageType::Solve, protocol::EncodeSolvePayload (solve), error))
        return false;

    sentRevision = requestRevision;
    awaitingSolution = true;
    pendingValid = false;
    pendingSinceMs = 0;
    return true;
}

WorkflowStatus GhWorkflowController::StatusLocked () const
{
    WorkflowStatus status;
    status.state = state;
    status.mode = mode;
    status.failure = failure;
    status.message = message;
    // Busy from the moment the input changed, not from the worker's Solving --
    // that lags by a round trip, and the spinner has to start when the user
    // moves the slider.
    status.busy = awaitingSolution || pendingValid;
    status.hasCurrentSolution = hasSolution && !solutionStale;
    status.currentSolutionRevision = hasSolution ? solution.solutionRevision : 0;
    return status;
}

void GhWorkflowController::PublishStatus (const WorkflowStatus& status) const
{
    StatusHandler handler;
    {
        std::lock_guard<std::mutex> lock (mutex);
        handler = statusHandler;
    }
    if (handler)
        handler (status);
}

} // namespace grasshopper
} // namespace evp
