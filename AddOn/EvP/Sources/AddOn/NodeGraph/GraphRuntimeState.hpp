#ifndef EVP_NODEGRAPH_GRAPHRUNTIMESTATE_HPP
#define EVP_NODEGRAPH_GRAPHRUNTIMESTATE_HPP

// The runtime's public face: documents held BY ID, each with its own evaluator,
// run lifecycle and observation stream. Everything a client can do to a graph
// goes through here, which is what makes the browser one client among several
// rather than the owner.
//
// Graphs are addressed by id even though the editor currently opens one. The
// alternative - a process-wide singleton document - is the shape that has to be
// unpicked later from every verb and every test at once.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/GraphReports.hpp"
#include "NodeGraph/GraphStore.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/RunContext.hpp"
#include "NodeGraph/RunHistory.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace evp::nodegraph {

// The graph a verb operates on when it names none. Keeps every existing client
// working while the contract is multi-graph underneath.
extern const char* const kDefaultGraphId;

struct EvaluationSummary {
    bool succeeded = false;
    bool cancelled = false;
    std::string error;
    NodeId failedNode;
    std::vector<NodeId> cyclicNodes;

    GraphId graphId;
    RunId runId = kNoRun;
    uint64_t revision = 0;

    // The stream position after this run, so a client can evaluate and then ask
    // for exactly the events this run produced.
    EventSeq lastEventSeq = kNoEvent;

    size_t plannedCount = 0;
    size_t executedCount = 0;
    size_t cacheHitCount = 0;
    size_t failedCount = 0;
    size_t blockedCount = 0;

    // Effectful nodes present in the plan that this run did not ask to run.
    std::vector<NodeId> skippedEffectNodes;

    // True when the deferred phase ran, so a client can say "applied" rather
    // than inferring it from a node status.
    bool effectsCommitted = false;

    // What the run's levels actually overlapped. Carried out to the client
    // because ADR-007's parallelism gate is answered by MEASURING one graph at
    // maxParallel 1 and again at N, and a number that never leaves the runtime
    // cannot answer it on the user's machine with the user's graph.
    ParallelismMetrics parallelism;
};

struct RuntimeNodeResult {
    NodeId nodeId;
    NodeStatus status;
    std::shared_ptr<const NodeResult> result;
};

struct ResultsSnapshot {
    GraphId graphId;
    uint64_t revision = 0;
    RunId lastRunId = kNoRun;

    // The sequence this snapshot is consistent with. A client stores it and asks
    // for events after it; that pairing is the whole synchronization contract.
    EventSeq lastEventSeq = kNoEvent;

    std::vector<RuntimeNodeResult> nodes;
};

// What one all-or-nothing transaction did, or refused to do.
//
// Separate from EditResult because a batch can fail for two reasons a single
// edit cannot: the document moved under the client (`expectedRevision`), and one
// member of the batch was refused (`failedIndex`). A client that only knows
// "rejected" cannot tell the user WHICH of five pasted nodes was the problem.
struct BatchEditResult {
    bool accepted = false;

    // Stable machine-readable reason, empty when accepted.
    std::string code;
    std::string error;

    // Which edit refused, when a member of the batch did. Meaningless when the
    // batch was refused before any edit ran - a revision conflict, say - which is
    // why `code` and not this is what a client branches on.
    size_t failedIndex = 0;

    std::vector<NodeId> dirtyNodes;
    uint64_t revision = 0;

    // The ids the runtime gave the nodes this transaction added, in the order
    // the adds appeared, each paired with the transaction-local alias the caller
    // used for it (empty when it used none). This is how a client learns what
    // its paste is actually called.
    std::vector<std::pair<std::string, NodeId>> assignedNodes;
};

// How many undo steps a graph keeps unless the client asks for another number.
//
// Each step is a whole document copy, so this trades memory for depth: the
// practical cost is the graph size times this number. Twenty is deep enough to
// walk back out of a wrong turn and shallow enough that a large graph does not
// quietly hold twenty copies of itself.
constexpr size_t kDefaultUndoDepth = 20;

