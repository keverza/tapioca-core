#include "NodeGraph/GraphEdit.hpp"

#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace evp::nodegraph {
namespace {

bool SameEdge (const Edge& first, const Edge& second)
{
    return first.sourceNode == second.sourceNode && first.sourcePort == second.sourcePort &&
           first.targetNode == second.targetNode && first.targetPort == second.targetPort;
}

bool ValidateNode (const Node& node, const NodeRegistry& registry, std::string& error)
{
    if (node.id.empty ()) {
        error = "node id is empty";
        return false;
    }
    const NodeType* nodeType = registry.Find (node.nodeType);
    if (nodeType == nullptr) {
        error = "unknown node type: " + node.nodeType;
        return false;
    }
    for (const auto& [parameterId, value] : node.parameters) {
        const ParameterSchema* parameter = FindParameter (*nodeType, parameterId);
        if (parameter == nullptr) {
            error = "unknown parameter: " + parameterId;
            return false;
        }
        if (parameter->valueType != value.Type ()) {
            error = "parameter type mismatch: " + parameterId;
            return false;
        }
    }
    for (const ParameterSchema& parameter : nodeType->parameters) {
        if (parameter.required && !node.parameters.contains (parameter.id) && !parameter.defaultValue) {
            error = "required parameter is absent: " + parameter.id;
            return false;
        }
    }
    return true;
}

bool ValidateDocument (const GraphDocument& document, const NodeRegistry& registry, std::string& error)
{
    for (const auto& [nodeId, node] : document.Nodes ()) {
        if (nodeId != node.id || !ValidateNode (node, registry, error))
            return false;
    }

    for (size_t index = 0; index < document.Edges ().size (); ++index) {
        const Edge& edge = document.Edges ()[index];
        const Node* source = document.FindNode (edge.sourceNode);
        const Node* target = document.FindNode (edge.targetNode);
        if (source == nullptr || target == nullptr) {
            error = "edge names an unknown node";
            return false;
        }
        const PortSchema* output = FindOutput (*registry.Find (source->nodeType), edge.sourcePort);
        const PortSchema* input = FindInput (*registry.Find (target->nodeType), edge.targetPort);
        if (output == nullptr || input == nullptr) {
            error = "edge names an unknown port";
            return false;
        }
        // An input declared Absent accepts ANY value. Nothing produces Absent as
        // an output, so the type is free to mean "any" without becoming
        // ambiguous - which is what lets an inspector node take whatever is
        // wired into it instead of needing one variant per type.
        if (input->valueType != ValueType::Absent && output->valueType != input->valueType) {
            error = "port type mismatch";
            return false;
        }
        for (size_t other = 0; other < index; ++other) {
            const Edge& existing = document.Edges ()[other];
            if (SameEdge (existing, edge)) {
                error = "edge already exists";
                return false;
            }
            if (!input->acceptsMultiple && existing.targetNode == edge.targetNode &&
                existing.targetPort == edge.targetPort) {
                error = "input port is already occupied";
                return false;
            }
        }
    }

    const TopoResult topo = BuildTopoOrder (document);
    if (!topo.IsAcyclic ()) {
        error = "cycle involving";
        for (const NodeId& nodeId : topo.cyclicNodes)
            error += " " + nodeId;
        return false;
    }
    return true;
}

// Stage F3/F4's one legality rule, in one place.
//
// ⚠️ IT IS CHECKED ON EVERY INGRESS, NOT JUST ON setExecutionMode. A graph file
// is a second way into the document, and a hand-edited one saying "holding" on a
// node type that cannot hold would otherwise load into a state no command could
// have produced - after which the evaluator's mode branches are reasoning about
// a node whose type never agreed to it.
bool ModeIsLegal (const NodeType& nodeType, ExecutionMode mode, std::string& error, std::string& code)
{
    if (mode == ExecutionMode::Bypassed && nodeType.bypassMappings.empty ()) {
        error = "'" + nodeType.id + "' declares no bypass mapping, so it cannot be bypassed";
        code = "mode.bypassUnsupported";
        return false;
    }
    if (mode == ExecutionMode::Holding && !nodeType.holdCapable) {
        error = "'" + nodeType.id + "' is not a hold-capable node";
        code = "mode.holdUnsupported";
        return false;
    }
    return true;
}

std::vector<NodeId> Descendants (const GraphDocument& document, std::set<NodeId> dirty)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Edge& edge : document.Edges ()) {
            if (dirty.contains (edge.sourceNode) && dirty.insert (edge.targetNode).second)
                changed = true;
        }
    }
    return { dirty.begin (), dirty.end () };
}

} // namespace

