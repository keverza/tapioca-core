// Offline gate for the `tree.*` node family (HANDOFF §9.1) - the nodes whose
// whole job IS the shape of the data, which is exactly the case NodeLifting
// cannot serve: a Flatten or a Graft run per item would be the identity
// function, because the walk has already reduced the node to one item before
// the body ever runs.
//
// Two layers, matching how the family is built:
//
//  - the erased AnyTree.hpp wrappers (FlattenTreeValue, GraftTreeValue,
//    SimplifyTreeValue, ShiftTreeValuePaths), exercised directly the way
//    test_anytree.cpp exercises the rest of that file;
//
//  - the registered nodes, run through a REAL evaluator, because the point of
//    this family is only proven by ListCount()/Paths() actually differing
//    between a grafted and a flattened result - a value-level assertion
//    (equal items, different order) would pass for a bug that silently
//    dropped the tree layer and lifted these nodes after all.

#include "NodeGraph/TreeNodes.hpp"

#include "NodeGraph/Data/AnyTree.hpp"
#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/NodeLifting.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp::nodegraph;
using namespace evp::nodegraph::data;

namespace {

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

// A single-branch tree of three integers at {0}, the shape a fan-in like
// `makeList` produces and the one every reshaping node in this family is
// meant to act on.
TreeValue ThreeIntegers ()
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 10);
    builder.Add (P ({ 0 }), 20);
    builder.Add (P ({ 0 }), 30);
    return MakeTreeValue<int64_t> (std::move (builder).Finish ());
}

} // namespace

// ---- AnyTree.hpp wrappers ---------------------------------------------------

TEST (TreeValueOps, FlattenCollapsesEveryBranchIntoOneListAtTheRoot)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 1);
    builder.Add (P ({ 1 }), 2);
    builder.Add (P ({ 1 }), 3);
    const TreeValue input = MakeTreeValue<int64_t> (std::move (builder).Finish ());

    TreeValue result;
    std::string error;
    ASSERT_TRUE (FlattenTreeValue (input, result, error)) << error;
    EXPECT_EQ (result.itemType, ItemType::Integer);
    ASSERT_EQ (result.tree->ListCount (), 1u);
    EXPECT_EQ (result.tree->ItemCount (), 3u);
    EXPECT_EQ (result.tree->Paths ()[0], P ({ 0 }));
}

TEST (TreeValueOps, GraftGivesEveryItemItsOwnBranch)
{
    const TreeValue input = ThreeIntegers ();
    TreeValue result;
    std::string error;
    ASSERT_TRUE (GraftTreeValue (input, result, error)) << error;
    EXPECT_EQ (result.itemType, ItemType::Integer);
    // Same items, but the SHAPE changed: one branch of three became three
    // branches of one. That is the fact a flat item-order comparison cannot
    // see, and it is the entire reason this family exists.
    EXPECT_EQ (result.tree->ListCount (), 3u);
    EXPECT_EQ (result.tree->ItemCount (), 3u);
    EXPECT_EQ (result.tree->Paths ()[0], P ({ 0, 0 }));
    EXPECT_EQ (result.tree->Paths ()[1], P ({ 0, 1 }));
    EXPECT_EQ (result.tree->Paths ()[2], P ({ 0, 2 }));
}

TEST (TreeValueOps, SimplifyReturnsTheSamePointerWhenThereIsNothingToSimplify)
{
    const TreeValue input = ThreeIntegers (); // Already one branch at {0}.
    TreeValue result;
    std::string error;
    ASSERT_TRUE (SimplifyTreeValue (input, result, error)) << error;
    // Structural sharing is the point of an immutable tree (DataTreeOps.hpp):
    // an operation that is a no-op must hand back the SAME tree, not an equal
    // copy of it, or every already-simple tree in a large graph gets silently
    // rebuilt on every simplify node.
    EXPECT_EQ (result.tree, input.tree);
}

