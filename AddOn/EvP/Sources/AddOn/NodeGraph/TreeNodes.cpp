#include "NodeGraph/TreeNodes.hpp"

#include "NodeGraph/Data/AnyTree.hpp"
#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/ParameterDescriptors.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

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

// A whole-number parameter off the node, defaulted rather than demanded: a
// tree operation with no shift typed into it is the identity, not an error.
int64_t IntegerParameter (const Node& node, const char* id, int64_t fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end ())
        return fallback;
    const auto* held = std::get_if<int64_t> (&found->second.DataValue ());
    return held == nullptr ? fallback : *held;
}

// One value chosen from a fixed set, as a Select parameter renders it.
ParameterUi ChoiceUi (const char* section, int order, const char* help, std::vector<ParameterOption> options)
{
    ParameterUi ui;
    ui.widget = ParameterWidget::Select;
    ui.section = section;
    ui.order = order;
    ui.help = help;
    ui.options = std::move (options);
    return ui;
}

// The two matching inputs `tree.zip` and `tree.crossProduct` re-match, planned
// as the engine would plan them for a lifted node - Item access on both, so
// they walk branch by branch and item by item exactly as an ordinary two-input
// node does.
std::vector<data::IterationInput> MatchingInputs (const data::TreeMap& inputs)
{
    std::vector<data::IterationInput> planned;
    for (const char* id : { "a", "b" }) {
        const data::TreeValue& tree = inputs.at (id);
        // MayBeMissing, because re-matching an empty collection against a full
        // one is a question with an answer (nothing pairs) rather than a fault.
        planned.push_back ({ tree.tree.get (), data::PortAccess::Item, data::InputRequirement::MayBeMissing });
    }
    return planned;
}

// Copies the item one cursor points at into `builder` at `path`, or a null when
// the cursor points past the end. Never drops the site: a zip whose inputs
// disagree in length must still produce two outputs of the SAME length, or the
// pairing it just computed is unreadable downstream.
bool EmitCursorItem (const data::TreeValue& tree, const data::InputCursor& cursor, const data::DataPath& path,
                     data::AnyTreeBuilder& builder, std::string& error)
{
    const std::optional<Value> item = data::ItemForCursor (tree, cursor);
    if (!item.has_value ()) {
        builder.AddNull (path);
        return true;
    }
    return builder.Add (path, *item, error);
}

} // namespace

void RegisterTreeMatchingNodes (NodeRegistry& registry);

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

    // Split out only because one function registering nine node types is a wall
    // rather than a list; the matching nodes are the same family.
    RegisterTreeMatchingNodes (registry);
}