// The range a client may ask for. Zero would mean "no undo at all", which is a
// setting nobody wants and an easy way to lose work to a typo.
constexpr size_t kMinUndoDepth = 1;
constexpr size_t kMaxUndoDepth = 200;

// Whether undo and redo have anything to do, and what they would undo.
struct HistoryState {
    bool canUndo = false;
    bool canRedo = false;
    std::string undoLabel;
    std::string redoLabel;

    // What the graph is currently keeping, so a client shows the setting in
    // force rather than the one it last asked for.
    size_t depth = kDefaultUndoDepth;

    // How deep each side currently is.
    size_t undoCount = 0;
    size_t redoCount = 0;

    // ⚠️ MONOTONIC, AND THAT IS THE WHOLE POINT OF IT. A client that keeps its
    // own metadata history - node positions, which this document does not hold -
    // has to interleave its steps with these, and so must be able to ask "did
    // the runtime record a step since I last looked?". `undoCount` cannot answer
    // that: once the stack is at `depth` a recorded step also drops the oldest
    // one, so the count is unchanged, and a coalesced edit is likewise
    // indistinguishable from no edit at all. This only ever goes up, once per
    // entry actually pushed, so the difference between two readings is exactly
    // the number of new undoable steps.
    uint64_t stepsRecorded = 0;
};

// Rejection codes this file owns. Prose moves between builds; a client's branch
// must not.
namespace batchcode {
constexpr const char* kRevisionConflict = "graph.revisionConflict";
constexpr const char* kEmptyBatch = "graph.emptyBatch";
constexpr const char* kBatchTooLarge = "graph.batchTooLarge";
constexpr const char* kUnbatchableEdit = "graph.unbatchableEdit";
constexpr const char* kNothingToUndo = "graph.nothingToUndo";
constexpr const char* kNothingToRedo = "graph.nothingToRedo";
// An alias in a transaction names a node the document already has. Refused
// rather than resolved: the client's wires would silently bind to the new node
// instead of the existing one, and nothing on screen would say so.
constexpr const char* kAliasConflict = "graph.aliasConflict";
} // namespace batchcode

// The most edits one transaction may carry. A paste is a handful of nodes; a
// number this size is a client bug or a hostile caller, and either way the
// answer is to refuse rather than to hold the document lock for a very long time.
constexpr size_t kMaxBatchEdits = 512;

class GraphRuntimeState final {
  public:
    static GraphRuntimeState& Get ();

    NodeRegistry Catalog () const;

    std::vector<GraphId> GraphIds () const;

    // Documents are created on first reference, so a client never has to open
    // one before editing it.
    GraphDocument Document (const GraphId& graphId) const;

    // One edit. Recorded on the undo stack like a batch is, because the two
    // edits the user most wants back - a deleted node and a slider they dragged
    // too far - both arrive through here rather than through ApplyBatch.
    EditResult Apply (const GraphId& graphId, const GraphEdit& edit, const std::string& label = std::string (),
                      const std::string& coalesceKey = std::string ());

    // Several edits, all or none, against the revision the client last read.
    //
    // ⚠️ THE POINT IS THE ROLLBACK, NOT THE ROUND-TRIP SAVING. Applying five
    // edits one at a time leaves the document in a state nobody designed when
    // the third is refused - four nodes pasted, one missing, and no wire between
    // them. This either moves the document all the way or not at all.
    //
    // `expectedRevision` is the revision the client built the request against.
    // Nothing else may have moved the document in between; if it did, the batch
    // is refused rather than applied to a graph the client has not seen. Pass
    // nullopt to skip the check, which is only right for a request that cannot
    // be stale.
    //
    // `label` names the transaction for undo ("Paste 5 nodes"). `coalesceKey`,
    // when non-empty, folds this transaction into the PREVIOUS undo entry if
    // that entry carried the same key - which is how a slider drag that lands
    // two hundred setParam edits becomes one Ctrl+Z. The key is opaque here; the
    // client makes it unique per gesture, because two separate drags of the same
    // slider must not collapse into one.
    BatchEditResult ApplyBatch (const GraphId& graphId, std::optional<uint64_t> expectedRevision,
                                const std::vector<GraphEdit>& edits, const std::string& label = std::string (),
                                const std::string& coalesceKey = std::string ());