TEST (TreeValueOps, SimplifyDropsTheSharedPrefixWhenThereIsOne)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 5, 0 }), 1);
    builder.Add (P ({ 5, 1 }), 2);
    const TreeValue input = MakeTreeValue<int64_t> (std::move (builder).Finish ());

    TreeValue result;
    std::string error;
    ASSERT_TRUE (SimplifyTreeValue (input, result, error)) << error;
    EXPECT_NE (result.tree, input.tree);
    EXPECT_EQ (result.tree->Paths ()[0], P ({ 0 }));
    EXPECT_EQ (result.tree->Paths ()[1], P ({ 1 }));
}

TEST (TreeValueOps, ShiftMovesEveryPathAndFailsRatherThanEmptyOne)
{
    const TreeValue input = ThreeIntegers ();
    TreeValue result;
    std::string error;
    ASSERT_TRUE (ShiftTreeValuePaths (input, -2, PathCollision::Error, result, error)) << error;
    EXPECT_EQ (result.tree->Paths ()[0], P ({ 0, 0, 0 }));

    EXPECT_FALSE (ShiftTreeValuePaths (input, 1, PathCollision::Error, result, error));
    EXPECT_NE (error.find ("empty"), std::string::npos);
}

TEST (TreeValueOps, EveryWrapperRefusesAnAbsentTree)
{
    const TreeValue absent;
    TreeValue result;
    std::string error;
    EXPECT_FALSE (FlattenTreeValue (absent, result, error));
    EXPECT_FALSE (GraftTreeValue (absent, result, error));
    EXPECT_FALSE (SimplifyTreeValue (absent, result, error));
    EXPECT_FALSE (ShiftTreeValuePaths (absent, 1, PathCollision::Error, result, error));
}

// ---- The registered node family, through a real evaluator ------------------

namespace {

NodeRegistry MakeTreeTestRegistry ()
{
    return MakeRuntimeNodeRegistry ();
}

bool AddNode (GraphDocument& graph, const NodeRegistry& registry, const std::string& id, const std::string& nodeType,
              double numberValue = 0.0)
{
    Node node { id, nodeType };
    if (nodeType == "number")
        node.parameters.emplace ("value", Value (numberValue));
    return ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { std::move (node) } }).accepted;
}

bool Wire (GraphDocument& graph, const NodeRegistry& registry, const char* sourceNode, const char* sourcePort,
           const char* targetNode, const char* targetPort)
{
    return ApplyEdit (graph, registry,
                      GraphEdit { ConnectEdit { Edge { sourceNode, sourcePort, targetNode, targetPort } } })
        .accepted;
}

} // namespace

