#include "NodeGraph/GraphEdit.hpp"

#include "NodeGraph/Data/AtomicValue.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/ScriptNodes.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace evp::nodegraph {
namespace {

// Whether an edge may join these two ports.
//
// ONE PREDICATE, because this question is asked twice - once when an edge is
// made, once when a re-typed node decides which of its existing edges survive -
// and two copies of it drift into a graph that accepts an edge it will later
// silently drop.
// What a port accepts once its modifier is taken into account.
//
// ⚠️ ONLY `Round` MOVES THIS. The reshaping modifiers change a tree's SHAPE and
// leave its item type alone, so they cannot make an illegal edge legal or the
// reverse. Round is the exception because it is a conversion: a port carrying it
// is exactly the port that may be fed a Double where it declared Integer, which
// is the entire point of putting it on the port.
PortModifier ModifierFor (const Node& node, const std::string& portId)
{
    const auto found = node.inputModifiers.find (portId);
    return found == node.inputModifiers.end () ? PortModifier::None : found->second;
}

ValueType AcceptedInputType (ValueType declared, PortModifier modifier)
{
    if (modifier == PortModifier::Round && declared == ValueType::Integer)
        return ValueType::Double;
    return declared;
}

bool PortTypesConnect (ValueType output, ValueType input)
{
    if (input == ValueType::Absent || output == ValueType::Absent || output == input)
        return true;
    // An Integer may travel a wire into a Double port, and the runtime converts
    // the tree as it gathers the input. The reverse is refused here so that the
    // user picks a rounding on the canvas rather than the runtime picking one
    // for them - see CanWidenItemType.
    return data::CanWidenValueType (output, input);
}

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
        if (parameter != nullptr) {
            if (parameter->valueType != value.Type ()) {
                error = "parameter type mismatch: " + parameterId;
                return false;
            }
            continue;
        }
        // A parameter named after an INPUT PORT is that input's internalised
        // value: what the node uses when nothing is wired to the port. It is
        // stored as an ordinary parameter rather than as a third kind of field
        // so it travels with the document, hashes into the evaluation cache and
        // round-trips through the library with no new plumbing. An edge always
        // wins over it; see Evaluator's input gathering.
        const PortSchema* input = FindInput (node, *nodeType, parameterId);
        if (input == nullptr) {
            error = "unknown parameter: " + parameterId;
            return false;
        }
        // Absent means "any type" on an input, exactly as it does for an edge,
        // and an Absent VALUE is how an internalised input is cleared again.
        if (value.Type () != ValueType::Absent && input->valueType != ValueType::Absent &&
            input->valueType != value.Type ()) {
            error = "internalised input type mismatch: " + parameterId;
            return false;
        }
    }
    for (const ParameterSchema& parameter : nodeType->parameters) {
        if (parameter.required && !node.parameters.contains (parameter.id) && !parameter.defaultValue) {
            error = "required parameter is absent: " + parameter.id;
            return false;
        }
    }
    // Instance ports are INPUT, from a parsed script header or a graph file, and
    // are validated on the same terms the registry validates a type's ports.
    // Skipping this would let a duplicate or empty port id reach the evaluator,
    // where an edge would match two ports and the second silently never run.
    if (!nodeType->instancePorts) {
        if (!node.dynamicInputs.empty () || !node.dynamicOutputs.empty ()) {
            error = "node carries instance ports but '" + node.nodeType + "' does not declare them";
            return false;
        }
        return true;
    }
    for (const auto* ports : { &node.dynamicInputs, &node.dynamicOutputs }) {
        std::set<std::string> seen;
        for (const PortSchema& port : *ports) {
            if (port.id.empty ()) {
                error = "instance port id is empty";
                return false;
            }
            if (!seen.insert (port.id).second) {
                error = "duplicate instance port: " + port.id;
                return false;
            }
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
        const PortSchema* output = FindOutput (*source, *registry.Find (source->nodeType), edge.sourcePort);
        const PortSchema* input = FindInput (*target, *registry.Find (target->nodeType), edge.targetPort);
        if (output == nullptr || input == nullptr) {
            error = "edge names an unknown port";
            return false;
        }
        // ⚠️ ABSENT IS THE WILDCARD ON BOTH ENDS, AND IT HAS TO BE.
        //
        // On an INPUT it always meant "any value", which is what lets one
        // inspector node take whatever is wired into it instead of needing a
        // variant per type.
        //
        // On an OUTPUT it used to be impossible, and this rule was written
        // saying so. The `tree.*` family made it real: a Graft does not change
        // what the items ARE and cannot know what they are, so it declares its
        // result Absent and forwards its input's actual type at run time. With
        // the old rule a Graft could only ever feed another wildcard port -
        // reshaping a collection and then asking its length was refused as a
        // type mismatch, which is most of the reason to reshape one.
        //
        // The run-time check is the one that still bites: Evaluator's publish
        // step compares the produced tree's item type against the port's, and a
        // port that named a type is checked there exactly as before.
        const PortModifier targetModifier = ModifierFor (*target, input->id);
        if (!PortTypesConnect (output->valueType, AcceptedInputType (input->valueType, targetModifier))) {
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
    std::vector<Edge> dropped;
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
            else if constexpr (std::is_same_v<T, SetPortModifierEdit>) {
                auto iterator = candidate.nodes_.find (operation.nodeId);
                if (iterator == candidate.nodes_.end ()) {
                    error = "unknown node: " + operation.nodeId;
                    code = "modifier.unknownNode";
                    return false;
                }
                Node& node = iterator->second;
                const NodeType* nodeType = registry.Find (node.nodeType);
                if (nodeType == nullptr) {
                    error = "unknown node type: " + node.nodeType;
                    code = "modifier.unknownNodeType";
                    return false;
                }
                const PortSchema* port = FindInput (node, *nodeType, operation.portId);
                if (port == nullptr) {
                    error = "unknown input: " + operation.portId;
                    code = "modifier.unknownPort";
                    return false;
                }
                if (ModifierFor (node, operation.portId) == operation.modifier) {
                    // Idempotent and reported as such, for the reason a mode is:
                    // an accepted no-op would dirty the whole downstream closure
                    // for nothing.
                    error = "'" + operation.nodeId + "." + operation.portId + "' is already " +
                            PortModifierName (operation.modifier);
                    code = "modifier.unchanged";
                    return false;
                }
                // Round is a conversion, so changing it changes what may be
                // connected. An edge that the new modifier would refuse is
                // reported rather than dropped: the user asked to change a port,
                // not to delete a wire.
                const ValueType accepted = AcceptedInputType (port->valueType, operation.modifier);
                for (const Edge& edge : candidate.edges_) {
                    if (edge.targetNode != operation.nodeId || edge.targetPort != operation.portId)
                        continue;
                    const auto source = candidate.nodes_.find (edge.sourceNode);
                    const NodeType* sourceType =
                        source == candidate.nodes_.end () ? nullptr : registry.Find (source->second.nodeType);
                    if (sourceType == nullptr)
                        continue;
                    const PortSchema* output = FindOutput (source->second, *sourceType, edge.sourcePort);
                    if (output != nullptr && !PortTypesConnect (output->valueType, accepted)) {
                        error = "'" + edge.sourceNode + "." + edge.sourcePort + "' could no longer connect here";
                        code = "modifier.breaksEdge";
                        return false;
                    }
                }

                if (operation.modifier == PortModifier::None)
                    node.inputModifiers.erase (operation.portId);
                else
                    node.inputModifiers[operation.portId] = operation.modifier;
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
            else if constexpr (std::is_same_v<T, SetScriptInterfaceEdit>) {
                auto iterator = candidate.nodes_.find (operation.nodeId);
                if (iterator == candidate.nodes_.end ()) {
                    error = "unknown node: " + operation.nodeId;
                    code = "script.unknownNode";
                    return false;
                }
                Node& node = iterator->second;
                const NodeType* nodeType = registry.Find (node.nodeType);
                if (nodeType == nullptr || !nodeType->instancePorts) {
                    error = "'" + node.nodeType + "' does not author its own ports";
                    code = "script.notAScriptNode";
                    return false;
                }

                node.dynamicInputs = operation.inputs;
                node.dynamicOutputs = operation.outputs;

                // A default fills a port the node has no value for. It does NOT
                // overwrite one: the number the user typed into the node is
                // theirs, and a reload that reset it on every save would make the
                // node unusable while its script was being worked on.
                for (const auto& [portId, value] : operation.defaults) {
                    if (!node.parameters.contains (portId))
                        node.parameters.emplace (portId, value);
                }
                node.parameters.insert_or_assign (kScriptSourceHashParameter, Value (operation.sourceHash));

                // Internalised values for ports that no longer exist go too.
                // Left behind they would fail ValidateNode as unknown parameters
                // the next time anything touched this node - long after the edit
                // that orphaned them.
                std::erase_if (node.parameters, [&] (const auto& entry) {
                    const std::string& parameterId = entry.first;
                    if (parameterId == kScriptPathParameter || parameterId == kScriptLanguageParameter ||
                        parameterId == kScriptSourceHashParameter)
                        return false;
                    return FindInput (node, *nodeType, parameterId) == nullptr;
                });

                // The wires. An edge survives only if its port still exists AND
                // still accepts what flows down it; a port that changed type is
                // as broken as one that was deleted, and quietly keeping it would
                // hand the evaluator a value its own rules reject.
                std::erase_if (candidate.edges_, [&] (const Edge& edge) {
                    const bool touchesSource = edge.sourceNode == operation.nodeId;
                    const bool touchesTarget = edge.targetNode == operation.nodeId;
                    if (!touchesSource && !touchesTarget)
                        return false;

                    const Node* source = candidate.FindNode (edge.sourceNode);
                    const Node* target = candidate.FindNode (edge.targetNode);
                    const NodeType* sourceType = source == nullptr ? nullptr : registry.Find (source->nodeType);
                    const NodeType* targetType = target == nullptr ? nullptr : registry.Find (target->nodeType);
                    if (sourceType == nullptr || targetType == nullptr)
                        return true;
                    const PortSchema* output = FindOutput (*source, *sourceType, edge.sourcePort);
                    const PortSchema* input = FindInput (*target, *targetType, edge.targetPort);
                    const bool survives =
                        output != nullptr && input != nullptr &&
                        PortTypesConnect (output->valueType,
                                          AcceptedInputType (input->valueType, ModifierFor (*target, input->id)));
                    if (survives)
                        return false;
                    dropped.push_back (edge);
                    return true;
                });

                for (const Edge& edge : candidate.edges_)
                    if (edge.sourceNode == operation.nodeId)
                        dirtyRoots.insert (edge.targetNode);
                for (const Edge& edge : dropped)
                    if (edge.targetNode != operation.nodeId)
                        dirtyRoots.insert (edge.targetNode);
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
        return { false, code, error, {}, document.Revision (), {} };

    candidate.revision_ = document.revision_ + 1;
    const std::vector<NodeId> dirtyNodes = Descendants (candidate, std::move (dirtyRoots));
    document = std::move (candidate);
    return { true, {}, {}, dirtyNodes, document.Revision (), std::move (dropped) };
}

} // namespace evp::nodegraph
