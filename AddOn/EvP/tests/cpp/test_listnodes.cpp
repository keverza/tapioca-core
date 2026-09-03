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
#include "NodeGraph/Data/AnyTree.hpp"
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
ValueMap RunListBody (const char* nodeType, const std::vector<Value>& items,
                      const std::vector<std::pair<std::string, int64_t>>& parameters = {})
{
    Node node { "n", nodeType };
    for (const auto& [id, value] : parameters)
        node.parameters.emplace (id, Value (value));

    ValueMap inputs;
    inputs.emplace ("list", Argument::FromItems (items));
    ValueMap outputs;
    NodeExecutionContext context;
    std::string error;
    EXPECT_TRUE (ExecuteListNode (node, inputs, context, outputs, error)) << error;
    return outputs;
}

std::vector<double> Numbers (const Argument& value)
{
    std::vector<double> numbers;
    for (const Value& item : value.Items ())
        numbers.push_back (std::get<double> (item.DataValue ()));
    return numbers;
}

DataPath P (std::initializer_list<DataPath::Segment> segments)
{
    return DataPath (segments);
}

std::vector<Value> Doubles (std::initializer_list<double> values)
{
    std::vector<Value> items;
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
    const std::vector<Value> items = Doubles ({ 10, 20, 30 });
    EXPECT_EQ (std::get<double> (RunListBody ("list.item", items, { { "index", 1 } }).at ("item").DataValue ()), 20.0);
    EXPECT_EQ (std::get<double> (RunListBody ("list.item", items, { { "index", -1 } }).at ("item").DataValue ()), 30.0);
    EXPECT_EQ (std::get<double> (RunListBody ("list.item", items, { { "index", -3 } }).at ("item").DataValue ()), 10.0);
}