TEST (TreeNodes, GraftAndFlattenActuallyChangeBranchStructureNotJustItemOrder)
{
    const NodeRegistry registry = MakeTreeTestRegistry ();
    GraphDocument graph;

    // Three numbers fanned into one makeList - the same fixture
    // NodeGraphBuiltins.EvaluatesArithmeticListMapAndWatchWorkflow uses - gives
    // a tree of exactly ONE branch holding three items, which is the shape
    // every assertion below starts from.
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 10.0));
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 20.0));
    ASSERT_TRUE (AddNode (graph, registry, "c", "number", 30.0));
    ASSERT_TRUE (AddNode (graph, registry, "list", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "grafted", "tree.graft"));
    ASSERT_TRUE (AddNode (graph, registry, "flattened", "tree.flatten"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "b", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "c", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "grafted", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "grafted", "tree", "flattened", "tree"));

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> listResult = evaluator.Result ("list");
    const std::shared_ptr<const NodeResult> graftResult = evaluator.Result ("grafted");
    const std::shared_ptr<const NodeResult> flattenResult = evaluator.Result ("flattened");
    ASSERT_NE (listResult, nullptr);
    ASSERT_NE (graftResult, nullptr);
    ASSERT_NE (flattenResult, nullptr);

    const TreeValue& listTree = listResult->outputs.at ("value");
    const TreeValue& graftTree = graftResult->outputs.at ("tree");
    const TreeValue& flattenTree = flattenResult->outputs.at ("tree");

    // The starting shape: one branch, three items.
    ASSERT_EQ (listTree.tree->ListCount (), 1u);
    ASSERT_EQ (listTree.tree->ItemCount (), 3u);

    // Grafting: three branches now, one item each - a DIFFERENT ListCount, not
    // a reordered ItemCount-1 list. This is the assertion a per-item lift
    // could never produce, because a per-item Graft has nothing left to graft.
    EXPECT_EQ (graftTree.tree->ListCount (), 3u);
    EXPECT_EQ (graftTree.tree->ItemCount (), 3u);
    EXPECT_NE (graftTree.tree->Paths (), listTree.tree->Paths ());

    // Flattening the grafted tree returns to one branch - a different branch
    // count from the graft it consumed, and the round trip proves flatten
    // undid graft's shape rather than merely leaving the items alone.
    EXPECT_EQ (flattenTree.tree->ListCount (), 1u);
    EXPECT_EQ (flattenTree.tree->ItemCount (), 3u);
    EXPECT_EQ (flattenTree.tree->Paths ()[0], P ({ 0 }));

    // And item ORDER survived every reshape - checked by value, because the
    // count surviving is exactly what a reshape that shuffled its items would
    // also show. Canonical order is the guarantee the whole layer rests on: a
    // graft that emitted 30, 10, 20 would satisfy every assertion above.
    for (const TreeValue* tree : { &listTree, &flattenTree }) {
        const IDataList& list = tree->tree->ListAt (0);
        ASSERT_EQ (list.Size (), 3u);
        EXPECT_EQ (std::get<double> (list.ValueAt (0)->DataValue ()), 10.0);
        EXPECT_EQ (std::get<double> (list.ValueAt (1)->DataValue ()), 20.0);
        EXPECT_EQ (std::get<double> (list.ValueAt (2)->DataValue ()), 30.0);
    }

    // The grafted tree holds the same three, one per branch, in the same order.
    for (size_t branch = 0; branch < 3; ++branch) {
        const IDataList& list = graftTree.tree->ListAt (branch);
        ASSERT_EQ (list.Size (), 1u);
        EXPECT_EQ (std::get<double> (list.ValueAt (0)->DataValue ()), 10.0 * static_cast<double> (branch + 1));
    }
}

TEST (TreeNodes, ItemCountAndBranchCountReportTheShapeNotJustTheContent)
{
    const NodeRegistry registry = MakeTreeTestRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1.0));
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2.0));
    ASSERT_TRUE (AddNode (graph, registry, "list", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "grafted", "tree.graft"));
    ASSERT_TRUE (AddNode (graph, registry, "flatItems", "tree.itemCount"));
    ASSERT_TRUE (AddNode (graph, registry, "flatBranches", "tree.branchCount"));
    ASSERT_TRUE (AddNode (graph, registry, "graftItems", "tree.itemCount"));
    ASSERT_TRUE (AddNode (graph, registry, "graftBranches", "tree.branchCount"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "b", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "grafted", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "flatItems", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "flatBranches", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "grafted", "tree", "graftItems", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "grafted", "tree", "graftBranches", "tree"));

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const auto countOf = [&] (const char* nodeId) {
        const std::shared_ptr<const NodeResult> result = evaluator.Result (nodeId);
        const TreeValue& tree = result->outputs.at ("count");
        return std::get<int64_t> (tree.tree->ListAt (0).ValueAt (0)->DataValue ());
    };

    // Same two items, before and after grafting: the item count does not move.
    EXPECT_EQ (countOf ("flatItems"), 2);
    EXPECT_EQ (countOf ("graftItems"), 2);

    // The branch count DOES move - one before grafting, two after - which is
    // exactly the distinction ItemCount alone could never report.
    EXPECT_EQ (countOf ("flatBranches"), 1);
    EXPECT_EQ (countOf ("graftBranches"), 2);
}

TEST (TreeNodes, UnwiredWildcardInputIsTheEmptyTreeNotAFailure)
{
    const NodeRegistry registry = MakeTreeTestRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "grafted", "tree.graft"));

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("grafted");
    ASSERT_NE (result, nullptr);
    const TreeValue& tree = result->outputs.at ("tree");
    EXPECT_TRUE (tree.IsPresent ());
    EXPECT_TRUE (tree.tree->IsEmpty ());
}

