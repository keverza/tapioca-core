#ifndef EVP_NODEGRAPH_GRAPHRUNTIMESTATE_HPP
#define EVP_NODEGRAPH_GRAPHRUNTIMESTATE_HPP

// The runtime's public face: one authoritative document, one evaluator, and the
// run lifecycle around them. Everything a client can do to the graph goes
// through here, which is what makes the browser one client among several rather
// than the owner.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/RunContext.hpp"

#include <mutex>
#include <optional>

namespace evp::nodegraph {

struct EvaluationSummary {
    bool succeeded = false;
    bool cancelled = false;
    std::string error;
    NodeId failedNode;
    std::vector<NodeId> cyclicNodes;

    RunId runId = kNoRun;
    uint64_t revision = 0;

    size_t plannedCount = 0;
    size_t executedCount = 0;
    size_t cacheHitCount = 0;
    size_t failedCount = 0;
    size_t blockedCount = 0;
};

struct RuntimeNodeResult {
    NodeId nodeId;
    NodeStatus status;
    std::shared_ptr<const NodeResult> result;
};

struct ResultsSnapshot {
    uint64_t revision = 0;
    RunId lastRunId = kNoRun;
    std::vector<RuntimeNodeResult> nodes;
};

class GraphRuntimeState final {
  public:
    static GraphRuntimeState& Get ();

    NodeRegistry Catalog () const;
    GraphDocument Document () const;
    EditResult Apply (const GraphEdit& edit);

    // Runs the request's targets. An empty target list means the document's
    // terminal nodes, never "every node".
    EvaluationSummary Evaluate (const EvaluationRequest& request);

    // Asks the run in flight to stop. Returns the run it signalled, or kNoRun.
    RunId Cancel ();

    ResultsSnapshot Results () const;

  private:
    GraphRuntimeState ();

    mutable std::mutex mutex_;
    NodeRegistry registry_;
    GraphDocument document_;
    Evaluator evaluator_;

    RunId nextRunId_ = 1;
    RunId currentRunId_ = kNoRun;
    RunId lastRunId_ = kNoRun;

    // Held outside the document lock: Cancel must be answerable while a run
    // holds that lock, or cancellation could only ever arrive after the run it
    // was meant to stop.
    mutable std::mutex runMutex_;
    std::optional<CancellationToken> currentCancellation_;
};

} // namespace evp::nodegraph

#endif
