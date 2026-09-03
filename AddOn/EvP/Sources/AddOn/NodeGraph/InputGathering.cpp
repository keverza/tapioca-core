#include "NodeGraph/InputGathering.hpp"

#include "NodeGraph/NodeLifting.hpp"
#include "NodeGraph/NodeRegistry.hpp"

namespace evp::nodegraph {
namespace {

void CombineHash (size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

void CombineText (size_t& seed, const std::string& text)
{
    CombineHash (seed, std::hash<std::string> {}(text));
}

} // namespace

bool ApplyPortModifier (PortModifier modifier, const data::TreeValue& input, data::TreeValue& result,
                        std::string& error)
{
    switch (modifier) {
        case PortModifier::None:
            result = input;
            return true;
        case PortModifier::Flatten:
            return data::FlattenTreeValue (input, result, error);
        case PortModifier::Graft:
            return data::GraftTreeValue (input, result, error);
        case PortModifier::Simplify:
            return data::SimplifyTreeValue (input, result, error);
        case PortModifier::Reverse:
            return data::ReverseTreeValue (input, result, error);
        case PortModifier::Round:
            result = data::RoundTreeValue (input);
            return true;
        case PortModifier::Normalise:
            result = data::NormaliseTreeValue (input);
            return true;
    }
    result = input;
    return true;
}

bool GatherNodeInputs (const GraphDocument& document, const NodeId& nodeId, const Node& node, const NodeType& nodeType,
                       const UpstreamResultLookup& upstream, TreeMap& inputs, size_t& inputHash, std::string& error)
{
    for (const PortSchema& input : ResolvedInputs (node, nodeType)) {
        const data::ItemType itemType = PortItemType (input);
        std::vector<data::TreeValue> wired;
        for (const Edge& edge : document.Edges ()) {
            if (edge.targetNode != nodeId || edge.targetPort != input.id)
                continue;
            const std::shared_ptr<const NodeResult> sourceResult = upstream (edge.sourceNode);
            if (!sourceResult || !sourceResult->outputs.contains (edge.sourcePort)) {
                error = "upstream output is absent: " + edge.sourceNode + "." + edge.sourcePort;
                return false;
            }
            // The tree crosses the wire UNCHANGED (§8.1): connecting an edge
            // does not flatten, graft or retype what it carries, and the shared
            // pointer means a large upstream result is not copied per consumer.
            //
            // The ONE exception is a declared widening - an Integer result
            // reaching a Double port - which GraphEdit already allowed when the
            // edge was made. Doing it HERE, once, is what lets every node body
            // trust that its argument really is the type its port declared;
            // leaving it to the bodies would mean each of them handling both,
            // and the first one that forgot would throw on a variant access.
            // WidenTreeValue returns the same pointer when nothing applies, so
            // the common path is untouched.
            wired.push_back (data::WidenTreeValue (sourceResult->outputs.at (edge.sourcePort), itemType));
            CombineText (inputHash, edge.sourceNode);
            CombineText (inputHash, edge.sourcePort);
            CombineHash (inputHash, static_cast<size_t> (sourceResult->outputRevision));
        }

        if (wired.empty ()) {
            // Nothing is wired to this port, so the node falls back to the
            // input's INTERNALISED value - a parameter stored under the input's
            // own id, which is how a typed-in number reaches a port that has no
            // declared parameter. GraphEdit's ValidateNode is what keeps the
            // type honest, and the value is already folded into inputHash above
            // with every other parameter, so retyping it invalidates the cache.
            //
            // An edge always wins: this branch is only reached when there is none.
            const auto internalised = node.parameters.find (input.id);
            if (internalised != node.parameters.end () && internalised->second.Type () != ValueType::Absent) {
                data::TreeValue tree;
                std::string treeError;
                if (!TreeFromValue (internalised->second, itemType, tree, treeError)) {
                    error = "internalised value for '" + input.id + "': " + treeError;
                    return false;
                }
                inputs.emplace (input.id, std::move (tree));
            }
            else if (input.required) {
                error = "required input is unconnected: " + input.id;
                return false;
            }
            else {
                // The EMPTY tree of the port's type, never a null pointer: a
                // node body is never handed "no tree", only a tree with nothing
                // in it (§7.5 keeps absent a port state, not a tree state).
                inputs.emplace (input.id, data::EmptyTreeValue (itemType));
            }
        }
        else if (wired.size () == 1) {
            inputs.emplace (input.id, std::move (wired.front ()));
        }
        else {
            // Fan-in, and the port's DECLARED contract decides it (§7.1.2
            // decision 3). The trees arrive in document edge order; what makes
            // that a contract rather than an accident is that the port said it
            // accepts several and the policy below is written down.
            data::FanInContract contract;
            data::TreeValue merged;
            std::string mergeError;
            if (!data::MergeTreeValues (wired, contract, merged, mergeError)) {
                error = "input '" + input.id + "': " + mergeError;
                return false;
            }
            inputs.emplace (input.id, std::move (merged));
        }
    }
    // ⚠️ AFTER EVERYTHING ELSE, AND ON THE SETTLED TREE. A modifier describes
    // what the PORT receives, so it runs once on whatever arrived - one edge,
    // several merged, an internalised value, or the empty tree - rather than per
    // edge. Grafting a port fed by three wires must graft the combined result;
    // grafting each wire first and merging after is a different tree, and the
    // one nobody drew.
    for (const auto& [portId, modifier] : node.inputModifiers) {
        if (modifier == PortModifier::None)
            continue;
        const auto found = inputs.find (portId);
        if (found == inputs.end ())
            continue; // A modifier on a port the type no longer declares.
        data::TreeValue modified;
        if (!ApplyPortModifier (modifier, found->second, modified, error)) {
            error = "input '" + portId + "' (" + PortModifierName (modifier) + "): " + error;
            return false;
        }
        found->second = std::move (modified);
        // The modifier is part of what the node computed FROM, so a graph that
        // differs only by one modifier must miss the cache.
        CombineText (inputHash, portId);
        CombineText (inputHash, PortModifierName (modifier));
    }

    return true;
}

} // namespace evp::nodegraph