// ---- §8.3's named matching policies -----------------------------------------
//
// §8.3 forbids a runtime-global default that guesses between shortest, longest
// and cross product, and requires a NAMED policy with fixtures before one is
// registered. These are those fixtures. They are written against the shapes the
// policies actually differ on - inputs of unequal length - because two policies
// agree on everything else, and a fixture built from equal-length inputs would
// pass whichever one the engine had been wired to.

namespace {

TreeValue Integers (std::initializer_list<DataPath::Segment> branch, std::initializer_list<int64_t> values)
{
    DataTreeBuilder<int64_t> builder;
    builder.EnsureList (DataPath (branch));
    for (const int64_t value : values)
        builder.Add (DataPath (branch), value);
    return MakeTreeValue<int64_t> (std::move (builder).Finish ());
}

// The items of one branch, as plain integers, for comparing a matching result
// against the sequence a person would write down.
std::vector<int64_t> BranchValues (const TreeValue& tree, size_t branch)
{
    std::vector<int64_t> values;
    const IDataList& list = tree.tree->ListAt (branch);
    for (size_t index = 0; index < list.Size (); ++index)
        values.push_back (std::get<int64_t> (list.ValueAt (index)->DataValue ()));
    return values;
}

// Runs one two-input tree node over `a` and `b` and returns its two outputs.
void RunMatchingNode (const char* nodeType, const TreeValue& a, const TreeValue& b, const char* matchPolicy,
                      TreeValue& outA, TreeValue& outB)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* type = registry.Find (nodeType);
    ASSERT_NE (type, nullptr) << nodeType;
    ASSERT_TRUE (static_cast<bool> (type->treeBody)) << nodeType << " is not tree-native";

    Node node { "n", nodeType };
    if (matchPolicy != nullptr)
        node.parameters.emplace ("match", Value (std::string (matchPolicy)));

    TreeMap inputs;
    inputs.emplace ("a", a);
    inputs.emplace ("b", b);
    TreeMap outputs;
    NodeExecutionContext context;
    std::string error;
    ASSERT_TRUE (type->treeBody (node, inputs, context, outputs, error)) << error;
    outA = outputs.at ("a");
    outB = outputs.at ("b");
}

} // namespace

TEST (TreeMatching, LongestRepeatsTheLastItemOfTheShorterInput)
{
    TreeValue outA;
    TreeValue outB;
    RunMatchingNode ("tree.zip", Integers ({ 0 }, { 1, 2, 3 }), Integers ({ 0 }, { 10 }), "longest", outA, outB);

    // Three results, and the single 10 answered all three of them. This is the
    // rule every lifted node already follows, which is why it is the default:
    // one number scaling three points must scale all three.
    EXPECT_EQ (BranchValues (outA, 0), (std::vector<int64_t> { 1, 2, 3 }));
    EXPECT_EQ (BranchValues (outB, 0), (std::vector<int64_t> { 10, 10, 10 }));
}

TEST (TreeMatching, ShortestStopsAtTheShorterInputAndRepeatsNothing)
{
    TreeValue outA;
    TreeValue outB;
    RunMatchingNode ("tree.zip", Integers ({ 0 }, { 1, 2, 3 }), Integers ({ 0 }, { 10 }), "shortest", outA, outB);

    // ONE result, not three. The two policies disagree here by design, and this
    // is the disagreement §8.3 refuses to resolve with a global default: pairing
    // is total under Shortest, so no item was ever used twice.
    EXPECT_EQ (BranchValues (outA, 0), (std::vector<int64_t> { 1 }));
    EXPECT_EQ (BranchValues (outB, 0), (std::vector<int64_t> { 10 }));
}

TEST (TreeMatching, TheDefaultPolicyIsTheOneEveryLiftedNodeAlreadyUses)
{
    // No `match` parameter at all - an older document, or a node nobody has
    // touched. It must behave as Longest, because that is what wiring the same
    // two collections into an ordinary two-input node does, and a Zip that
    // disagreed with the implicit matching would be a trap rather than a tool.
    TreeValue outA;
    TreeValue outB;
    RunMatchingNode ("tree.zip", Integers ({ 0 }, { 1, 2 }), Integers ({ 0 }, { 10 }), nullptr, outA, outB);
    EXPECT_EQ (BranchValues (outB, 0), (std::vector<int64_t> { 10, 10 }));

    // An unrecognised name is the same fallback rather than a failure: a policy
    // name is a file-format contract, so a graph saved by a build that knows a
    // third policy still opens here.
    RunMatchingNode ("tree.zip", Integers ({ 0 }, { 1, 2 }), Integers ({ 0 }, { 10 }), "someLaterPolicy", outA, outB);
    EXPECT_EQ (BranchValues (outB, 0), (std::vector<int64_t> { 10, 10 }));
}

