#ifndef EVP_NODEGRAPH_EVALUATIONPLAN_HPP
#define EVP_NODEGRAPH_EVALUATIONPLAN_HPP

// What one evaluation intends to do, computed before it does any of it.
//
// The plan is where "evaluate this" stops meaning "cook the whole document".
// It is temporary and never persisted.

#include "NodeGraph/Graph.hpp"
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
};

struct EvaluationPlan {
    RunId runId = kNoRun;
    uint64_t graphRevision = 0;
    EvaluationMode mode = EvaluationMode::Incremental;

    // The upstream closure of the targets, in dependency order.
    std::vector<NodeId> requiredNodes;

    // requiredNodes partitioned into independent groups.
    std::vector<std::vector<NodeId>> levels;

    std::vector<NodeId> targets;
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

// Validates targets, rejects cycles, rejects an oversized plan, and reduces the
// document to the closure that actually has to run. Pure: touches no cache and
// executes nothing.
PlanOutcome BuildEvaluationPlan (const GraphDocument& document, const NodeRegistry& registry,
                                 const EvaluationRequest& request, const RunContext& context);

} // namespace evp::nodegraph

#endif