EditResult ApplyEdit (GraphDocument& document, const NodeRegistry& registry, const GraphEdit& edit)
{
    GraphDocument candidate = document;
    std::set<NodeId> dirtyRoots;
    std::string error;
    // The generic code every pre-Stage-F rejection still carries. Phase 0 gives
    // each of those its own; the flow-control operations below set theirs now,
    // because F3 and F5 require a client to distinguish "this type cannot be
    // bypassed" from "that node does not exist" without reading prose.
    std::string code = "edit.rejected";

    const bool changed = std::visit (
        [&] (const auto& operation) {
            using T = std::decay_t<decltype (operation)>;
            if constexpr (std::is_same_v<T, AddNodeEdit>) {
                if (candidate.nodes_.contains (operation.node.id)) {
                    error = "node already exists: " + operation.node.id;
                    return false;
                }
                // A node may ARRIVE already in a mode - that is how a saved graph
                // restores one - so the same legality rule applies here. An
                // unknown type is left to ValidateDocument, which owns that
                // rejection for every operation.
                if (const NodeType* addedType = registry.Find (operation.node.nodeType); addedType != nullptr) {
                    if (!ModeIsLegal (*addedType, operation.node.executionMode, error, code))
                        return false;
                }
                candidate.nodes_.emplace (operation.node.id, operation.node);
                dirtyRoots.insert (operation.node.id);
            }
            else if constexpr (std::is_same_v<T, RemoveNodeEdit>) {
                if (!candidate.nodes_.contains (operation.nodeId)) {
                    error = "unknown node: " + operation.nodeId;
                    return false;
                }
                for (const Edge& edge : candidate.edges_)
                    if (edge.sourceNode == operation.nodeId)
                        dirtyRoots.insert (edge.targetNode);
                candidate.nodes_.erase (operation.nodeId);
                std::erase_if (candidate.edges_, [&] (const Edge& edge) {
                    return edge.sourceNode == operation.nodeId || edge.targetNode == operation.nodeId;
                });
            }
            else if constexpr (std::is_same_v<T, RemoveElementsEdit>) {
                if (operation.nodeIds.empty () && operation.edges.empty ()) {
                    error = "removeElements requires at least one element";
                    return false;
                }
                const std::set<NodeId> removedNodes (operation.nodeIds.begin (), operation.nodeIds.end ());
                for (const NodeId& nodeId : removedNodes) {
                    if (!candidate.nodes_.contains (nodeId)) {
                        error = "unknown node: " + nodeId;
                        return false;
                    }
                }
                for (const Edge& requested : operation.edges) {
                    if (std::find_if (candidate.edges_.begin (), candidate.edges_.end (), [&] (const Edge& edge) {
                            return SameEdge (edge, requested);
                        }) == candidate.edges_.end ()) {
                        error = "edge does not exist";
                        return false;
                    }
                }
                for (const Edge& edge : candidate.edges_) {
                    const bool removesEdge =
                        removedNodes.contains (edge.sourceNode) || removedNodes.contains (edge.targetNode) ||
                        std::any_of (operation.edges.begin (), operation.edges.end (),
                                     [&] (const Edge& requested) { return SameEdge (edge, requested); });
                    if (removesEdge && !removedNodes.contains (edge.targetNode))
                        dirtyRoots.insert (edge.targetNode);
                }
                for (const NodeId& nodeId : removedNodes)
                    candidate.nodes_.erase (nodeId);
                std::erase_if (candidate.edges_, [&] (const Edge& edge) {
                    return removedNodes.contains (edge.sourceNode) || removedNodes.contains (edge.targetNode) ||
                           std::any_of (operation.edges.begin (), operation.edges.end (),
                                        [&] (const Edge& requested) { return SameEdge (edge, requested); });
                });
            }
            else if constexpr (std::is_same_v<T, ConnectEdit>) {
                candidate.edges_.push_back (operation.edge);
                dirtyRoots.insert (operation.edge.targetNode);
            }
            else if constexpr (std::is_same_v<T, DisconnectEdit>) {
                const auto iterator = std::find_if (candidate.edges_.begin (), candidate.edges_.end (),
                                                    [&] (const Edge& edge) { return SameEdge (edge, operation.edge); });
                if (iterator == candidate.edges_.end ()) {
                    error = "edge does not exist";
                    return false;
                }
                dirtyRoots.insert (iterator->targetNode);
                candidate.edges_.erase (iterator);
            }
            else if constexpr (std::is_same_v<T, SetParameterEdit>) {
                auto iterator = candidate.nodes_.find (operation.nodeId);
                if (iterator == candidate.nodes_.end ()) {
                    error = "unknown node: " + operation.nodeId;
                    return false;
                }
                iterator->second.parameters[operation.parameterId] = operation.value;
                dirtyRoots.insert (operation.nodeId);
            }
            else if constexpr (std::is_same_v<T, SetExecutionModeEdit>) {
                auto iterator = candidate.nodes_.find (operation.nodeId);
                if (iterator == candidate.nodes_.end ()) {
                    error = "unknown node: " + operation.nodeId;
                    code = "mode.unknownNode";
                    return false;
                }
                const NodeType* nodeType = registry.Find (iterator->second.nodeType);
                if (nodeType == nullptr) {
                    error = "unknown node type: " + iterator->second.nodeType;
                    code = "mode.unknownNodeType";
                    return false;
                }
                // Refused here rather than at evaluation, so the document never
                // holds a mode the evaluator would have to guess about.
                if (!ModeIsLegal (*nodeType, operation.mode, error, code))
                    return false;
                if (iterator->second.executionMode == operation.mode) {
                    // Idempotent, and reported as such rather than accepted: an
                    // accepted no-op would bump the revision and dirty the whole
                    // downstream closure for nothing.
                    error = "'" + operation.nodeId + "' is already " + ExecutionModeName (operation.mode);
                    code = "mode.unchanged";
                    return false;
                }
                iterator->second.executionMode = operation.mode;
                dirtyRoots.insert (operation.nodeId);
            }
            else if constexpr (std::is_same_v<T, ReleaseHoldingEdit>) {
                const auto iterator = candidate.nodes_.find (operation.nodeId);
                if (iterator == candidate.nodes_.end ()) {
                    error = "unknown node: " + operation.nodeId;
                    code = "release.unknownNode";
                    return false;
                }
                if (iterator->second.executionMode != ExecutionMode::Holding) {
                    error = "'" + operation.nodeId + "' is not holding, so there is nothing to release";
                    code = "release.notHolding";
                    return false;
                }
                // The document is UNCHANGED by a release - the mode stays
                // Holding and no node, edge or parameter moves. What changes is
                // the value consumers read, which lives in the evaluator. The
                // edit exists to make that change atomic, revisioned and
                // dirty-tracked like any other, so the downstream closure is
                // marked here and the promotion happens in GraphRuntimeState.
                dirtyRoots.insert (operation.nodeId);
            }
            return true;
        },
        edit.data);

    if (!changed || !ValidateDocument (candidate, registry, error))
        return { false, code, error, {}, document.Revision () };

    candidate.revision_ = document.revision_ + 1;
    const std::vector<NodeId> dirtyNodes = Descendants (candidate, std::move (dirtyRoots));
    document = std::move (candidate);
    return { true, {}, {}, dirtyNodes, document.Revision () };
}

} // namespace evp::nodegraph
