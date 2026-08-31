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

class GraphRuntimeState final {
  public:
    static GraphRuntimeState& Get ();

    NodeRegistry Catalog () const;

    std::vector<GraphId> GraphIds () const;

    // Documents are created on first reference, so a client never has to open
    // one before editing it.
    GraphDocument Document (const GraphId& graphId) const;
    EditResult Apply (const GraphId& graphId, const GraphEdit& edit);

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

    // Saves the live graph `graphId` into the library under `name`.
    StoreResult SaveToLibrary (const GraphId& graphId, const std::string& name);

    // Replaces the live graph `graphId` with the library's `name`. The document
    // is swapped only after the load has fully succeeded, so a graph that
    // cannot be read leaves the one on screen alone.
    StoreResult LoadFromLibrary (const std::string& name, const GraphId& graphId);

  private:
    // One graph's whole world. Held by unique_ptr so a reference handed out
    // under the map lock stays valid while the map grows.
    struct Slot {
        GraphDocument document;
        GraphMetadata metadata;
        Evaluator evaluator;
        RunRecorder recorder;

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