    // Step the document back, and forward again.
    //
    // ⚠️ IMPLEMENTED WITH SNAPSHOTS, NOT INVERSE EDITS, AND THAT IS THE WHOLE
    // DESIGN. An inverse action has to be written once per edit kind and is
    // wrong the day an edit gains a side effect somebody forgets to mirror - the
    // classic symptom being an undo that restores a node without its wires.
    // GraphDocument is two containers and a counter, so a copy IS the inverse,
    // for every edit kind that exists and every one added later.
    BatchEditResult Undo (const GraphId& graphId);
    BatchEditResult Redo (const GraphId& graphId);
    HistoryState History (const GraphId& graphId) const;

    // How many steps this graph keeps. Clamped into [kMinUndoDepth,
    // kMaxUndoDepth]; lowering it discards the oldest steps immediately rather
    // than waiting for the next edit, so the setting means what it says as soon
    // as it is applied.
    void SetHistoryDepth (const GraphId& graphId, size_t depth);

    // Runs the request's targets. An empty target list means the document's
    // terminal nodes, never "every node".
    EvaluationSummary Evaluate (const GraphId& graphId, const EvaluationRequest& request);

    // Asks the run in flight to stop. Returns the run it signalled, or kNoRun.
    RunId Cancel (const GraphId& graphId);

    ResultsSnapshot Results (const GraphId& graphId) const;

    RunEventLog::Tail Events (const GraphId& graphId, EventSeq sinceSeq, size_t maxEvents) const;

    // One resolution pass, projected two ways. Both are computed from the same
    // walk so they cannot disagree about what the document names.
    GraphDependencyReport Dependencies (const GraphId& graphId) const;
    CompatibilityReport Compatibility (const GraphId& graphId, uint32_t formatVersion) const;

    std::vector<RunRecord> RecentRuns (const GraphId& graphId, size_t maxRuns) const;

    // Editor layout and labels for a live graph. The runtime CARRIES this and
    // never reads a member of it: §34 allows layout to live alongside the graph
    // provided the evaluator does not depend on it, and holding it here is what
    // makes node positions survive a palette that closes and reopens.
    GraphMetadata Metadata (const GraphId& graphId) const;
    void SetMetadata (const GraphId& graphId, GraphMetadata metadata);

    // The workflow library, one per process. Replaceable so a test can point the
    // runtime at a temporary directory, and so a project-embedded backend could
    // arrive without any verb changing.
    void SetLibrary (std::unique_ptr<IGraphStore> library);
    IGraphStore& Library () const;

    // One of the five actions on a selection-set node.
    //
    // ⚠️ THESE ARE NOT EVALUATIONS AND MUST NOT BECOME ONE. A button the user
    // pressed is a deliberate act, which is exactly what EvaluationRequest's
    // allowSideEffects gate exists to distinguish from a graph reaching out on
    // its own during a run. Reselect changes Archicad's selection because the
    // user asked it to, here, once - not because a node happened to be in a
    // plan.
    enum class SelectionAction {
        // Replace the set with what is selected in Archicad now.
        Update,
        // Add the current selection to the set.
        Add,
        // Take the current selection out of the set.
        Remove,
        // Select the set's elements in Archicad. Reads the set, never writes it.
        Reselect,
        // Empty the set.
        Clear,
    };

    struct SelectionActionResult {
        bool ok = false;
        std::string error;

        // The set's size after the action.
        size_t count = 0;
        // How many elements the action actually added, removed or replaced.
        size_t changed = 0;
        // References that no longer resolve. Reselect FAILS on these rather
        // than selecting the survivors; the others report them and carry on,
        // because a set that cannot drop a deleted element is unusable.
        std::vector<std::string> missing;

        uint64_t revision = 0;

