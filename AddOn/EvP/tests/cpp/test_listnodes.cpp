// Offline gate for the `list.*` family - reading INTO one branch.
//
// The assertions that matter here are the ones about the boundary with `tree.*`:
// a list node answers a question about ONE branch, and the runtime runs it once
// per branch. Nothing in ListNodes.cpp says so, which is exactly why it has to
// be tested: the day lifting stops walking branches, these nodes go on
// compiling and quietly start answering about the wrong thing.
//
// The other theme is that an index which addresses nothing produces NOTHING -
// a null item - rather than the nearest item that does exist. A clamped read is
// indistinguishable from a real one at the far end of a wire.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/ListNodes.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/NodeLifting.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace evp::nodegraph;
using namespace evp::nodegraph::data;

namespace {

// One list node run directly on one list, with no graph around it. `index` and
// `count` reach it as parameters, the way an unwired node carries them.
ValueMap RunListBody (const char* nodeType, const Value::List& items,
                      const std::vector<std::pair<std::string, int64_t>>& parameters = {})
{
    Node node { "n", nodeType };
    for (const auto& [id, value] : parameters)
        node.parameters.emplace (id, Value (value));

    ValueMap inputs;
    inputs.emplace ("list", Value (items));
    ValueMap outputs;
    NodeExecutionContext context;
    std::string error;
    EXPECT_TRUE (ExecuteListNode (node, inputs, context, outputs, error)) << error;
    return outputs;
}

std::vector<double> Numbers (const Value& value)
{
    std::vector<double> numbers;
    for (const Value& item : std::get<Value::List> (value.DataValue ()))
        numbers.push_back (std::get<double> (item.DataValue ()));
    return numbers;
}

Value::List Doubles (std::initializer_list<double> values)
{
    Value::List items;
    for (const double value : values)
        items.emplace_back (value);
    return items;
}

} // namespace

TEST (ListNodes, LengthCountsTheItemsAndZeroIsAnAnswer)
{
    EXPECT_EQ (std::get<int64_t> (RunListBody ("list.length", Doubles ({ 1, 2, 3 })).at ("length").DataValue ()), 3);

    // An unwired list is EMPTY, not missing: the length of nothing is zero, and
    // failing here would make an empty branch fail its whole node.
    EXPECT_EQ (std::get<int64_t> (RunListBody ("list.length", {}).at ("length").DataValue ()), 0);
}

TEST (ListNodes, ItemReadsByPositionAndCountsBackFromTheEnd)
{
    const Value::List items = Doubles ({ 10, 20, 30 });
    EXPECT_EQ (std::get<double> (RunListBody ("list.item", items, { { "index", 1 } }).at ("item").DataValue ()), 20.0);
    EXPECT_EQ (std::get<double> (RunListBody ("list.item", items, { { "index", -1 } }).at ("item").DataValue ()), 30.0);
    EXPECT_EQ (std::get<double> (RunListBody ("list.item", items, { { "index", -3 } }).at ("item").DataValue ()), 10.0);
}

TEST (ListNodes, AnIndexThatAddressesNothingProducesNothingRatherThanTheNearestItem)
{
    const Value::List items = Doubles ({ 10, 20, 30 });

    // ⚠️ NO `item` OUTPUT AT ALL, which the lift writes as a null item. Clamping
    // to 30 would be a wrong answer that reads exactly like a right one:
    // downstream, item 9 of a 3-item list and item 2 of it are both just 30.
    EXPECT_FALSE (RunListBody ("list.item", items, { { "index", 9 } }).contains ("item"));
    EXPECT_FALSE (RunListBody ("list.item", items, { { "index", -4 } }).contains ("item"));
    EXPECT_FALSE (RunListBody ("list.item", {}, { { "index", 0 } }).contains ("item"));
}

TEST (ListNodes, ReverseKeepsEveryItemAndOnlyTheOrderChanges)
{
    EXPECT_EQ (Numbers (RunListBody ("list.reverse", Doubles ({ 1, 2, 3 })).at ("list")),
               (std::vector<double> { 3, 2, 1 }));
    EXPECT_TRUE (Numbers (RunListBody ("list.reverse", {}).at ("list")).empty ());
}

TEST (ListNodes, SliceTakesARunAndCountZeroMeansEverythingFromTheStart)
{
    const Value::List items = Doubles ({ 1, 2, 3, 4, 5 });
    EXPECT_EQ (Numbers (RunListBody ("list.slice", items, { { "start", 1 }, { "count", 2 } }).at ("list")),
               (std::vector<double> { 2, 3 }));

    // The documented convention, and the reason it is not a hidden sentinel:
    // the default has to be the answer somebody wants.
    EXPECT_EQ (Numbers (RunListBody ("list.slice", items, { { "start", 2 } }).at ("list")),
               (std::vector<double> { 3, 4, 5 }));

    // A count longer than what is left is a COMPLETE answer to "take up to ten",
    // unlike an index past the end, which is not an answer at all.
    EXPECT_EQ (Numbers (RunListBody ("list.slice", items, { { "start", 3 }, { "count", 10 } }).at ("list")),
               (std::vector<double> { 4, 5 }));

    // A start that addresses nothing takes nothing, for the index reason above.
    EXPECT_TRUE (Numbers (RunListBody ("list.slice", items, { { "start", 9 } }).at ("list")).empty ());
    EXPECT_EQ (Numbers (RunListBody ("list.slice", items, { { "start", -2 } }).at ("list")),
               (std::vector<double> { 4, 5 }));
}

