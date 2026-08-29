#include "NodeGraph/EvaluationPlan.hpp"

#include "NodeGraph/ArchicadHost.hpp"
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

    // Capability: can every node in this plan actually run here? Answering now
    // is the difference between "this graph needs an open project" and a GUID in
    // an error message halfway through a run.
    GenerationSet needed;
    std::set<NodeId> effectful;
    for (const NodeId& nodeId : outcome.plan.requiredNodes) {
        const NodeType& nodeType = *registry.Find (document.FindNode (nodeId)->nodeType);
        if (nodeType.executionDomain == ExecutionDomain::ArchicadMainThread &&
            (context.archicad == nullptr || !context.archicad->IsAvailable ())) {
            outcome.error = "node " + nodeId + " (" + nodeType.label + ") needs an open Archicad project";
            return outcome;
        }
        for (const GenerationDomain domain : nodeType.generations.Domains ())
            needed.Add (domain);
        if (nodeType.effect == EffectKind::HostUiWrite)
            effectful.insert (nodeId);
    }

    // Sample once, for the whole run.
    for (const GenerationDomain domain : needed.Domains ()) {
        uint64_t value = 0;
        std::string error;
        if (context.archicad == nullptr || !context.archicad->Generations ().Sample (domain, value, error)) {
            outcome.error = std::string ("the '") + GenerationDomainName (domain) +
                            "' state this graph depends on cannot be read" + (error.empty () ? "" : ": " + error);
            return outcome;
        }
        outcome.plan.generations.Set (domain, value);
    }

    // Effectful nodes, and everything downstream of them, run in a second phase.
    // Downstream too: a node consuming an effectful node's output cannot run
    // before it, so it inherits the deferral rather than being reordered.
    std::set<NodeId> deferred;
    if (!effectful.empty ()) {
        const std::vector<NodeId> roots (effectful.begin (), effectful.end ());
        for (const NodeId& nodeId : DownstreamClosure (document, roots)) {
            if (required.contains (nodeId))
                deferred.insert (nodeId);
        }
    }

    const bool permitted = request.allowSideEffects;
    for (const NodeId& nodeId : outcome.plan.requiredNodes) {
        if (!deferred.contains (nodeId)) {
            outcome.plan.primaryNodes.push_back (nodeId);
            continue;
        }
        if (permitted)
            outcome.plan.deferredNodes.push_back (nodeId);
        else
            outcome.plan.skippedEffectNodes.push_back (nodeId);
    }

    for (const std::vector<NodeId>& level : topo.levels) {
        std::vector<NodeId> filtered;
        for (const NodeId& nodeId : level) {
            if (required.contains (nodeId) && !deferred.contains (nodeId))
                filtered.push_back (nodeId);
        }
        if (!filtered.empty ())
            outcome.plan.levels.push_back (std::move (filtered));
    }

    outcome.accepted = true;
    return outcome;
}

} // namespace evp::nodegraph
