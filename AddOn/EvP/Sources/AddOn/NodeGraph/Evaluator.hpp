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

// What one topological level actually did, measured rather than assumed.
struct LevelMetrics {
    size_t levelIndex = 0;

    // Nodes whose bodies ran. A cache hit or a blocked node is in the level but
    // not in this count, which is why a level can measure zero work.
    size_t executedCount = 0;
    size_t workerNodeCount = 0;
    size_t hostNodeCount = 0;

    // Wall clock across the level's execute phase.
    double wallClockMs = 0.0;

    // Sum of the node durations in it. Exceeds wallClockMs exactly to the extent
    // that the level overlapped.
    double workMs = 0.0;

    // The most node bodies observed running at one instant. Counted by the
    // bodies themselves, so it is a measurement of overlap and not an inference
    // from the timings.
    size_t peakConcurrency = 0;
};

// ADR-007's parallelism gate is "pure nodes DEMONSTRABLY execute concurrently".
// This is the demonstration, carried out of the run rather than logged: a client
// can run the same graph at maxParallel 1 and at N and compare.
struct ParallelismMetrics {
    // Pool threads available, excluding the coordinator.
    size_t workerThreads = 0;

    // The cap this run used, counting the coordinator. 1 is the sequential arm.
    size_t maxParallel = 1;

    double wallClockMs = 0.0;
    double workMs = 0.0;
    size_t peakConcurrency = 0;

    std::vector<LevelMetrics> levels;

    // Work divided by wall clock: 1.0 means nothing overlapped. Reported, never
    // asserted - a graph of one node per level has no parallelism to find and
    // that is not a defect.
    double Speedup () const
    {
        return wallClockMs > 0.0 ? workMs / wallClockMs : 0.0;
    }
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

    ParallelismMetrics parallelism;
};

// A node body. Returns false and fills `error` to fail the node. It may also
// throw, or fault outright: the barrier in FaultBarrier.hpp turns both into a
// failed node rather than a dead process.
//
// ⚠️ IT MUST BE SAFE TO CALL FROM SEVERAL THREADS AT ONCE, on different nodes.
// Worker-domain bodies run across the pool, so a body that reaches mutable
// shared state - a static, a cache of its own, a logger without a lock - is a
// data race rather than a slow node. Everything a body legitimately needs
// arrives in its arguments, which is what makes that requirement satisfiable.
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

    // Drops every cached result and status. Loading a graph replaces the
    // PROGRAM, and a cache is an answer to the old one: a node that kept its id
    // and its parameters across a load would otherwise serve the previous
    // document's result as though this document had produced it.
    void Reset ();

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

    // One node gathered on the coordinator and not yet run. Defined in the
    // implementation: it names the fault barrier's outcome type, which the
    // runtime's public header has no business pulling in.
    struct PreparedNode;

    // Gathers inputs, folds the cache key, and decides whether the node runs at
    // all. COORDINATOR THREAD ONLY - it both reads and writes the cache, which
    // is what keeps the cache single-threaded while node bodies are not.
    // Returns false when there is nothing to execute: a cache hit, a node
    // already made unusable by an upstream failure, or a preparation failure.
    bool PrepareNode (const NodeId& nodeId, PhaseState& state, PreparedNode& prepared);

    // Validates one executed node's outputs and publishes or fails it.
    // COORDINATOR THREAD ONLY.
    void PublishNode (PreparedNode& prepared, PhaseState& state);

    // Records the failure, blocks everything downstream of it, and adds that
    // closure to the run's unusable set so a later level does not try to run a
    // node whose inputs can never arrive. COORDINATOR THREAD ONLY.
    void FailNode (const NodeId& nodeId, const std::string& message, double elapsedMs, PhaseState& state);

    // Prepare every node of one level, run the worker-domain members across the
    // pool and the Archicad-domain members on this thread, then publish in
    // level order so the event stream does not depend on thread scheduling.
    // Returns false when the run was cancelled partway.
    bool RunLevel (size_t levelIndex, const std::vector<NodeId>& level, PhaseState& state);

    // Levels in order. Returns false when the run was cancelled partway.
    bool RunLevels (const std::vector<std::vector<NodeId>>& levels, PhaseState& state);

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
