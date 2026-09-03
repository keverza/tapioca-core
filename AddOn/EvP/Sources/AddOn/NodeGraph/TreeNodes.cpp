#include "NodeGraph/TreeNodes.hpp"

#include "NodeGraph/Data/AnyTree.hpp"
#include "NodeGraph/Evaluator.hpp"

#include <stdexcept>
#include <utility>

namespace evp::nodegraph {
namespace {

PortSchema Port (const char* id, ValueType valueType, bool required = true)
{
    return { id, id, valueType, required, false };
}

// Named PureNode to match every other family: a tree operation reads nothing
// outside the trees it is handed, so it is Pure and runs on a worker exactly
// like a lifted node would.
NodeType PureNode (const char* id, const char* label, const char* description)
{
    NodeType type { id, label, "Tree", description };
    type.effect = EffectKind::Pure;
    return type;
}

// The one input every reshaping node in this family takes: a tree of any item
// type, because a tree operation genuinely does not care what the items are -
// that is what ValueType::Absent (the wildcard) is for, and it is why these
// nodes are the exception NodeType::treeBody exists for rather than something
// lifting could serve: a per-item walk never sees the shape to reshape.
//
// NOT required. An unconnected reshaping node is not a wiring mistake the way
// an unconnected arithmetic operand is - Grasshopper's own Flatten/Graft accept
// nothing wired and simply report the empty tree - and marking it required
// would make the evaluator refuse the node before its body ever saw the empty
// tree RunLiftedNode already promises it (NodeLifting.hpp).
PortSchema WildcardTree (const char* id)
{
    return Port (id, ValueType::Absent, false);
}

// Wraps one AnyTree.hpp reshaping call as a `TreeNodeBody`. Every node below
// differs only in which erased operation it calls, so the plumbing - read
// "tree", write "tree", surface the operation's own error - is written once
// here instead of four times with four chances to diverge.
using ReshapeFn = bool (*) (const data::TreeValue&, data::TreeValue&, std::string&);

TreeNodeBody ReshapeBody (ReshapeFn reshape)
{
    return [reshape] (const Node&, const data::TreeMap& inputs, const NodeExecutionContext&, data::TreeMap& outputs,
                      std::string& error) {
        data::TreeValue result;
        if (!reshape (inputs.at ("tree"), result, error))
            return false;
        outputs.emplace ("tree", std::move (result));
        return true;
    };
}

// tree.itemCount and tree.branchCount both reduce the whole tree to one
// integer; only which count differs; captured with a lambda per registration
// so both share this one builder instead of duplicating the AnyTreeBuilder
// dance.
data::TreeValue ScalarCount (size_t count)
{
    data::AnyTreeBuilder builder (data::ItemType::Integer);
    std::string error;
    // An in-range item count can never fail AnyTreeBuilder::Add - it is adding
    // an Integer to an Integer tree at a fixed path - so a failure here would
    // be a logic error in the builder, not a runtime condition worth a bool
    // return the two callers below would just assert past.
    if (!builder.Add (data::DataPath::Zero (), Value (static_cast<int64_t> (count)), error))
        throw std::logic_error ("ScalarCount: " + error);
    return std::move (builder).Finish ();
}

} // namespace

void RegisterTreeNodes (NodeRegistry& registry)
{
    std::string error;

    struct ReshapeEntry {
        const char* id;
        const char* label;
        const char* description;
        ReshapeFn fn;
    };

    // Flatten, graft and simplify all take exactly one wildcard tree and
    // return one of the same shape-changed shape, so the table drives the
    // registration loop instead of three copies of the same six lines.
    const ReshapeEntry reshapeEntries[] = {
        { "tree.flatten", "Flatten", "Collapses every branch into one list at the root, in canonical traversal order.",
          data::FlattenTreeValue },
        { "tree.graft", "Graft", "Gives every item its own branch, one level deeper than it started.",
          data::GraftTreeValue },
        { "tree.simplify", "Simplify",
          "Drops the longest path prefix every branch shares, without changing branch count or item order.",
          data::SimplifyTreeValue },
    };

    for (const ReshapeEntry& entry : reshapeEntries) {
        NodeType type = PureNode (entry.id, entry.label, entry.description);
        type.inputs.push_back (WildcardTree ("tree"));
        type.outputs.push_back (WildcardTree ("tree"));
        // Unambiguous: one input, one output, same id, same wildcard type - the
        // exact case BypassMapping exists to let a type declare rather than
        // leave for the editor to guess (see NodeType.hpp).
        type.bypassMappings.push_back ({ "tree", "tree" });
        type.treeBody = ReshapeBody (entry.fn);
        if (!registry.Register (std::move (type), error))
            throw std::logic_error (error);
    }

    NodeType itemCount = PureNode ("tree.itemCount", "Item Count", "The total number of items across every branch.");
    itemCount.inputs.push_back (WildcardTree ("tree"));
    itemCount.outputs.push_back (Port ("count", ValueType::Integer));
    itemCount.treeBody = [] (const Node&, const data::TreeMap& inputs, const NodeExecutionContext&,
                             data::TreeMap& outputs, std::string&) {
        const data::TreeValue& tree = inputs.at ("tree");
        outputs.emplace ("count", ScalarCount (tree.IsPresent () ? tree.tree->ItemCount () : 0));
        return true;
    };
    if (!registry.Register (std::move (itemCount), error))
        throw std::logic_error (error);

    NodeType branchCount =
        PureNode ("tree.branchCount", "Branch Count", "The number of branches (lists) the tree has.");
    branchCount.inputs.push_back (WildcardTree ("tree"));
    branchCount.outputs.push_back (Port ("count", ValueType::Integer));
    branchCount.treeBody = [] (const Node&, const data::TreeMap& inputs, const NodeExecutionContext&,
                               data::TreeMap& outputs, std::string&) {
        const data::TreeValue& tree = inputs.at ("tree");
        outputs.emplace ("count", ScalarCount (tree.IsPresent () ? tree.tree->ListCount () : 0));
        return true;
    };
    if (!registry.Register (std::move (branchCount), error))
        throw std::logic_error (error);
}

bool IsTreeNodeType (const std::string& nodeTypeId)
{
    // Prefix-matched like every other family's ids (see GeometryNodes.cpp) -
    // adding a member needs no change here.
    return nodeTypeId.rfind ("tree.", 0) == 0;
}

} // namespace evp::nodegraph