TEST (ListNodes, AnIndexThatAddressesNothingProducesNothingRatherThanTheNearestItem)
{
    const std::vector<Value> items = Doubles ({ 10, 20, 30 });

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
    const std::vector<Value> items = Doubles ({ 1, 2, 3, 4, 5 });
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

// ---- Sorting ----------------------------------------------------------------

namespace {

std::vector<Value> Strings (std::initializer_list<const char*> values)
{
    std::vector<Value> items;
    for (const char* value : values)
        items.emplace_back (std::string (value));
    return items;
}

// list.sort takes two lists, so it needs its own runner.
ValueMap RunSort (const std::vector<Value>& keys, const std::vector<Value>& values, bool descending = false)
{
    Node node { "n", "list.sort" };
    node.parameters.emplace ("descending", Value (descending));

    ValueMap inputs;
    inputs.emplace ("keys", Argument::FromItems (keys));
    inputs.emplace ("values", Argument::FromItems (values));
    ValueMap outputs;
    NodeExecutionContext context;
    std::string error;
    EXPECT_TRUE (ExecuteListNode (node, inputs, context, outputs, error)) << error;
    return outputs;
}

std::vector<std::string> Texts (const Argument& value)
{
    std::vector<std::string> texts;
    for (const Value& item : value.Items ())
        texts.push_back (std::get<std::string> (item.DataValue ()));
    return texts;
}

} // namespace

TEST (ListNodes, SortOrdersByKeysAndCarriesTheParallelListWithIt)
{
    // ⚠️ THE SHAPE THIS NODE HAS, AND WHY. The useful question is almost never
    // "put these numbers in order" - it is "put these WALLS in order of their
    // area", with the areas computed upstream. So the keys order the sort and a
    // second list moves the same way; sorting the two lists independently would
    // pair them correctly only by luck.
    const ValueMap sorted = RunSort (Doubles ({ 30, 10, 20 }), Strings ({ "c", "a", "b" }));
    EXPECT_EQ (Numbers (sorted.at ("keys")), (std::vector<double> { 10, 20, 30 }));
    EXPECT_EQ (Texts (sorted.at ("values")), (std::vector<std::string> { "a", "b", "c" }));
}

TEST (ListNodes, SortIsStableSoEqualKeysKeepTheOrderTheyArrivedIn)
{
    // The solution re-runs on every keystroke, so an unstable sort would let two
    // runs over identical input produce two different graphs.
    const ValueMap sorted = RunSort (Doubles ({ 1, 1, 1 }), Strings ({ "first", "second", "third" }));
    EXPECT_EQ (Texts (sorted.at ("values")), (std::vector<std::string> { "first", "second", "third" }));

    // Descending is the ascending order READ BACKWARDS, ties included, rather
    // than a second sort with a flipped comparator.
    const ValueMap down = RunSort (Doubles ({ 1, 1, 1 }), Strings ({ "first", "second", "third" }), true);
    EXPECT_EQ (Texts (down.at ("values")), (std::vector<std::string> { "third", "second", "first" }));
}

TEST (ListNodes, SortRefusesKeysThatHaveNoOrderRatherThanInventingOne)
{
    Node node { "n", "list.sort" };
    ValueMap inputs;
    // A mesh is not greater or less than anything. A sort that fell back to
    // pointer order would be stable, repeatable and meaningless - and would look
    // exactly like a correct one.
    inputs.emplace ("keys", Argument::FromItems ({ Value (1.0), Value (Value::ImmutableMesh {}) }));
    ValueMap outputs;
    NodeExecutionContext context;
    std::string error;
    EXPECT_FALSE (ExecuteListNode (node, inputs, context, outputs, error));
    EXPECT_NE (error.find ("must be numbers"), std::string::npos);

    // Text is refused too: a key list orders the sort and ordering is a numeric
    // question here by decision, so there is no "is 10 before or after apple".
    ValueMap text;
    text.emplace ("keys", Argument::FromItems ({ Value (1.0), Value (std::string ("a")) }));
    EXPECT_FALSE (ExecuteListNode (node, text, context, outputs, error));
    EXPECT_NE (error.find ("must be numbers"), std::string::npos);
}

TEST (ListNodes, SortWithNoValuesIsAnOrdinarySort)
{
    const ValueMap sorted = RunSort (Doubles ({ 3, 1, 2 }), {});
    EXPECT_EQ (Numbers (sorted.at ("keys")), (std::vector<double> { 1, 2, 3 }));
    // The values list was empty, so every site is absent rather than shifted:
    // position n means the same thing in both lists or the pairing is a lie.
    EXPECT_EQ (sorted.at ("values").Items ().size (), 3u);
}

// ---- The numeric type lattice -----------------------------------------------

TEST (PortTypes, AnIntegerReachesADoublePortAndIsConvertedOnTheWay)
{
    // ⚠️ WIDENING IS SILENT BECAUSE NOTHING IS LOST. Every numeric source in the
    // catalog is Double and several ports are Integer, so without this rule a
    // whole corner of the catalog is wired only to itself.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 10.0));
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 20.0));
    ASSERT_TRUE (AddNode (graph, registry, "list", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "count", "list.length")); // Integer out.
    ASSERT_TRUE (AddNode (graph, registry, "scaled", "scaleList"));  // Double `factor` in.
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "b", "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "count", "list"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "scaled", "list"));
    ASSERT_TRUE (Wire (graph, registry, "count", "length", "scaled", "factor"));

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context);
    // The body reads `factor` with std::get<double>. Had the Integer tree
    // arrived unconverted, this would be a bad_variant_access caught by the
    // fault barrier - a failed node rather than a compile error, which is why
    // the conversion happens once in the evaluator and not in each body.
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const TreeValue& scaled = evaluator.Result ("scaled")->outputs.at ("value");
    ASSERT_EQ (scaled.tree->ItemCount (), 2u);
    EXPECT_EQ (std::get<double> (scaled.tree->ListAt (0).ValueAt (0)->DataValue ()), 20.0); // 10 x 2
    EXPECT_EQ (std::get<double> (scaled.tree->ListAt (0).ValueAt (1)->DataValue ()), 40.0); // 20 x 2
}