        // Present when the action changed the set: the run that refreshed
        // everything the change can reach, so no one has to press Evaluate.
        std::optional<EvaluationSummary> evaluation;
    };

    SelectionActionResult ApplySelectionAction (const GraphId& graphId, const NodeId& nodeId, SelectionAction action);

    // Saves the live graph `graphId` into the library under `name`.
    StoreResult SaveToLibrary (const GraphId& graphId, const std::string& name);

    // Replaces the live graph `graphId` with the library's `name`. The document
    // is swapped only after the load has fully succeeded, so a graph that
    // cannot be read leaves the one on screen alone.
    StoreResult LoadFromLibrary (const std::string& name, const GraphId& graphId);

  private:
    struct Slot;

    // What the viewport draws, republished from the document and the cache.
    // Called with the slot's document mutex held; see the definition.
    void PublishPreviewLocked (const GraphId& graphId, Slot& slot);

    // Record `before` as one undo step. Called with the document mutex held, and
    // only once the edit is known to have been ACCEPTED - a refused edit did not
    // move the document, so an undo entry for it would be a Ctrl+Z that appears
    // to do nothing.
    void PushHistoryLocked (Slot& slot, const GraphDocument& before, const std::string& label,
                            const std::string& coalesceKey);

    // A fresh id for a node of `nodeType`, unique in this slot's document.
    // Called with the document mutex held.
    NodeId GenerateNodeIdLocked (Slot& slot, const std::string& nodeType);

    // Lift the ordinal above every id already in the document, so a graph loaded
    // from disk does not start naming nodes over the top of its own.
    void SeedNodeOrdinalLocked (Slot& slot);

    // The half of undo and redo that is the same in both directions: move the
    // document to `target`, work out what that changed, invalidate it and
    // republish. Called with the document mutex held.
    BatchEditResult StepHistoryLocked (const GraphId& graphId, Slot& slot, const GraphDocument& target);

    // One graph's whole world. Held by unique_ptr so a reference handed out
    // under the map lock stays valid while the map grows.
    // One undoable step: the document as it stood BEFORE the transaction.
    struct HistoryEntry {
        GraphDocument document;
        std::string label;
        std::string coalesceKey;
    };

    struct Slot {
        GraphDocument document;
        GraphMetadata metadata;
        Evaluator evaluator;
        RunRecorder recorder;

        // Guarded by documentMutex, because they are only ever read or written
        // in the same critical section that moves the document.
        std::vector<HistoryEntry> undoStack;
        std::vector<HistoryEntry> redoStack;
        size_t undoDepth = kDefaultUndoDepth;

        // See HistoryState::stepsRecorded. Counts pushes, not stack size.
        uint64_t stepsRecorded = 0;

        // The next ordinal a generated node id will carry.
        //
        // MONOTONIC WITHIN A SESSION, AND DELIBERATELY NOT PART OF THE DOCUMENT.
        // Deriving it from the nodes present would reuse the id of a node that
        // was just deleted, and a client keeps editor metadata - position,
        // nickname, colour - keyed by id, so the next node would silently
        // inherit a dead one's appearance. Undo must not rewind it either, for
        // the same reason, which is why it lives in the slot rather than in the
        // GraphDocument the history snapshots.
        uint64_t nextNodeOrdinal = 1;

        mutable std::mutex documentMutex;

        // Separate from documentMutex on purpose: Cancel has to be answerable
        // while a run holds the document lock, or it could never reach the run
        // it exists to stop.
        mutable std::mutex runMutex;
        RunId nextRunId = 1;
        RunId currentRunId = kNoRun;
        RunId lastRunId = kNoRun;
        std::optional<CancellationToken> currentCancellation;
    };

    GraphRuntimeState ();

    Slot& SlotFor (const GraphId& graphId) const;

    mutable std::mutex mapMutex_;
    mutable std::map<GraphId, std::unique_ptr<Slot>> graphs_;
    NodeRegistry registry_;

    mutable std::mutex libraryMutex_;
    std::unique_ptr<IGraphStore> library_;
};

} // namespace evp::nodegraph

#endif
