#ifndef EVP_GRASSHOPPER_GHWORKFLOWCONTROLLER_HPP
#define EVP_GRASSHOPPER_GHWORKFLOWCONTROLLER_HPP

// The Archicad end of one workflow session: what to load, which inputs, when to
// solve, and which result may be published.
//
// Deliberately DevKit-free, Win32-free and CLR-free — only the session protocol,
// <cstdint>, <functional>, <mutex>, <string> and <vector> — for the same reason
// HostState.hpp is. Everything the revision evidence has to be sure of is a rule
// about STATE, and a rule about state can be proved by a test instead of by a
// live Archicad run:
//
//   * a solve is not requested until the input has settled;
//   * a result older than the newest request is never published;
//   * a result from another host generation or another session is never
//     published;
//   * Apply reads a stored revision and never asks anything to solve.
//
// None of those fails loudly. A stale publication is not an error on screen — it
// is the WRONG geometry on screen with an Apply button beside it, and a live run
// cannot reliably produce the race that causes it.
//
// ⚠️ THIS IS THE CONTROLLER, NOT A RuntimeManager, AND THE DIFFERENCE IS
// DELIBERATE. HANDOFF-GHHost.md §15: a generic runtime hierarchy would be
// speculative while no second production runtime implements it. There is one
// workflow controller and it names Grasshopper's host; when a second runtime
// needs the same seam, the seam is this class's public surface.
//
// ⚠️ IT SENDS THROUGH A CALLBACK RATHER THAN CALLING GhBridge. That is what
// keeps it offline-testable, and it is also honest about the layering: this
// class decides WHAT to send and WHEN, and the bridge owns the pipe.

#include "GhSessionProtocol.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace evp {
namespace grasshopper {

// One accepted solution, copied out of the message that carried it.
//
// ⚠️ COPIED, NOT REFERENCED, AND THAT IS §15's "fetch commit payload on Apply"
// deviation in one struct. Apply must commit exactly what was previewed; a
// payload fetched from the worker at Apply time would be whatever the definition
// produces THEN, which after any edit or restart is a different thing wearing
// the same revision number.
struct StoredSolution {
    uint32_t hostGeneration = 0;
    uint32_t sessionId = 0;
    uint32_t requestRevision = 0;
    uint32_t solutionRevision = 0;
    uint32_t elapsedMs = 0;
    uint32_t previewEpoch = 0;
    uint32_t previewRevision = 0;
    std::string definitionIdentity;
    std::string inputSnapshotHash;
    std::vector<protocol::SessionOutputValue> outputs;
    std::vector<protocol::SessionDiagnostic> diagnostics;
};

// What the panel needs to render itself in one word plus one sentence.
struct WorkflowStatus {
    protocol::SessionState state = protocol::SessionState::Empty;
    protocol::SessionMode mode = protocol::SessionMode::Headless;
    protocol::FailureCode failure = protocol::FailureCode::None;
    std::string message;
    // True while a solve has been requested and no result for the newest
    // request has arrived. Distinct from state==Solving, which is the WORKER's
    // account and lags a request by a round trip -- the spinner has to start
    // when the user moves the slider, not when Rhino gets round to saying so.
    bool busy = false;
    bool hasCurrentSolution = false;
    uint32_t currentSolutionRevision = 0;
};

class GhWorkflowController {
  public:
    // Sends one encoded message. Returns false with a reason; the controller
    // records the reason and does not retry, because every message it sends is
    // either replaced by a newer one or is a user action that will be repeated.
    using Sender = std::function<bool (protocol::MessageType, const std::vector<uint8_t>&, std::string&)>;

    // Called when a solution becomes current, and when one stops being current.
    // The panel and the preview layer subscribe; nothing here draws.
    using SolutionHandler = std::function<void (const StoredSolution&)>;
    using StatusHandler = std::function<void (const WorkflowStatus&)>;

    // How long the input must sit still before a solve is requested.
    //
    // ⚠️ A PLACEHOLDER WITH A NAME, NOT A TUNED NUMBER. §14: no production
    // debounce is chosen without the input-settle-to-visible-preview
    // measurements, which need a live Archicad. 120 ms is long enough to
    // swallow a slider drag's event storm and short enough not to feel laggy,
    // and it is a parameter so the measurement can replace it without a code
    // change.
    static constexpr uint32_t DefaultDebounceMs = 120;

    GhWorkflowController () = default;

    void SetSender (Sender sender);
    void SetSolutionHandler (SolutionHandler handler);
    void SetStatusHandler (StatusHandler handler);
    void SetDebounce (uint32_t milliseconds);

    // ---- host lifecycle ------------------------------------------------

    // A worker came up under this generation. Every session from an older one is
    // invalidated: its process is gone and so is the document it held.
    //
    // ⚠️ THE STORED SOLUTION IS KEPT AND MARKED STALE RATHER THAN DROPPED. §11:
    // keep the last complete preview after a failure, and mark it visibly stale
    // when its source session is gone. Clearing it would blank the viewport at
    // the exact moment the user is trying to work out what happened.
    void OnHostGeneration (uint32_t generation);