TEST (TreeMatching, CrossProductPairsEveryItemWithEveryOtherAndStaysInItsBranch)
{
    TreeValue outA;
    TreeValue outB;
    RunMatchingNode ("tree.crossProduct", Integers ({ 0 }, { 1, 2 }), Integers ({ 0 }, { 10, 20, 30 }), nullptr, outA,
                     outB);

    // Six results from 2 x 3, read down the columns as pairs: (1,10) (1,20)
    // (1,30) (2,10) (2,20) (2,30).
    EXPECT_EQ (BranchValues (outA, 0), (std::vector<int64_t> { 1, 1, 1, 2, 2, 2 }));
    EXPECT_EQ (BranchValues (outB, 0), (std::vector<int64_t> { 10, 20, 30, 10, 20, 30 }));

    // ⚠️ AND THE COMBINATIONS DID NOT GRAFT THEMSELVES ONTO A NEW LEVEL. Which
    // input would own that level is not derivable from what was asked, so the
    // node does not decide it; tree.graft is how somebody says they want it.
    EXPECT_EQ (outA.tree->ListCount (), 1u);
    EXPECT_EQ (outA.tree->Paths ()[0], P ({ 0 }));
}

TEST (TreeMatching, BothOutputsAlwaysHaveTheSameLength)
{
    // The one invariant that makes a re-matching node usable at all: the two
    // outputs are read together, position by position, so a pairing that
    // produced 3 and 2 would be unreadable however sensible each half looked.
    const TreeValue three = Integers ({ 0 }, { 1, 2, 3 });
    const TreeValue one = Integers ({ 0 }, { 10 });
    const TreeValue none = EmptyTreeValue (ItemType::Integer);

    for (const char* policy : { "longest", "shortest" }) {
        for (const TreeValue* right : { &one, &none }) {
            TreeValue outA;
            TreeValue outB;
            RunMatchingNode ("tree.zip", three, *right, policy, outA, outB);
            EXPECT_EQ (outA.tree->ItemCount (), outB.tree->ItemCount ())
                << "policy " << policy << " left the two outputs different lengths";
        }
    }
}

TEST (TreeMatching, AnEmptyInputUnderShortestPairsNothingRatherThanEverything)
{
    TreeValue outA;
    TreeValue outB;
    RunMatchingNode ("tree.zip", Integers ({ 0 }, { 1, 2, 3 }), EmptyTreeValue (ItemType::Integer), "shortest", outA,
                     outB);
    // Nothing pairs with nothing. Not a failure - an empty answer to a question
    // that has one.
    EXPECT_EQ (outA.tree->ItemCount (), 0u);
    EXPECT_EQ (outB.tree->ItemCount (), 0u);
}

// ---- tree.shiftPath ---------------------------------------------------------

TEST (TreeNodes, ShiftPathMovesEveryBranchAndLeavesItemOrderAlone)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* type = registry.Find ("tree.shiftPath");
    ASSERT_NE (type, nullptr);

    Node node { "n", "tree.shiftPath" };
    node.parameters.emplace ("shift", Value (static_cast<int64_t> (-1)));

    TreeMap inputs;
    inputs.emplace ("tree", Integers ({ 4 }, { 7, 8 }));
    TreeMap outputs;
    NodeExecutionContext context;
    std::string error;
    ASSERT_TRUE (type->treeBody (node, inputs, context, outputs, error)) << error;

    const TreeValue& shifted = outputs.at ("tree");
    EXPECT_EQ (shifted.tree->Paths ()[0], P ({ 0, 4 }));
    EXPECT_EQ (BranchValues (shifted, 0), (std::vector<int64_t> { 7, 8 }));
}