TEST (PortTypes, ADoubleIsRefusedByAnIntegerPortAndNeedsTheConversionNode)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 2.5));
    ASSERT_TRUE (AddNode (graph, registry, "values", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "pick", "list.item"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "values", "items"));
    ASSERT_TRUE (Wire (graph, registry, "values", "value", "pick", "list"));

    // ⚠️ REFUSED, AND THAT IS THE FEATURE. 2.5 is item 2 or item 3 depending on
    // an answer only the author has; a silent cast would put that answer in the
    // runtime where nobody can see it.
    EXPECT_FALSE (Wire (graph, registry, "a", "value", "pick", "index"));

    // The conversion node is how the graph says which answer it wants, and the
    // edge it enables is legal.
    ASSERT_TRUE (AddNode (graph, registry, "whole", "math.toInteger"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "whole", "value"));
    EXPECT_TRUE (Wire (graph, registry, "whole", "value", "pick", "index"));
}

TEST (PortTypes, EveryRoundingModeIsADifferentAnswerAndTheNodeSaysWhich)
{
    // The four modes are the whole reason narrowing is explicit. If two of them
    // agreed on a value this test would be describing a choice that is not one.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const auto whole = [&registry] (double value, const char* mode) {
        Node node { "n", "math.toInteger" };
        node.parameters.emplace ("value", Value (value));
        node.parameters.emplace ("mode", Value (std::string (mode)));
        ValueMap inputs;
        inputs.emplace ("value", Value (value));
        ValueMap outputs;
        NodeExecutionContext context;
        std::string error;
        EXPECT_TRUE (ExecuteRuntimeNode (node, inputs, context, outputs, error)) << error;
        return std::get<int64_t> (outputs.at ("value").DataValue ());
    };

    EXPECT_EQ (whole (2.5, "nearest"), 3);
    EXPECT_EQ (whole (2.5, "floor"), 2);
    EXPECT_EQ (whole (2.5, "ceiling"), 3);
    EXPECT_EQ (whole (2.5, "truncate"), 2);

    // Negative numbers are where floor and truncate part company, which is the
    // pair most easily assumed to be the same thing.
    EXPECT_EQ (whole (-2.5, "floor"), -3);
    EXPECT_EQ (whole (-2.5, "truncate"), -2);
    EXPECT_EQ (whole (-2.5, "ceiling"), -2);
}

TEST (PortTypes, WideningATreeKeepsItsShapeItsNullsAndItsIdentityWhenItDoesNothing)
{
    DataTreeBuilder<int64_t> builder;
    builder.Add (P ({ 0 }), 1);
    builder.AddNull (P ({ 0 }));
    builder.EnsureList (P ({ 1 }));
    const TreeValue integers = MakeTreeValue<int64_t> (std::move (builder).Finish ());

    const TreeValue widened = WidenTreeValue (integers, ItemType::Double);
    EXPECT_EQ (widened.itemType, ItemType::Double);
    EXPECT_EQ (widened.tree->ListCount (), 2u); // The empty branch survived.
    ASSERT_EQ (widened.tree->ListAt (0).Size (), 2u);
    EXPECT_EQ (std::get<double> (widened.tree->ListAt (0).ValueAt (0)->DataValue ()), 1.0);
    // A widening changes what an item IS, never whether there is one.
    EXPECT_TRUE (widened.tree->ListAt (0).IsNullAt (1));

    // Nothing to do means the SAME tree, not an equal copy of it - this runs on
    // every wire of every evaluation.
    EXPECT_EQ (WidenTreeValue (integers, ItemType::Integer).tree, integers.tree);
    EXPECT_EQ (WidenTreeValue (integers, ItemType::Any).tree, integers.tree);
    EXPECT_EQ (WidenTreeValue (integers, ItemType::String).tree, integers.tree);
}