    // The bridge lost the worker. Fails the in-flight request and makes the
    // current solution ineligible for Apply, because nothing can vouch any more
    // for what produced it.
    void OnHostGone (const std::string& reason);

    uint32_t HostGeneration () const;

    // ---- session ------------------------------------------------------

    // Opens a session and returns its id, or 0 with a reason. The id is assigned
    // HERE: the host owns the numbering because the host owns the store the
    // results land in.
    uint32_t OpenSession (protocol::SessionMode mode, std::string& error);
    bool CloseSession (std::string& error);
    uint32_t SessionId () const;

    bool LoadDefinition (const std::string& path, std::string& error);
    bool RequestSchema (std::string& error);

    // ---- inputs and solving --------------------------------------------

    // Records a new input snapshot and starts the settle window. It does NOT
    // send: SetInputs and Solve are sent together by Tick once the input has
    // stopped moving, which is what makes a drag one solve instead of forty.
    void SetInputs (const std::vector<protocol::SessionInputValue>& inputs, uint32_t wants);

    // Drives the debounce. `nowMs` is any monotonic millisecond clock; the
    // controller never reads a clock itself, which is what lets a test advance
    // time by hand instead of sleeping.
    //
    // Returns true when this tick sent a solve.
    bool Tick (uint64_t nowMs);

    // Sends the pending snapshot and a solve immediately, ignoring the settle
    // window. This is the explicit press, and a press must not wait.
    bool SolveNow (std::string& error);

    bool CancelSolve (std::string& error);
    bool RequestDiagnostics (std::string& error);

    // ---- inbound --------------------------------------------------------

    // Each returns whether the message was ACCEPTED. A refusal is never an
    // error the user sees; it is a message for a generation or a session this
    // controller is not driving, which is the ordinary shape of a restart race.
    bool OnSessionEvent (const protocol::SessionAckPayload& event);
    bool OnSchemaResult (const protocol::SchemaResultPayload& schema);
    bool OnSolutionStarted (const protocol::SolutionStartedPayload& started);
    bool OnSolutionResult (const protocol::SolutionResultPayload& result);
    bool OnSolutionFailed (const protocol::SolutionFailedPayload& failure);

    // ---- reading --------------------------------------------------------

    WorkflowStatus Status () const;

    // The last schema the worker sent, as JSON. Empty until one arrives.
    std::string SchemaJson () const;

    // The current accepted solution, or false when there is none. Copied out
    // under the lock: the caller renders on another thread than the one the
    // bridge delivered on.
    bool CurrentSolution (StoredSolution& out) const;

    // What Apply commits.
    //
    // ⚠️ IT NEVER SOLVES, AND THE ABSENCE OF A SOLVE IS THE GUARANTEE. §7: Apply
    // reads an immutable payload already copied into the store. It refuses
    // rather than refreshing when the solution is not current, because "the
    // thing you previewed is gone" is the honest answer and a silent re-solve
    // would commit something the user never saw.
    bool SolutionForApply (uint32_t solutionRevision, StoredSolution& out, std::string& error) const;

  private:
    bool SendLocked (protocol::MessageType type, const std::vector<uint8_t>& payload, std::string& error);
    bool SendSnapshotLocked (std::string& error);
    protocol::SessionEnvelope EnvelopeLocked (uint32_t requestRevision) const;
    WorkflowStatus StatusLocked () const;
    void PublishStatus (const WorkflowStatus& status) const;

    mutable std::mutex mutex;

    Sender sender;
    SolutionHandler solutionHandler;
    StatusHandler statusHandler;

    uint32_t debounceMs = DefaultDebounceMs;
    uint32_t hostGeneration = 0;
    uint32_t sessionId = 0;
    uint32_t nextSessionId = 1;

    protocol::SessionMode mode = protocol::SessionMode::Headless;
    protocol::SessionState state = protocol::SessionState::Empty;
    protocol::FailureCode failure = protocol::FailureCode::None;
    std::string message;
    std::string definitionPath;
    std::string schemaJson;

    // The snapshot waiting for the settle window to close, and when it was last
    // touched. `pendingValid` rather than an empty vector, because a definition
    // with no inputs still has a snapshot and it is the empty one.
    std::vector<protocol::SessionInputValue> pendingInputs;
    bool pendingValid = false;
    uint64_t pendingSinceMs = 0;
    uint32_t pendingWants = protocol::SolveWantsPreview | protocol::SolveWantsData;

    // Counts REQUESTS, and only ever forwards. The worker counts solutions.
    uint32_t requestRevision = 0;
    // The newest request a solve was actually sent for; a result older than this
    // is stale by definition.
    uint32_t sentRevision = 0;
    bool awaitingSolution = false;

    bool hasSolution = false;
    // Set when the solution's session or generation is gone. It stays VISIBLE
    // and stops being Applyable, which are two different things.
    bool solutionStale = false;
    StoredSolution solution;
};

} // namespace grasshopper
} // namespace evp

#endif
