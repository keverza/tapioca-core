#ifndef EVP_NODEGRAPH_EVALUATOR_HPP
#define EVP_NODEGRAPH_EVALUATOR_HPP

#include "NodeGraph/EvaluationPlan.hpp"
#include "NodeGraph/Graph.hpp"
#include "NodeGraph/ProjectGenerations.hpp"
#include "NodeGraph/ReferenceResolver.hpp"
#include "NodeGraph/RunContext.hpp"
#include "NodeGraph/RunEvents.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace evp::nodegraph {

using ValueMap = std::map<std::string, Value>;

class IArchicadHost;

// What a node body is allowed to reach. Everything a node can touch outside its
// own inputs arrives here, which is what keeps "no node calls ACAPI directly"
// a structural property rather than a rule people remember.
struct NodeExecutionContext {
    // nullptr offline. A node whose domain needs it never runs without it: the
    // plan refuses first.
    IArchicadHost* archicad = nullptr;

    // Never null. Defaults to a resolver that answers Missing with a reason.
    const IReferenceResolver* references = nullptr;

    // Sampled once for the whole run, so every node sees one consistent view.
    GenerationSample generations;

    CancellationToken cancellation;
};

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

    // In the plan, permitted to exist, and deliberately not run: an effectful
    // node on an evaluation that did not ask for side effects. Distinct from
    // Blocked, because nothing is wrong.
    Skipped,
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
    std::vector<NodeId> skippedEffectNodes;

    size_t executedCount = 0;
    size_t cacheHitCount = 0;
    size_t failedCount = 0;
    size_t blockedCount = 0;

    // True when the deferred phase ran. False both when there was nothing to
    // defer and when the primary phase did not earn it.
    bool effectsCommitted = false;
};

// A node body. Returns false and fills `error` to fail the node. It may also
// throw, or fault outright: the barrier in FaultBarrier.hpp turns both into a
// failed node rather than a dead process.
using NodeExecutor =
    std::function<bool (const Node&, const ValueMap&, const NodeExecutionContext&, ValueMap&, std::string&)>;

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

    // Everything one phase of a run needs, so the two phases are the same code
    // called twice rather than two copies that drift.
    struct PhaseState {
        const GraphDocument* document = nullptr;
        const NodeRegistry* registry = nullptr;
        const NodeExecutor* executor = nullptr;
        const EvaluationPlan* plan = nullptr;
        const RunContext* context = nullptr;
        const NodeExecutionContext* execution = nullptr;
        EvaluationOutcome* outcome = nullptr;
        std::set<NodeId>* unusable = nullptr;
    };

    // Returns false when the run was cancelled partway.
    bool RunPhase (const std::vector<NodeId>& nodes, PhaseState& state);

    // Marks `origin` failed and everything downstream of it blocked, so an
    // independent branch of the same plan can still finish.
    void BlockDownstream (const GraphDocument& document, const NodeId& origin, const std::string& reason,
                          const RunContext& context, size_t& blockedCount);

    std::map<NodeId, CacheEntry> cache_;
    uint64_t nextOutputRevision_ = 1;
    std::atomic<bool> running_ { false };
};

} // namespace evp::nodegraph

#endif
