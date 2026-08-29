#include "NodeGraph/EvaluationPlan.hpp"

#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <set>

namespace evp::nodegraph {

PlanOutcome BuildEvaluationPlan (const GraphDocument& document, const NodeRegistry& registry,
                                 const EvaluationRequest& request, const RunContext& context)
{
    PlanOutcome outcome;
    outcome.plan.runId = context.runId;
    outcome.plan.graphRevision = document.Revision ();
    outcome.plan.mode = request.mode;

    for (const NodeId& target : request.targets) {
        if (document.FindNode (target) == nullptr)
            outcome.unknownTargets.push_back (target);
    }
    if (!outcome.unknownTargets.empty ()) {
        outcome.error = "unknown evaluation target: " + outcome.unknownTargets.front ();
        return outcome;
    }

    // A missing node type is a plan-time rejection, not a mid-run surprise. The
    // whole point of the plan is that this class of failure is reported before
    // anything has executed.
    for (const auto& [nodeId, node] : document.Nodes ()) {
        if (registry.Find (node.nodeType) == nullptr) {
            outcome.error = "unknown node type '" + node.nodeType + "' on node " + nodeId;
            return outcome;
        }
    }

    const TopoResult topo = BuildTopoOrder (document);
    if (!topo.IsAcyclic ()) {
        outcome.cyclicNodes = topo.cyclicNodes;
        outcome.error = "the graph contains a cycle through node " + topo.cyclicNodes.front ();
        return outcome;
    }

    outcome.plan.targets = request.targets.empty () ? TerminalNodes (document) : request.targets;

    const std::vector<NodeId> closure = UpstreamClosure (document, outcome.plan.targets);
    if (closure.size () > context.limits.maxPlanNodes) {
        outcome.error = "the evaluation plan exceeds the node ceiling";
        return outcome;
    }
    const std::set<NodeId> required (closure.begin (), closure.end ());

    // Reuse the whole-document ordering rather than re-sorting the closure: the
    // topological order of a subgraph is the document order filtered to it, and
    // the same holds level by level.
    for (const NodeId& nodeId : topo.order) {
        if (required.contains (nodeId))
            outcome.plan.requiredNodes.push_back (nodeId);
    }
    for (const std::vector<NodeId>& level : topo.levels) {
        std::vector<NodeId> filtered;
        for (const NodeId& nodeId : level) {
            if (required.contains (nodeId))
                filtered.push_back (nodeId);
        }
        if (!filtered.empty ())
            outcome.plan.levels.push_back (std::move (filtered));
    }

    outcome.accepted = true;
    return outcome;
}

} // namespace evp::nodegraph
