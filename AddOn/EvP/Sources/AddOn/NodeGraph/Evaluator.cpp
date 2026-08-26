#include "NodeGraph/Evaluator.hpp"

#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/TopoOrder.hpp"

#include <exception>
#include <chrono>
#include <set>

namespace evp::nodegraph {
namespace {

void CombineHash (size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

void Evaluator::Invalidate (const GraphDocument& document, const std::vector<NodeId>& roots)
{
    std::set<NodeId> dirty (roots.begin (), roots.end ());
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Edge& edge : document.Edges ()) {
            if (dirty.contains (edge.sourceNode) && dirty.insert (edge.targetNode).second)
                changed = true;
        }
    }
    for (const NodeId& nodeId : dirty) {
        cache_[nodeId].dirty = true;
        cache_[nodeId].status = { NodeExecutionState::Dirty, {} };
    }
}

bool Evaluator::Evaluate (const GraphDocument& document, const NodeRegistry& registry, const NodeExecutor& executor,
                          std::string& error)
{
    std::erase_if (cache_, [&document] (const auto& item) { return document.FindNode (item.first) == nullptr; });
    for (const auto& [nodeId, node] : document.Nodes ()) {
        (void) node;
        cache_.try_emplace (nodeId);
    }

    const TopoResult topo = BuildTopoOrder (document);
    if (!topo.IsAcyclic ()) {
        error = "cannot evaluate a cyclic graph";
        return false;
    }

    const auto failNode = [&] (const NodeId& failedNode, const std::string& message) {
        cache_[failedNode].status = { NodeExecutionState::Failed, message };
        std::set<NodeId> blocked { failedNode };
        bool changed = true;
        while (changed) {
            changed = false;
            for (const Edge& edge : document.Edges ())
                if (blocked.contains (edge.sourceNode) && blocked.insert (edge.targetNode).second)
                    changed = true;
        }
        blocked.erase (failedNode);
        for (const NodeId& nodeId : blocked)
            cache_[nodeId].status = { NodeExecutionState::Blocked, "upstream node failed: " + failedNode };
        error = "node " + failedNode + " failed: " + message;
        return false;
    };

    auto candidate = cache_;
    for (const NodeId& nodeId : topo.order) {
        const Node& node = *document.FindNode (nodeId);
        const NodeType* nodeType = registry.Find (node.nodeType);
        if (nodeType == nullptr) {
            error = "unknown node type: " + node.nodeType;
            return false;
        }

        ValueMap inputs;
        size_t inputHash = std::hash<std::string> {}(node.nodeType);
        for (const auto& [parameterId, value] : node.parameters) {
            CombineHash (inputHash, std::hash<std::string> {}(parameterId));
            CombineHash (inputHash, value.Hash ());
        }
        for (const ParameterSchema& parameter : nodeType->parameters) {
            if (!node.parameters.contains (parameter.id) && parameter.defaultValue) {
                CombineHash (inputHash, std::hash<std::string> {}(parameter.id));
                CombineHash (inputHash, parameter.defaultValue->Hash ());
            }
        }

        for (const PortSchema& input : nodeType->inputs) {
            Value::List values;
            for (const Edge& edge : document.Edges ()) {
                if (edge.targetNode != nodeId || edge.targetPort != input.id)
                    continue;
                const auto source = candidate.find (edge.sourceNode);
                const std::shared_ptr<const NodeResult> sourceResult =
                    source == candidate.end () ? nullptr : source->second.result;
                if (!sourceResult || !sourceResult->outputs.contains (edge.sourcePort)) {
                    return failNode (nodeId, "source output is absent: " + edge.sourceNode + "." + edge.sourcePort);
                }
                const Value& value = sourceResult->outputs.at (edge.sourcePort);
                values.push_back (value);
                CombineHash (inputHash, std::hash<std::string> {}(edge.sourceNode));
                CombineHash (inputHash, std::hash<std::string> {}(edge.sourcePort));
                CombineHash (inputHash, value.Hash ());
            }
            if (values.empty ()) {
                if (input.required) {
                    return failNode (nodeId, "required input is unconnected: " + input.id);
                }
                inputs.emplace (input.id, Value {});
            }
            else if (input.acceptsMultiple) {
                inputs.emplace (input.id, Value (std::move (values)));
            }
            else {
                inputs.emplace (input.id, std::move (values.front ()));
            }
        }

        CacheEntry& entry = candidate[nodeId];
        if (entry.result && !entry.dirty && entry.inputHash == inputHash) {
            entry.dirty = false;
            entry.status = { NodeExecutionState::Complete, {} };
            continue;
        }

        Node effectiveNode = node;
        for (const ParameterSchema& parameter : nodeType->parameters)
            if (!effectiveNode.parameters.contains (parameter.id) && parameter.defaultValue)
                effectiveNode.parameters.emplace (parameter.id, *parameter.defaultValue);
        ValueMap outputs;
        std::string nodeError;
        bool succeeded = false;
        const auto started = std::chrono::steady_clock::now ();
        try {
            succeeded = executor (effectiveNode, inputs, outputs, nodeError);
        }
        catch (const std::exception& exception) {
            nodeError = exception.what ();
        }
        catch (...) {
            nodeError = "unknown exception";
        }
        if (!succeeded) {
            return failNode (nodeId, nodeError);
        }

        for (const auto& [portId, value] : outputs) {
            const PortSchema* output = FindOutput (*nodeType, portId);
            if (output == nullptr || output->valueType != value.Type ()) {
                return failNode (nodeId, "invalid output: " + portId);
            }
        }
        for (const PortSchema& output : nodeType->outputs) {
            if (output.required && !outputs.contains (output.id)) {
                return failNode (nodeId, "omitted output: " + output.id);
            }
        }

        entry.inputHash = inputHash;
        const auto elapsed = std::chrono::steady_clock::now () - started;
        entry.result = std::make_shared<const NodeResult> (
            NodeResult { std::move (outputs), std::chrono::duration<double, std::milli> (elapsed).count () });
        entry.dirty = false;
        entry.status = { NodeExecutionState::Complete, {} };
    }

    cache_ = std::move (candidate);
    error.clear ();
    return true;
}

NodeStatus Evaluator::Status (const NodeId& nodeId) const
{
    const auto iterator = cache_.find (nodeId);
    return iterator == cache_.end () ? NodeStatus {} : iterator->second.status;
}

std::shared_ptr<const NodeResult> Evaluator::Result (const NodeId& nodeId) const
{
    const auto iterator = cache_.find (nodeId);
    return iterator == cache_.end () ? nullptr : iterator->second.result;
}

bool Evaluator::IsDirty (const NodeId& nodeId) const
{
    const auto iterator = cache_.find (nodeId);
    return iterator == cache_.end () || iterator->second.dirty;
}

} // namespace evp::nodegraph
