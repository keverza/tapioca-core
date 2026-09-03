#include "NodeGraph/NodeLifting.hpp"

#include "NodeGraph/NodeRegistry.hpp"

#include <algorithm>
#include <limits>

namespace evp::nodegraph {
namespace {

using data::AnyTreeBuilder;
using data::DataPath;
using data::InputCursor;
using data::InputRequirement;
using data::ItemType;
using data::IterationInput;
using data::IterationPlan;
using data::PortAccess;
using data::TreeValue;

// Every item of a tree, in canonical order, as one flat list. What a port that
// speaks lists receives, and what the browser projection renders.
void CollectItems (const data::IDataTree& tree, size_t maxItems, std::vector<Value>& items, bool& truncated)
{
    for (size_t listIndex = 0; listIndex < tree.ListCount (); ++listIndex) {
        const data::IDataList& list = tree.ListAt (listIndex);
        for (size_t index = 0; index < list.Size (); ++index) {
            if (items.size () >= maxItems) {
                truncated = true;
                return;
            }
            const std::optional<Value> value = list.ValueAt (index);
            // A null item is a site with no value, and Absent is how that
            // reads to a consumer that still speaks Value.
            items.push_back (value.has_value () ? *value : Value {});
        }
    }
}

// The argument one port hands the body for one iteration.
bool BuildArgument (const PortSchema& port, const TreeValue& tree, const InputCursor& cursor, Argument& argument,
                    std::string& error)
{
    switch (PortAccessOf (port)) {
        case PortAccess::Item: {
            const std::optional<Value> item = data::ItemForCursor (tree, cursor);
            argument = item.has_value () ? *item : Value {};
            return true;
        }
        case PortAccess::List: {
            std::vector<Value> items;
            if (cursor.present && cursor.listIndex < tree.tree->ListCount ()) {
                const data::IDataList& list = tree.tree->ListAt (cursor.listIndex);
                for (size_t index = 0; index < list.Size (); ++index) {
                    const std::optional<Value> value = list.ValueAt (index);
                    items.push_back (value.has_value () ? *value : Value {});
                }
            }
            argument = Argument::FromItems (std::move (items));
            return true;
        }
        case PortAccess::Tree: {
            // The same projection the browser and the panel use, so a single
            // item arrives as that item rather than as a list of one.
            argument = ProjectTreeToValue (tree);
            return true;
        }
    }

    error = "Unknown port access on " + port.id;
    return false;
}

// One body result folded into one output tree. A List-access port spreads its
// list across the site; anything else contributes exactly one item, so a body
// that produced nothing for a port leaves a null rather than a gap.
bool AppendOutput (const PortSchema& port, const DataPath& path, const ValueMap& produced, AnyTreeBuilder& builder,
                   std::string& error)
{
    const auto found = produced.find (port.id);
    if (found == produced.end () || found->second.Type () == ValueType::Absent) {
        builder.AddNull (path);
        return true;
    }

    const Argument& value = found->second;
    if (PortAccessOf (port) == PortAccess::List || value.Type () == ValueType::List) {
        if (value.Type () != ValueType::List) {
            // A single value on a list port is a list of one, not an error: a
            // body with one answer should not have to wrap it.
            return builder.Add (path, value.AsValue (), error);
        }
        for (const Value& item : value.Items ()) {
            if (item.Type () == ValueType::Absent)
                builder.AddNull (path);
            else if (!builder.Add (path, item, error))
                return false;
        }
        return true;
    }

    return builder.Add (path, value.AsValue (), error);
}

} // namespace

data::ItemType PortItemType (const PortSchema& port)
{
    const std::optional<ItemType> itemType = data::ItemTypeFromValueType (port.valueType);
    return itemType.value_or (ItemType::Any);
}

data::PortAccess PortAccessOf (const PortSchema& port)
{
    // The three cases the existing declarations actually distinguish:
    //
    //   a CONCRETE type (Double, Point3, Mesh, ...) says "one of these", so the
    //   port is walked per item and the body is lifted - this is what gives a
    //   scalar node ordinal matching over a list (§7.1.2 decision 2);
    //
    //   `List` says "a whole list at a time", so the body sees one list;
    //
    //   `Absent` is the WILDCARD, and it says nothing about shape. A port that
    //   has not said what one item is cannot be walked per item without the
    //   runtime inventing an answer, and these are the inspector and
    //   pass-through ports - panel, preview, watch, the conditional branches -
    //   which want the whole result anyway. They take the tree.
    if (port.valueType == ValueType::List)
        return PortAccess::List;
    if (port.valueType == ValueType::Absent)
        return PortAccess::Tree;
    return PortAccess::Item;
}

data::InputRequirement PortRequirement (const PortSchema& port)
{
    return port.required ? InputRequirement::MustExist : InputRequirement::MayBeMissing;
}

bool TreeFromValue (const Argument& value, data::ItemType itemType, data::TreeValue& result, std::string& error)
{
    AnyTreeBuilder builder (itemType);
    if (value.Type () == ValueType::List) {
        builder.EnsureList (DataPath::Zero ());
        for (const Value& item : value.Items ()) {
            if (item.Type () == ValueType::Absent)
                builder.AddNull (DataPath::Zero ());
            else if (!builder.Add (DataPath::Zero (), item, error))
                return false;
        }
    }
    else if (value.Type () == ValueType::Absent) {
        // Absent is not an item. An absent internalised value produces the
        // EMPTY tree, which is what an unwired optional port means.
    }
    else if (!builder.Add (DataPath::Zero (), value.AsValue (), error)) {
        return false;
    }

    result = std::move (builder).Finish ();
    return true;
}

Argument ProjectTreeToValue (const data::TreeValue& tree, size_t maxItems, bool& truncated)
{
    truncated = false;
    if (!tree.IsPresent ())
        return Argument {};

    // One item at one path is a scalar, not a list of one. Anything else keeps
    // its collection shape, because a consumer that asked for a value cannot be
    // told the difference any other way.
    if (tree.tree->ListCount () == 1 && tree.tree->ListAt (0).Size () == 1) {
        const std::optional<Value> only = tree.tree->ListAt (0).ValueAt (0);
        return only.has_value () ? Argument (*only) : Argument {};
    }

    std::vector<Value> items;
    CollectItems (*tree.tree, maxItems, items, truncated);
    return Argument::FromItems (std::move (items));
}

Argument ProjectTreeToValue (const data::TreeValue& tree)
{
    bool truncated = false;
    return ProjectTreeToValue (tree, std::numeric_limits<size_t>::max (), truncated);
}

size_t TreeItemCount (const data::TreeValue& tree)
{
    return tree.IsPresent () ? tree.tree->ItemCount () : 0;
}

ProjectedOutput ProjectOutput (const data::TreeValue& tree, size_t maxItems, size_t maxBranches)
{
    ProjectedOutput projected;
    projected.value = ProjectTreeToValue (tree, maxItems, projected.truncated);
    projected.itemCount = TreeItemCount (tree);
    if (!tree.IsPresent ())
        return projected;

    projected.itemType = tree.itemType;
    projected.branchCount = tree.tree->ListCount ();
    projected.branchesTruncated = projected.branchCount > maxBranches;

    // One budget, spent in canonical order. It is deliberately the same number
    // the flat projection was given: the two views of one output cost the same,
    // so enabling the branch view cannot double what a run sends.
    size_t remaining = maxItems;
    const size_t shown = std::min (projected.branchCount, maxBranches);
    projected.branches.reserve (shown);
    for (size_t listIndex = 0; listIndex < shown; ++listIndex) {
        const data::IDataList& list = tree.tree->ListAt (listIndex);
        ProjectedBranch branch;
        branch.path = tree.tree->Paths ()[listIndex];
        branch.itemCount = list.Size ();

        std::vector<Value> items;
        const size_t take = std::min (branch.itemCount, remaining);
        items.reserve (take);
        for (size_t index = 0; index < take; ++index) {
            const std::optional<Value> value = list.ValueAt (index);
            items.push_back (value.has_value () ? *value : Value {});
        }
        remaining -= take;
        branch.truncated = take < branch.itemCount;
        branch.value = Argument::FromItems (std::move (items));
        projected.branches.push_back (std::move (branch));
    }
    return projected;
}

bool RunLiftedNode (const NodeType& nodeType, const Node& node, const TreeMap& inputs,
                    const NodeExecutionContext& context, const NodeBodyExecutor& body, TreeMap& outputs,
                    LiftReport& report, std::string& error)
{
    // A tree-native type is not lifted at all: no plan, no per-item loop, no
    // per-value body. It is handed the trees and returns trees, which is the
    // only way a node whose job IS the shape can do that job (§9.1).
    if (nodeType.treeBody) {
        report.iterationCount = 1;
        // Every declared input is PRESENT, as an empty tree of its type when
        // nothing is wired. A tree body may therefore read its ports by name
        // without each one re-deciding what a missing port means - the same
        // guarantee the lifted path gives, and the reason "absent input" is a
        // state of the tree rather than of the map (§7.5).
        TreeMap complete = inputs;
        for (const PortSchema& port : ResolvedInputs (node, nodeType))
            complete.emplace (port.id, data::EmptyTreeValue (PortItemType (port)));
        return nodeType.treeBody (node, complete, context, outputs, error);
    }

    const std::vector<PortSchema>& inputPorts = ResolvedInputs (node, nodeType);
    const std::vector<PortSchema>& outputPorts = ResolvedOutputs (node, nodeType);

    // The trees the walk reads, in declaration order. A port with no entry is
    // an unwired optional one; it gets the empty tree of its type so that the
    // plan sees a real tree and the body sees an absent argument.
    std::vector<data::TreeValue> trees;
    std::vector<IterationInput> iterationInputs;
    trees.reserve (inputPorts.size ());
    iterationInputs.reserve (inputPorts.size ());
    for (const PortSchema& port : inputPorts) {
        const auto found = inputs.find (port.id);
        trees.push_back (found == inputs.end () ? data::EmptyTreeValue (PortItemType (port)) : found->second);
    }
    for (size_t index = 0; index < inputPorts.size (); ++index) {
        iterationInputs.push_back (IterationInput { trees[index].tree.get (), PortAccessOf (inputPorts[index]),
                                                    PortRequirement (inputPorts[index]) });
    }

    std::vector<AnyTreeBuilder> builders;
    builders.reserve (outputPorts.size ());
    for (const PortSchema& port : outputPorts)
        builders.emplace_back (PortItemType (port));

    // A node with no inputs at all still runs once, at {0}: a source node has
    // nothing to be walked over and one answer to give.
    IterationPlan plan;
    if (iterationInputs.empty ()) {
        data::Iteration single;
        single.outputPath = DataPath::Zero ();
        plan.iterations.push_back (std::move (single));
    }
    else if (!BuildIterationPlan (iterationInputs, plan, error)) {
        return false;
    }

    for (const DataPath& path : plan.emptyPaths) {
        for (AnyTreeBuilder& builder : builders)
            builder.EnsureList (path);
    }

    for (const data::Iteration& iteration : plan.iterations) {
        if (!iteration.satisfied) {
            // The body does not run, and every output takes a null at this
            // site: the shape of the answer follows the shape of the question,
            // whether or not there was anything to compute with.
            ++report.skippedCount;
            for (AnyTreeBuilder& builder : builders)
                builder.AddNull (iteration.outputPath);
            continue;
        }

        ValueMap arguments;
        for (size_t index = 0; index < inputPorts.size (); ++index) {
            Argument argument;
            const InputCursor cursor = index < iteration.cursors.size () ? iteration.cursors[index] : InputCursor {};
            if (!BuildArgument (inputPorts[index], trees[index], cursor, argument, error))
                return false;
            arguments.emplace (inputPorts[index].id, std::move (argument));
        }

        ValueMap produced;
        if (!body (node, arguments, context, produced, error))
            return false;
        ++report.iterationCount;

        for (size_t index = 0; index < outputPorts.size (); ++index) {
            if (!AppendOutput (outputPorts[index], iteration.outputPath, produced, builders[index], error)) {
                error = "output '" + outputPorts[index].id + "': " + error;
                return false;
            }
        }
    }

    for (size_t index = 0; index < outputPorts.size (); ++index)
        outputs.emplace (outputPorts[index].id, std::move (builders[index]).Finish ());
    return true;
}

} // namespace evp::nodegraph