void RegisterTreeMatchingNodes (NodeRegistry& registry)
{
    std::string error;

    // ---- tree.shiftPath ----------------------------------------------------

    NodeType shift =
        PureNode ("tree.shiftPath", "Shift Path", "Adds or removes leading path levels, without touching item order.");
    shift.inputs.push_back (WildcardTree ("tree"));
    shift.outputs.push_back (WildcardTree ("tree"));
    shift.bypassMappings.push_back ({ "tree", "tree" });
    ParameterSchema shiftAmount { "shift", "Shift", ValueType::Integer, false, Value (static_cast<int64_t> (0)) };
    // Negative deepens and positive shortens, matching ShiftTreePaths - and a
    // path cannot be shortened to nothing, which is why that operation FAILS
    // rather than clamping: an emptied path is not a path.
    shiftAmount.ui = CountUi ("Shift Path", 0, "Positive drops leading levels; negative adds them.", -8, 8);
    shift.parameters.push_back (std::move (shiftAmount));
    shift.treeBody = [] (const Node& node, const data::TreeMap& inputs, const NodeExecutionContext&,
                         data::TreeMap& outputs, std::string& error) {
        const int32_t amount = static_cast<int32_t> (IntegerParameter (node, "shift", 0));
        data::TreeValue result;
        if (!data::ShiftTreeValuePaths (inputs.at ("tree"), amount, data::PathCollision::Error, result, error))
            return false;
        outputs.emplace ("tree", std::move (result));
        return true;
    };
    if (!registry.Register (std::move (shift), error))
        throw std::logic_error (error);

    // ---- tree.zip ----------------------------------------------------------
    //
    // RE-MATCHING, NOT PAIRING. It takes two collections and returns two
    // collections of equal length, rather than one collection of pairs - there
    // is no pair item type, and inventing one to serve this node would put a
    // compound inside a container whose whole contract is that an item is
    // atomic (7.2).
    //
    // AND IT PLANS THE WALK WITH THE ENGINE THE LIFT USES. Writing the matching
    // again here would give Tapioca two answers to "what pairs with what" that
    // agree right up until the day one of them is fixed. 8.3's requirement is a
    // NAMED policy, so the policy became a parameter of the one engine rather
    // than a second engine with a name on it.

    NodeType zip = PureNode ("tree.zip", "Zip",
                             "Re-matches two collections against each other, item by item and branch by branch.");
    zip.inputs.push_back (WildcardTree ("a"));
    zip.inputs.push_back (WildcardTree ("b"));
    zip.outputs.push_back (WildcardTree ("a"));
    zip.outputs.push_back (WildcardTree ("b"));
    ParameterSchema match { "match", "Match", ValueType::String, false, Value (std::string ("longest")) };
    match.ui = ChoiceUi ("Zip", 0, "How to resolve inputs of different length.",
                         { { "Longest (repeat the last item)", Value (std::string ("longest")) },
                           { "Shortest (stop at the shorter)", Value (std::string ("shortest")) } });
    zip.parameters.push_back (std::move (match));
    zip.treeBody = [] (const Node& node, const data::TreeMap& inputs, const NodeExecutionContext&,
                       data::TreeMap& outputs, std::string& error) {
        data::IterationPolicy policy;
        const auto chosen = node.parameters.find ("match");
        const std::string* name =
            chosen == node.parameters.end () ? nullptr : std::get_if<std::string> (&chosen->second.DataValue ());
        // An unrecognised name falls back to the default rather than failing.
        // A policy name is a file-format contract, so a graph saved by a later
        // build that knows a third policy should still open here and do
        // something defensible rather than refuse to run.
        if (name != nullptr && *name == data::ItemMatchName (data::ItemMatch::Shortest))
            policy.itemMatch = data::ItemMatch::Shortest;

        data::IterationPlan plan;
        if (!data::BuildIterationPlan (MatchingInputs (inputs), policy, plan, error))
            return false;

        data::AnyTreeBuilder outA (inputs.at ("a").itemType);
        data::AnyTreeBuilder outB (inputs.at ("b").itemType);
        for (const data::DataPath& path : plan.emptyPaths) {
            outA.EnsureList (path);
            outB.EnsureList (path);
        }
        for (const data::Iteration& iteration : plan.iterations) {
            if (iteration.cursors.size () < 2)
                continue;
            if (!EmitCursorItem (inputs.at ("a"), iteration.cursors[0], iteration.outputPath, outA, error) ||
                !EmitCursorItem (inputs.at ("b"), iteration.cursors[1], iteration.outputPath, outB, error))
                return false;
        }
        outputs.emplace ("a", std::move (outA).Finish ());
        outputs.emplace ("b", std::move (outB).Finish ());
        return true;
    };
    if (!registry.Register (std::move (zip), error))
        throw std::logic_error (error);

    // ---- tree.filter -------------------------------------------------------
    //
    // ⚠️ IT HAS TO BE TREE-NATIVE, AND THAT IS WHY A SCRIPT NODE CANNOT DO IT.
    // A lifted body runs once per item and returns a value; it can TRANSFORM an
    // item (that is map, and a script node does it today) but it has no way to
    // say "there should be no item here at all" - returning nothing writes a
    // NULL, which keeps the site. Filtering changes how many items a branch
    // holds, and only a body that sees the branch can do that.

    NodeType filter = PureNode ("tree.filter", "Filter", "Keeps the items a pattern of true/false marks as true.");
    filter.inputs.push_back (WildcardTree ("tree"));
    filter.inputs.push_back (Port ("mask", ValueType::Bool, false));
    filter.outputs.push_back (WildcardTree ("tree"));
    filter.treeBody = [] (const Node&, const data::TreeMap& inputs, const NodeExecutionContext&, data::TreeMap& outputs,
                          std::string& error) {
        const data::TreeValue& tree = inputs.at ("tree");
        const data::TreeValue& mask = inputs.at ("mask");
        data::AnyTreeBuilder kept (tree.itemType);

        for (size_t branch = 0; branch < tree.tree->ListCount (); ++branch) {
            const data::DataPath& path = tree.tree->Paths ()[branch];
            const data::IDataList& list = tree.tree->ListAt (branch);
            // A branch that survives filtering to nothing is still a branch: an
            // empty list and no list are different states (§7.5), and collapsing
            // them would silently renumber everything downstream of it.
            kept.EnsureList (path);

            const data::IDataList* pattern = nullptr;
            if (mask.IsPresent () && mask.tree->ListCount () > 0) {
                // The mask's own branches pair with the tree's, clamping when it
                // has fewer - the same guide rule everything else follows.
                const size_t maskBranch = std::min (branch, mask.tree->ListCount () - 1);
                pattern = &mask.tree->ListAt (maskBranch);
            }

            for (size_t index = 0; index < list.Size (); ++index) {
                if (pattern != nullptr && pattern->Size () > 0) {
                    // ⚠️ THE PATTERN REPEATS, it does not clamp to its last
                    // value. This is the one place in the runtime that cycles,
                    // and it is deliberate: a mask is a PATTERN, not a parallel
                    // list. "true, false" clamped would keep item 0 and then
                    // drop everything after it, which is not what anybody types
                    // "every other one" to mean.
                    const std::optional<Value> flag = pattern->ValueAt (index % pattern->Size ());
                    const bool keep = flag.has_value () && std::get<bool> (flag->DataValue ());
                    if (!keep)
                        continue;
                }
                // No mask at all is the identity, not "keep nothing": an
                // unwired optional input means the node was not told to do
                // anything, and dropping every item would look like data loss.
                const std::optional<Value> value = list.ValueAt (index);
                if (!value.has_value ())
                    kept.AddNull (path, list.MetadataAt (index));
                else if (!kept.Add (path, *value, list.MetadataAt (index), error))
                    return false;
            }
        }
        outputs.emplace ("tree", std::move (kept).Finish ());
        return true;
    };
    if (!registry.Register (std::move (filter), error))
        throw std::logic_error (error);

    // ---- tree.crossProduct -------------------------------------------------

    NodeType cross = PureNode ("tree.crossProduct", "Cross Product",
                               "Re-matches two collections so every item of one meets every item of the other.");
    cross.inputs.push_back (WildcardTree ("a"));
    cross.inputs.push_back (WildcardTree ("b"));
    cross.outputs.push_back (WildcardTree ("a"));
    cross.outputs.push_back (WildcardTree ("b"));
    cross.treeBody = [] (const Node&, const data::TreeMap& inputs, const NodeExecutionContext&, data::TreeMap& outputs,
                         std::string& error) {
        const data::TreeValue& a = inputs.at ("a");
        const data::TreeValue& b = inputs.at ("b");
        data::AnyTreeBuilder outA (a.itemType);
        data::AnyTreeBuilder outB (b.itemType);

        // THE COMBINATIONS STAY IN THE BRANCH THEY CAME FROM. Grafting them
        // onto a deeper level would be a second decision - which of the two
        // inputs owns the new level - and neither answer is derivable from what
        // the user asked for. Somebody who wants that wires tree.graft, which
        // says so on the canvas.
        const size_t branches = std::max (a.tree->ListCount (), b.tree->ListCount ());
        // The wider input names the branches, which is the guide rule the
        // iteration engine already follows for every lifted node.
        const data::IDataTree& guide = a.tree->ListCount () >= b.tree->ListCount () ? *a.tree : *b.tree;

        for (size_t branch = 0; branch < branches; ++branch) {
            const data::IDataList* listA = branch < a.tree->ListCount () ? &a.tree->ListAt (branch) : nullptr;
            const data::IDataList* listB = branch < b.tree->ListCount () ? &b.tree->ListAt (branch) : nullptr;
            const data::DataPath path = branch < guide.ListCount () ? guide.Paths ()[branch] : data::DataPath::Zero ();
            outA.EnsureList (path);
            outB.EnsureList (path);
            if (listA == nullptr || listB == nullptr)
                continue; // Nothing meets nothing; the branch survives as empty.

            for (size_t indexA = 0; indexA < listA->Size (); ++indexA) {
                for (size_t indexB = 0; indexB < listB->Size (); ++indexB) {
                    const std::optional<Value> itemA = listA->ValueAt (indexA);
                    const std::optional<Value> itemB = listB->ValueAt (indexB);
                    if (!itemA.has_value ())
                        outA.AddNull (path);
                    else if (!outA.Add (path, *itemA, error))
                        return false;
                    if (!itemB.has_value ())
                        outB.AddNull (path);
                    else if (!outB.Add (path, *itemB, error))
                        return false;
                }
            }
        }
        outputs.emplace ("a", std::move (outA).Finish ());
        outputs.emplace ("b", std::move (outB).Finish ());
        return true;
    };
    if (!registry.Register (std::move (cross), error))
        throw std::logic_error (error);
}

bool IsTreeNodeType (const std::string& nodeTypeId)
{
    // Prefix-matched like every other family's ids (see GeometryNodes.cpp) -
    // adding a member needs no change here.
    return nodeTypeId.rfind ("tree.", 0) == 0;
}

} // namespace evp::nodegraph