// ---- Port modifiers, through the document and the evaluator -----------------

namespace {

EditResult SetModifier (GraphDocument& graph, const NodeRegistry& registry, const char* nodeId, const char* portId,
                        PortModifier modifier)
{
    return ApplyEdit (graph, registry, GraphEdit { SetPortModifierEdit { nodeId, portId, modifier } });
}

} // namespace

TEST (PortModifiers, AGraftModifierChangesWhatTheNodeComputesFrom)
{
    // ⚠️ THE POINT OF THE FEATURE: the same answer as wiring tree.graft into the
    // port, without a node on the canvas for it. Three numbers in one branch,
    // grafted at the port, must give the Length node THREE answers of one.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 10.0));
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 20.0));
    ASSERT_TRUE (AddNode (graph, registry, "c", "number", 30.0));
    ASSERT_TRUE (AddNode (graph, registry, "list", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "length", "list.length"));
    for (const char* id : { "a", "b", "c" })
        ASSERT_TRUE (Wire (graph, registry, id, "value", "list", "items"));
    ASSERT_TRUE (Wire (graph, registry, "list", "value", "length", "list"));

    ASSERT_TRUE (SetModifier (graph, registry, "length", "list", PortModifier::Graft).accepted);

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    ASSERT_TRUE (evaluator.Evaluate (graph, registry, ExecuteRuntimeNode, EvaluationRequest {}, context).succeeded);

    const TreeValue& lengths = evaluator.Result ("length")->outputs.at ("length");
    ASSERT_EQ (lengths.tree->ListCount (), 3u);
    for (size_t branch = 0; branch < 3u; ++branch)
        EXPECT_EQ (std::get<int64_t> (lengths.tree->ListAt (branch).ValueAt (0)->DataValue ()), 1);
}

TEST (PortModifiers, AModifierIsRefusedWhenItIsAlreadySetAndOnAnUnknownPort)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "length", "list.length"));

    EXPECT_EQ (SetModifier (graph, registry, "length", "nope", PortModifier::Graft).code, "modifier.unknownPort");
    // An accepted no-op would dirty the whole downstream closure for nothing.
    EXPECT_EQ (SetModifier (graph, registry, "length", "list", PortModifier::None).code, "modifier.unchanged");
    ASSERT_TRUE (SetModifier (graph, registry, "length", "list", PortModifier::Flatten).accepted);
    EXPECT_EQ (SetModifier (graph, registry, "length", "list", PortModifier::Flatten).code, "modifier.unchanged");
}

TEST (PortModifiers, RoundOnAPortIsWhatLetsADoubleReachAnIntegerOne)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 2.5));
    ASSERT_TRUE (AddNode (graph, registry, "values", "makeList"));
    ASSERT_TRUE (AddNode (graph, registry, "pick", "list.item"));
    ASSERT_TRUE (Wire (graph, registry, "a", "value", "values", "items"));
    ASSERT_TRUE (Wire (graph, registry, "values", "value", "pick", "list"));

    // Refused while the port says Integer and nothing has said which rounding.
    EXPECT_FALSE (Wire (graph, registry, "a", "value", "pick", "index"));

    // The modifier IS that answer, given on the port.
    ASSERT_TRUE (SetModifier (graph, registry, "pick", "index", PortModifier::Round).accepted);
    EXPECT_TRUE (Wire (graph, registry, "a", "value", "pick", "index"));

    // ⚠️ AND CLEARING IT AGAIN IS REFUSED RATHER THAN SILENTLY DROPPING THE WIRE.
    // The user asked to change a port, not to delete a connection they drew.
    const EditResult cleared = SetModifier (graph, registry, "pick", "index", PortModifier::None);
    EXPECT_FALSE (cleared.accepted);
    EXPECT_EQ (cleared.code, "modifier.breaksEdge");
}