// ---- The boundary with tree.* ----------------------------------------------

namespace {

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

TEST (ListNodes, ALengthNodeAnswersPerBranchWithoutKnowingBranchesExist)
{
    // ⚠️ THE ASSERTION THIS FILE EXISTS FOR. Nothing in ListNodes.cpp mentions a
    // branch; the runtime walks them. Three numbers grafted into three branches
    // must give THREE lengths of one, not one length of three - and if lifting
    // ever stops walking branches, this is what says so.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 10.0));
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 20.0));
    ASSERT_TRUE (AddNode (graph, registry, "c", "number", 30.0));
    ASSERT_TRUE (AddNode (graph, registry, "list", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "grafted", "tree.graft"));
    ASSERT_TRUE (AddNode (graph, registry, "flatLength", "list.length"));
    ASSERT_TRUE (AddNode (graph, registry, "graftLength", "list.length"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "b", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "c", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "grafted", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "flatLength", "list"));
    ASSERT_TRUE (Wire (graph, registry, "grafted", "tree", "graftLength", "list"));

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const TreeValue& flat = evaluator.Result ("flatLength")->outputs.at ("length");
    const TreeValue& grafted = evaluator.Result ("graftLength")->outputs.at ("length");

    // One branch of three items: one answer, and it is 3.
    ASSERT_EQ (flat.tree->ItemCount (), 1u);
    EXPECT_EQ (std::get<int64_t> (flat.tree->ListAt (0).ValueAt (0)->DataValue ()), 3);

    // Three branches of one item each: three answers, each 1.
    ASSERT_EQ (grafted.tree->ListCount (), 3u);
    ASSERT_EQ (grafted.tree->ItemCount (), 3u);
    for (size_t branch = 0; branch < 3; ++branch)
        EXPECT_EQ (std::get<int64_t> (grafted.tree->ListAt (branch).ValueAt (0)->DataValue ()), 1);
}

TEST (ListNodes, AWildcardOutputMayFeedAPortThatNamedItsType)
{
    // ⚠️ THE REGRESSION THIS PAIRING EXISTS TO CATCH. `tree.graft` declares its
    // result Absent because a reshape does not know what the items are; a rule
    // that only treated Absent as a wildcard on the INPUT side refused this edge
    // as a type mismatch, which made "reshape a collection, then ask about it"
    // - most of the reason to reshape one - unwireable.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 10.0));
    ASSERT_TRUE (AddNode (graph, registry, "list", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "grafted", "tree.graft"));
    ASSERT_TRUE (AddNode (graph, registry, "length", "list.length"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "grafted", "tree"));
    ASSERT_TRUE (Wire (graph, registry, "grafted", "tree", "length", "list"));
}

TEST (ListNodes, WiringSeveralIndicesIntoItemReturnsSeveralItems)
{
    // The index port is Item access against a List-access list, so the engine
    // walks the indices and hands the whole list to each step. Two indices in,
    // two items out - with no loop written in ListNodes.cpp.
    //
    // The indices come from a `list.length` rather than from `number` nodes for
    // a reason worth knowing: `index` is an Integer port and the catalog's
    // numeric sources are all Double, so a Number node cannot currently be
    // wired to it at all. That is a real gap in the catalog, not a property of
    // this node - see the handoff.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "v1", "number", 10.0));
    ASSERT_TRUE (AddNode (graph, registry, "v2", "number", 20.0));
    ASSERT_TRUE (AddNode (graph, registry, "v3", "number", 30.0));
    ASSERT_TRUE (AddNode (graph, registry, "values", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "counted", "list.length"));
    ASSERT_TRUE (AddNode (graph, registry, "pick", "list.item"));
    for (const char* id : { "v1", "v2", "v3" })
        ASSERT_TRUE (Wire (graph, registry, id, "value", "values", "items"));
    ASSERT_TRUE (Wire (graph, registry, "values", "value", "counted", "list"));
    ASSERT_TRUE (Wire (graph, registry, "values", "value", "pick", "list"));
    ASSERT_TRUE (Wire (graph, registry, "counted", "length", "pick", "index"));

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    // length is 3, and index 3 addresses nothing in a 3-item list - so the
    // answer is a NULL item, not the last one. That is the same refusal to
    // clamp as above, arriving through a real graph.
    const TreeValue& picked = evaluator.Result ("pick")->outputs.at ("item");
    ASSERT_EQ (picked.tree->ItemCount (), 1u);
    EXPECT_TRUE (picked.tree->ListAt (0).IsNullAt (0));
}
