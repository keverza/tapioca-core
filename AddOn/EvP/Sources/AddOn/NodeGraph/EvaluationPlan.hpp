#ifndef EVP_NODEGRAPH_EVALUATIONPLAN_HPP
#define EVP_NODEGRAPH_EVALUATIONPLAN_HPP

// What one evaluation intends to do, computed before it does any of it.
//
// The plan is where "evaluate this" stops meaning "cook the whole document", and
// where everything that can be known to be impossible is rejected while nothing
// has run yet: a cycle, a missing node type, an unreachable host, an unsampleable
// generation domain, a side effect nobody asked for.
//
// It is temporary and never persisted.

#include "NodeGraph/Graph.hpp"
#include "NodeGraph/ProjectGenerations.hpp"
#include "NodeGraph/RunContext.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

class NodeRegistry;

enum class EvaluationMode {
    // Reuse every cache entry that is still valid.
    Incremental,

    // Ignore the cache and re-execute the whole closure. Diagnostic use.
    Forced,
};

struct EvaluationRequest {
    // Empty means "the document's terminal nodes" - still not "every node",
    // because a disconnected upstream branch feeding nothing stays cold.
    std::vector<NodeId> targets;

    EvaluationMode mode = EvaluationMode::Incremental;

    // Permission for HostUiWrite nodes, and it defaults to REFUSED.
    //
    // A graph that reaches out and changes the user's selection while they are
    // editing is a defect, so a preview, a watch and an auto-evaluated branch
    // must never do it. Only a deliberate Run sets this. A plan containing an
    // effectful node without it is not an error: the node is reported as
    // skipped, with the reason, and the rest of the graph still evaluates.
    bool allowSideEffects = false;

    // How many node bodies may run at once, counting the coordinator thread.
    // 0 lets the runtime decide; 1 is fully sequential.
    //
    // A REQUEST-LEVEL KNOB BECAUSE ADR-007'S GATE IS AN A/B. "Pure nodes
    // demonstrably execute concurrently" is shown by running one graph at 1 and
    // at N and comparing the measured overlap, and a client that has to be
    // rebuilt between the two arms cannot show it on the user's machine with
    // the user's graph.
    size_t maxParallel = 0;
};

struct EvaluationPlan {
    RunId runId = kNoRun;
    uint64_t graphRevision = 0;
    EvaluationMode mode = EvaluationMode::Incremental;

    // The upstream closure of the targets, in dependency order.
    std::vector<NodeId> requiredNodes;

    // requiredNodes minus deferredNodes, in dependency order. Everything that
    // can run without permission to change host state.
    std::vector<NodeId> primaryNodes;

    // Effectful nodes and everything downstream of them, in dependency order.
    // Run only after every primary node succeeded and the run was not cancelled,
    // so a failed graph never leaves the user with a selection they did not ask
    // for.
    std::vector<NodeId> deferredNodes;

    // primaryNodes partitioned into independent groups.
    std::vector<std::vector<NodeId>> levels;

    std::vector<NodeId> targets;

    // Sampled ONCE for the whole run: two nodes reading the selection in one run
    // must see the same selection or the run is not internally consistent.
    GenerationSample generations;

    // Effectful nodes present in the plan but not permitted by this request.
    // Reported rather than silently dropped.
    std::vector<NodeId> skippedEffectNodes;
};

struct PlanOutcome {
    bool accepted = false;
    std::string error;

    // Set when the document contains a cycle, so the caller can name it.
    std::vector<NodeId> cyclicNodes;

    // Set when a requested target does not exist.
    std::vector<NodeId> unknownTargets;

    EvaluationPlan plan;
};

// Validates targets, rejects cycles, rejects an oversized plan, checks that
// every node's execution domain and generation dependencies can actually be
// served, and reduces the document to the closure that has to run. Executes
// nothing and touches no cache; it does sample generations, which for the
// Archicad source means one batched read on the host thread.
PlanOutcome BuildEvaluationPlan (const GraphDocument& document, const NodeRegistry& registry,
                                 const EvaluationRequest& request, const RunContext& context);

} // namespace evp::nodegraph

#endif
