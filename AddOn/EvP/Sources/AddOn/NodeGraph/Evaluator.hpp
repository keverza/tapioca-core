#ifndef EVP_NODEGRAPH_EVALUATOR_HPP
#define EVP_NODEGRAPH_EVALUATOR_HPP

#include "NodeGraph/EvaluationPlan.hpp"
#include "NodeGraph/Graph.hpp"
#include "NodeGraph/RunContext.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace evp::nodegraph {

using ValueMap = std::map<std::string, Value>;

struct NodeResult {
    ValueMap outputs;
    double durationMilliseconds = 0.0;

    // Bumped on every successful publication. Downstream cache keys carry this
    // number instead of a hash of the outputs, so a large mesh is never rehashed
    // to decide whether its consumer is still clean.
    uint64_t outputRevision = 0;

    // Aggregate item count across the outputs, for the editor and for the
    // output ceiling.
    size_t itemCount = 0;
};

enum class NodeExecutionState {
    Dirty,
    Complete,
    Failed,
    Blocked,
    Cancelled,
};

struct NodeStatus {
    NodeExecutionState state = NodeExecutionState::Dirty;
    std::string message;

    // True when the last run satisfied this node from cache without executing it.
    bool cacheHit = false;

    double durationMilliseconds = 0.0;
    uint64_t evaluationCount = 0;
    size_t itemCount = 0;

    // The run that last moved this node. Lets a client tell a fresh status from
    // one left over by an earlier evaluation.
    RunId runId = kNoRun;
};

struct EvaluationOutcome {
    bool succeeded = false;
    bool cancelled = false;

    RunId runId = kNoRun;
    uint64_t graphRevision = 0;

    // The first failure, if any. Per-node detail is in each node's status.
    std::string error;
    NodeId failedNode;

    std::vector<NodeId> cyclicNodes;

    std::vector<NodeId> plannedNodes;
    size_t executedCount = 0;
    size_t cacheHitCount = 0;
    size_t failedCount = 0;
    size_t blockedCount = 0;
};

// A node body. Returns false and fills `error` to fail the node. It may also
// throw, or fault outright: the barrier in FaultBarrier.hpp turns both into a
// failed node rather than a dead process.
using NodeExecutor = std::function<bool (const Node&, const ValueMap&, ValueMap&, std::string&)>;

class NodeRegistry;

class Evaluator {
  public:
    // Marks `roots` and everything downstream of them dirty. Cheap metadata
    // work: it schedules nothing and executes nothing.
    void Invalidate (const GraphDocument& document, const std::vector<NodeId>& roots);

    // Runs the upstream closure of the request's targets. Never throws.
    EvaluationOutcome Evaluate (const GraphDocument& document, const NodeRegistry& registry,
                                const NodeExecutor& executor, const EvaluationRequest& request,
                                const RunContext& context);

    std::shared_ptr<const NodeResult> Result (const NodeId& nodeId) const;
    NodeStatus Status (const NodeId& nodeId) const;
    bool IsDirty (const NodeId& nodeId) const;

    // True while an evaluation is in flight on this evaluator. A run requested
    // from inside a run is rejected rather than allowed to reenter the cache.
    bool IsRunning () const
    {
        return running_.load (std::memory_order_relaxed);
    }

  private:
    struct CacheEntry {
        bool dirty = true;
        size_t inputHash = 0;
        std::shared_ptr<const NodeResult> result;
        NodeStatus status;
    };

    // Marks `origin` failed and everything downstream of it blocked, so an
    // independent branch of the same plan can still finish.
    void BlockDownstream (const GraphDocument& document, const NodeId& origin, const std::string& reason, RunId runId,
                          size_t& blockedCount);

    std::map<NodeId, CacheEntry> cache_;
    uint64_t nextOutputRevision_ = 1;
    std::atomic<bool> running_ { false };
};

} // namespace evp::nodegraph

#endif
