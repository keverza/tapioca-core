#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/BuiltinNodes.hpp"
#include "NodeGraph/EvaluationPlan.hpp"
#include "NodeGraph/FaultBarrier.hpp"
#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/GraphReports.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/ScriptNodes.hpp"
#include "Geometry/Transforms.hpp"
#include "NodeGraph/PreviewProjection.hpp"
#include "Preview/GraphPreviewStore.hpp"
#include "NodeGraph/ValueText.hpp"
#include "NodeGraph/RunEvents.hpp"
#include "NodeGraph/RunHistory.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/GraphSerializer.hpp"
#include "NodeGraph/GraphStore.hpp"
#include "NodeGraph/Json.hpp"
#include "NodeGraph/WorkerPool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <memory>
#include <thread>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace evp::nodegraph;

NodeRegistry MakeRegistry ()
{
    NodeRegistry registry;
    std::string error;

    NodeType number;
    number.id = "number";
    number.label = "Number";
    number.outputs.push_back ({ "value", "Value", ValueType::Integer });
    number.parameters.push_back ({ "value", "Value", ValueType::Integer, true });
    EXPECT_TRUE (registry.Register (std::move (number), error)) << error;

    NodeType add;
    add.id = "add";
    add.label = "Add";
    add.inputs.push_back ({ "left", "Left", ValueType::Integer });
    add.inputs.push_back ({ "right", "Right", ValueType::Integer });
    add.outputs.push_back ({ "sum", "Sum", ValueType::Integer });
    EXPECT_TRUE (registry.Register (std::move (add), error)) << error;
    return registry;
}

EditResult AddNode (GraphDocument& graph, const NodeRegistry& registry, const std::string& id,
                    const std::string& nodeType, int64_t value = 0)
{
    Node node { id, nodeType };
    if (nodeType == "number")
        node.parameters.emplace ("value", Value (value));
    return ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { std::move (node) } });
}

Edge Connect (const std::string& sourceNode, const std::string& sourcePort, const std::string& targetNode,
              const std::string& targetPort)
{
    return { sourceNode, sourcePort, targetNode, targetPort };
}

int64_t Integer (const Argument& value)
{
    return std::get<int64_t> (value.DataValue ());
}

// A published output, as one value.
//
// A node publishes a TREE per port now; these assertions are about what a node
// COMPUTED, not about how the result is shaped, so they read it back through
// the same projection the browser and the panel use. A one-item tree projects
// to that item, which is what every scalar assertion here expects.
Argument Out (const std::shared_ptr<const NodeResult>& result, const char* portId)
{
    const auto found = result->outputs.find (portId);
    return found == result->outputs.end () ? Argument {} : ProjectTreeToValue (found->second);
}

// The inverse, for a test that stages an upstream result by hand.
data::TreeValue AsTree (const Argument& value)
{
    data::TreeValue tree;
    std::string error;
    EXPECT_TRUE (TreeFromValue (value, data::ItemType::Any, tree, error)) << error;
    return tree;
}

// How many times each node's body ran, counted safely.
//
// ⚠️ A BARE std::map HERE IS A DATA RACE, NOT A SHORTCUT. Independent nodes of
// one level run CONCURRENTLY - one on a pool thread, one on the coordinator -
// and NodeExecutor's contract in Evaluator.hpp says in as many words that a body
// must be safe to call from several threads at once. Two unguarded
// `++counts[id]` calls on the same map lost an increment often enough to make
// two cache tests fail intermittently in the full suite while passing alone,
// which reads as a runtime bug and is not one.
class ExecutionCounter {
  public:
    void Record (const std::string& nodeId)
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        ++counts_[nodeId];
    }

    int operator[] (const std::string& nodeId) const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        const auto found = counts_.find (nodeId);
        return found == counts_.end () ? 0 : found->second;
    }

  private:
    mutable std::mutex mutex_;
    std::map<std::string, int> counts_;
};

// Every test drives the evaluator through one run context, so a test that cares
// about run identity or cancellation can supply its own.
EvaluationOutcome RunGraph (Evaluator& evaluator, const GraphDocument& graph, const NodeRegistry& registry,
                            const NodeExecutor& executor, std::vector<NodeId> targets = {}, RunId runId = 1)
{
    RunContext context;
    context.runId = runId;
    return evaluator.Evaluate (graph, registry, executor, EvaluationRequest { std::move (targets) }, context);
}

} // namespace

TEST (NodeGraphRegistry, RejectsInvalidDeclarativeSchemas)
{
    NodeRegistry registry;
    NodeType invalid;
    invalid.id = "bad";
    invalid.inputs.push_back ({ "same", "First", ValueType::Double });
    invalid.inputs.push_back ({ "same", "Second", ValueType::Double });
    std::string error;
    EXPECT_FALSE (registry.Register (std::move (invalid), error));
    EXPECT_NE (std::string::npos, error.find ("duplicate input"));
}

TEST (NodeGraphEdits, RejectsCycleAtomically)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "add").accepted);

    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "b", "left") } }).accepted);
    const uint64_t revision = graph.Revision ();
    const size_t edgeCount = graph.Edges ().size ();

    const EditResult result =
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "sum", "b", "right") } });
    EXPECT_FALSE (result.accepted);
    EXPECT_NE (std::string::npos, result.error.find ("cycle"));
    EXPECT_EQ (revision, graph.Revision ());
    EXPECT_EQ (edgeCount, graph.Edges ().size ());
}

TEST (NodeGraphEdits, RejectsTypeMismatchOccupiedInputAndUnknownIds)
{
    NodeRegistry registry = MakeRegistry ();
    NodeType text;
    text.id = "text";
    text.outputs.push_back ({ "value", "Value", ValueType::String });
    std::string error;
    ASSERT_TRUE (registry.Register (std::move (text), error));

    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "one", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "two", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "add", "add").accepted);
    ASSERT_TRUE (AddNode (graph, registry, "text", "text").accepted);

    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("one", "value", "add", "left") } }).accepted);
    EXPECT_FALSE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("two", "value", "add", "left") } }).accepted);
    EXPECT_FALSE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("text", "value", "add", "right") } }).accepted);
    EXPECT_FALSE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("missing", "value", "add", "right") } })
            .accepted);
    EXPECT_EQ (1U, graph.Edges ().size ());
}

TEST (NodeGraphEdits, RemovesNodesAndEdgesInOneAtomicRevision)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "one", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "two", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    const Edge left = Connect ("one", "value", "sum", "left");
    const Edge right = Connect ("two", "value", "sum", "right");
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { left } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { right } }).accepted);
    const uint64_t revision = graph.Revision ();

    const EditResult result = ApplyEdit (graph, registry, GraphEdit { RemoveElementsEdit { { "one" }, { right } } });

    ASSERT_TRUE (result.accepted) << result.error;
    EXPECT_EQ (revision + 1, graph.Revision ());
    EXPECT_EQ (nullptr, graph.FindNode ("one"));
    EXPECT_TRUE (graph.Edges ().empty ());
    EXPECT_NE (result.dirtyNodes.end (), std::find (result.dirtyNodes.begin (), result.dirtyNodes.end (), "sum"));
}

TEST (NodeGraphEdits, RejectsWholeElementRemovalWhenOneIdIsUnknown)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "one", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    const Edge edge = Connect ("one", "value", "sum", "left");
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { edge } }).accepted);
    const uint64_t revision = graph.Revision ();

    const EditResult result = ApplyEdit (graph, registry, GraphEdit { RemoveElementsEdit { { "missing" }, { edge } } });

    EXPECT_FALSE (result.accepted);
    EXPECT_EQ (revision, graph.Revision ());
    EXPECT_NE (nullptr, graph.FindNode ("one"));
    EXPECT_EQ (1U, graph.Edges ().size ());
}

TEST (NodeGraphTopoOrder, IsDeterministicAndIncludesDisconnectedNodes)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "z", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("z", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "right") } }).accepted);

    const TopoResult topo = BuildTopoOrder (graph);
    ASSERT_TRUE (topo.IsAcyclic ());
    EXPECT_EQ ((std::vector<NodeId> { "a", "z", "sum" }), topo.order);
}

TEST (NodeGraphEvaluator, CachesResultsAndPropagatesDirtDownstream)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "one", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "two", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("one", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("two", "value", "sum", "right") } }).accepted);

    ExecutionCounter executions;
    const NodeExecutor executor = [&executions] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                                 ValueMap& outputs, std::string&) {
        executions.Record (node.id);
        if (node.nodeType == "number")
            outputs.emplace ("value", node.parameters.at ("value"));
        else
            outputs.emplace ("sum", Value (Integer (inputs.at ("left")) + Integer (inputs.at ("right"))));
        return true;
    };

    Evaluator evaluator;
    ASSERT_TRUE (RunGraph (evaluator, graph, registry, executor).succeeded);
    ASSERT_TRUE (RunGraph (evaluator, graph, registry, executor).succeeded);
    EXPECT_EQ (1, executions["one"]);
    EXPECT_EQ (1, executions["two"]);
    EXPECT_EQ (1, executions["sum"]);
    EXPECT_EQ (3, Integer (Out (evaluator.Result ("sum"), "sum")));

    evaluator.Invalidate (graph, { "one" });
    ASSERT_TRUE (RunGraph (evaluator, graph, registry, executor).succeeded);
    EXPECT_EQ (2, executions["one"]);
    EXPECT_EQ (1, executions["two"]);
    EXPECT_EQ (2, executions["sum"]);

    const EditResult edit =
        ApplyEdit (graph, registry, GraphEdit { SetParameterEdit { "one", "value", Value (int64_t { 5 }) } });
    ASSERT_TRUE (edit.accepted) << edit.error;
    evaluator.Invalidate (graph, edit.dirtyNodes);
    EXPECT_TRUE (evaluator.IsDirty ("one"));
    EXPECT_TRUE (evaluator.IsDirty ("sum"));
    EXPECT_FALSE (evaluator.IsDirty ("two"));

    ASSERT_TRUE (RunGraph (evaluator, graph, registry, executor).succeeded);
    EXPECT_EQ (3, executions["one"]);
    EXPECT_EQ (1, executions["two"]);
    EXPECT_EQ (3, executions["sum"]);
    EXPECT_EQ (7, Integer (Out (evaluator.Result ("sum"), "sum")));
}

TEST (NodeGraphValue, HoldsListsAndImmutableMeshes)
{
    auto mesh = std::make_shared<geomsrv::Mesh> ();
    mesh->guid = "guid";
    const Argument value =
        Argument::FromItems ({ Value (true), Value (Point3 { 1.0, 2.0, 3.0 }), Value (Value::ImmutableMesh (mesh)),
                               Value (ArchicadElementRef { "element-guid" }) });
    EXPECT_EQ (ValueType::List, value.Type ());
    EXPECT_NE (0U, value.Hash ());
    EXPECT_EQ (4U, value.Items ().size ());
}

TEST (NodeGraphBuiltins, CatalogIsSchemaDrivenAndPure)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    // Counted by CATEGORY rather than as one number, because the total is now a
    // library that grows: a bare count told you a node had been added and
    // nothing about whether it landed where a user would look for it.
    std::map<std::string, size_t> byCategory;
    for (const auto& [id, nodeType] : registry.Types ())
        ++byCategory[nodeType.category];
    EXPECT_EQ (8U, byCategory["Core"]);    // number, the four arithmetic nodes, makeList, scaleList, watch
    EXPECT_EQ (2U, byCategory["Inspect"]); // panel, preview
    EXPECT_EQ (2U, byCategory["Flow"]);    // dataDam - Stage F, so Holding is reachable
    EXPECT_EQ (2U, byCategory["Input"]);   // numberSlider, booleanToggle
    EXPECT_EQ (11U,
               byCategory["Archicad"]); // an attribute picker per domain, the two selection nodes, the library part
    EXPECT_EQ (21U, byCategory["Geometry"]); // inputs, vectors, polygons, solids, curves and the surface makers
    EXPECT_EQ (6U, byCategory["Transform"]); // move, rotate, scale, mirror and the two arrays
    EXPECT_EQ (3U, byCategory["Math"]);      // remap, random, toInteger
    // One container per element type the classification table marks as one. The
    // number is the table's, not a taste: a type that quietly lost its container
    // moves it, and test_elementclassification.cpp says which.
    EXPECT_EQ (18U, byCategory["Archicad Elements"]);
    // The script node family: one type per language, so a user looking for
    // "Python" finds a node called Python rather than one they must place and
    // then reconfigure. Their ports are NOT here - they are declared in the file
    // each node runs, which is what NodeType::instancePorts marks.
    EXPECT_EQ (2U, byCategory["Script"]); // script.javascript, script.python
    // tree.* - flatten, graft, simplify, itemCount, branchCount, shiftPath,
    // zip, crossProduct, filter. See TreeNodes.hpp for why they carry no
    // Execute* function of their own.
    EXPECT_EQ (9U, byCategory["Tree"]);
    // list.* - length, item, reverse, sort, slice. Ordinary lifted bodies,
    // unlike tree.*: they answer about ONE branch and the runtime walks the rest.
    EXPECT_EQ (5U, byCategory["List"]);
    EXPECT_EQ (89U, registry.Types ().size ());
    EXPECT_EQ (ExecutionDomain::Worker, registry.Find ("scaleList")->executionDomain);
    EXPECT_EQ (ValueType::List, registry.Find ("watch")->outputs.front ().valueType);

    // Stage F3/F4 capability is declared, not inferred, so it is assertable.
    EXPECT_TRUE (registry.Find ("dataDam")->holdCapable);
    EXPECT_FALSE (registry.Find ("add")->holdCapable);
    EXPECT_TRUE (registry.Find ("panel")->bypassMappings.empty ());
    ASSERT_EQ (1U, registry.Find ("add")->bypassMappings.size ());
    EXPECT_EQ ("left", registry.Find ("add")->bypassMappings.front ().inputId);
}

// An input with nothing wired to it falls back to the value typed into the node
// - Grasshopper's "internalised" input. It is stored as a parameter under the
// INPUT'S OWN ID, which is what lets a node type that declares no parameters at
// all (Multiply declares none) still take a typed-in operand.
TEST (NodeGraphBuiltins, InternalisedInputValueSuppliesAnUnconnectedPort)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "product", "multiply" } } }).accepted);

    // Refused before it reaches the document: an internalised value is checked
    // against the PORT's declared type, exactly as an edge into it would be.
    const EditResult mistyped =
        ApplyEdit (graph, registry, GraphEdit { SetParameterEdit { "product", "left", Value (std::string ("six")) } });
    EXPECT_FALSE (mistyped.accepted);
    EXPECT_FALSE (
        ApplyEdit (graph, registry, GraphEdit { SetParameterEdit { "product", "nosuchport", Value (6.0) } }).accepted);

    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { SetParameterEdit { "product", "left", Value (6.0) } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { SetParameterEdit { "product", "right", Value (7.0) } }).accepted);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteBuiltinNode);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_DOUBLE_EQ (42.0, std::get<double> (Out (evaluator.Result ("product"), "value").DataValue ()));

    // A wire beats the typed-in value rather than merging with it.
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { [] {
                                Node node { "ten", "number" };
                                node.parameters.emplace ("value", Value (10.0));
                                return node;
                            }() } })
                     .accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("ten", "value", "product", "left") } })
                     .accepted);
    Evaluator wired;
    ASSERT_TRUE (RunGraph (wired, graph, registry, ExecuteBuiltinNode).succeeded);
    EXPECT_DOUBLE_EQ (70.0, std::get<double> (Out (wired.Result ("product"), "value").DataValue ()));
}

TEST (NodeGraphBuiltins, EvaluatesArithmeticListMapAndWatchWorkflow)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    const auto addNode = [&] (const char* id, const char* nodeType, double numberValue = 0.0) {
        Node node { id, nodeType };
        if (node.nodeType == "number")
            node.parameters.emplace ("value", Value (numberValue));
        return ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { std::move (node) } }).accepted;
    };
    ASSERT_TRUE (addNode ("two", "number", 2.0));
    ASSERT_TRUE (addNode ("three", "number", 3.0));
    ASSERT_TRUE (addNode ("sum", "add"));
    ASSERT_TRUE (addNode ("product", "multiply"));
    ASSERT_TRUE (addNode ("list", "makeList"));
    ASSERT_TRUE (addNode ("scaled", "scaleList"));
    ASSERT_TRUE (addNode ("watch", "watch"));
    const auto connect = [&] (const char* source, const char* sourcePort, const char* target, const char* targetPort) {
        return ApplyEdit (graph, registry,
                          GraphEdit { ConnectEdit { Connect (source, sourcePort, target, targetPort) } })
            .accepted;
    };
    ASSERT_TRUE (connect ("two", "value", "sum", "left"));
    ASSERT_TRUE (connect ("three", "value", "sum", "right"));
    ASSERT_TRUE (connect ("sum", "value", "product", "left"));
    ASSERT_TRUE (connect ("three", "value", "product", "right"));
    ASSERT_TRUE (connect ("two", "value", "list", "items"));
    ASSERT_TRUE (connect ("three", "value", "list", "items"));
    ASSERT_TRUE (connect ("product", "value", "list", "items"));
    ASSERT_TRUE (connect ("list", "value", "scaled", "list"));
    ASSERT_TRUE (connect ("two", "value", "scaled", "factor"));
    ASSERT_TRUE (connect ("scaled", "value", "watch", "value"));

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteBuiltinNode);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    const Argument valuesValue = Out (evaluator.Result ("watch"), "value");
    const std::vector<Value>& values = valuesValue.Items ();
    ASSERT_EQ (3U, values.size ());
    EXPECT_DOUBLE_EQ (4.0, std::get<double> (values[0].DataValue ()));
    EXPECT_DOUBLE_EQ (6.0, std::get<double> (values[1].DataValue ()));
    EXPECT_DOUBLE_EQ (30.0, std::get<double> (values[2].DataValue ()));
}

TEST (NodeGraphEvaluator, FailurePreservesLastGoodResult)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "one", "number", 1).accepted);
    Evaluator evaluator;
    const NodeExecutor succeeds = [] (const Node& node, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs,
                                      std::string&) {
        outputs.emplace ("value", node.parameters.at ("value"));
        return true;
    };
    ASSERT_TRUE (RunGraph (evaluator, graph, registry, succeeds).succeeded);
    const auto first = evaluator.Result ("one");
    evaluator.Invalidate (graph, { "one" });
    const NodeExecutor fails = [] (const Node&, const ValueMap&, const NodeExecutionContext&, ValueMap&,
                                   std::string& nodeError) {
        nodeError = "expected failure";
        return false;
    };
    EXPECT_FALSE (RunGraph (evaluator, graph, registry, fails).succeeded);
    EXPECT_EQ (first, evaluator.Result ("one"));
    EXPECT_EQ (NodeExecutionState::Error, evaluator.Status ("one").state);
}

// --- Stage A: demand-driven evaluation ------------------------------------

TEST (NodeGraphPlan, EvaluatesOnlyTheUpstreamClosureOfItsTargets)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    // wanted <- a, b     ignored <- c
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "c", "number", 3).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "wanted", "add").accepted);
    ASSERT_TRUE (AddNode (graph, registry, "ignored", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "wanted", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "wanted", "right") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("c", "value", "ignored", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("c", "value", "ignored", "right") } }).accepted);

    ExecutionCounter executions;
    const NodeExecutor executor = [&executions] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                                 ValueMap& outputs, std::string&) {
        executions.Record (node.id);
        if (node.nodeType == "number")
            outputs.emplace ("value", node.parameters.at ("value"));
        else
            outputs.emplace ("sum", Value (Integer (inputs.at ("left")) + Integer (inputs.at ("right"))));
        return true;
    };

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, executor, { "wanted" });
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (3U, outcome.plannedNodes.size ());
    EXPECT_EQ (1, executions["a"]);
    EXPECT_EQ (1, executions["b"]);
    EXPECT_EQ (1, executions["wanted"]);
    EXPECT_EQ (0, executions["c"]);
    EXPECT_EQ (0, executions["ignored"]);
    EXPECT_TRUE (evaluator.IsDirty ("ignored"));
}

TEST (NodeGraphPlan, GroupsIndependentNodesIntoOneLevel)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    RunContext context;
    context.runId = 1;
    const PlanOutcome planned = BuildEvaluationPlan (graph, registry, EvaluationRequest {}, context);
    ASSERT_TRUE (planned.accepted) << planned.error;
    ASSERT_EQ (2U, planned.plan.levels.size ());
    // a and b are independent, so the worker pool may run them together.
    EXPECT_EQ ((std::vector<NodeId> { "a", "b" }), planned.plan.levels[0]);
    EXPECT_EQ ((std::vector<NodeId> { "sum" }), planned.plan.levels[1]);
}

TEST (NodeGraphPlan, RejectsUnknownTargetsAndUnknownNodeTypesBeforeExecuting)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    RunContext context;
    context.runId = 1;
    const PlanOutcome missing = BuildEvaluationPlan (graph, registry, EvaluationRequest { { "nope" } }, context);
    EXPECT_FALSE (missing.accepted);
    EXPECT_EQ ((std::vector<NodeId> { "nope" }), missing.unknownTargets);

    // A node type that vanished from the registry - the after-upgrade case -
    // is a plan-time rejection, not a mid-run surprise.
    const NodeRegistry empty;
    const PlanOutcome unknownType = BuildEvaluationPlan (graph, empty, EvaluationRequest {}, context);
    EXPECT_FALSE (unknownType.accepted);
    EXPECT_NE (std::string::npos, unknownType.error.find ("unknown node type"));
}

TEST (NodeGraphRun, CancellationKeepsFinishedResultsAndMarksTheRestCancelled)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    RunContext context;
    context.runId = 7;
    // Cancel from inside the first node, the way a user pressing stop would.
    const NodeExecutor executor = [&context] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                              ValueMap& outputs, std::string&) {
        if (node.nodeType == "number")
            outputs.emplace ("value", node.parameters.at ("value"));
        else
            outputs.emplace ("sum", Value (Integer (inputs.at ("left")) + Integer (inputs.at ("right"))));
        if (node.id == "a")
            context.cancellation.Cancel ();
        return true;
    };

    Evaluator evaluator;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, executor, EvaluationRequest {}, context);
    EXPECT_TRUE (outcome.cancelled);
    EXPECT_FALSE (outcome.succeeded);
    // Rule 7: what finished is kept, exactly as it was.
    EXPECT_EQ (NodeExecutionState::Success, evaluator.Status ("a").state);
    ASSERT_NE (nullptr, evaluator.Result ("a"));
    EXPECT_EQ (1, Integer (Out (evaluator.Result ("a"), "value")));
    EXPECT_EQ (NodeExecutionState::Cancelled, evaluator.Status ("sum").state);
    EXPECT_EQ (nullptr, evaluator.Result ("sum"));
}

// --- Stage A: fault containment (PLAN-NodeGraphBackend.md section 5) --------

TEST (NodeGraphContainment, ThrowingNodeFailsThatNodeOnly)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);

    Evaluator evaluator;
    const NodeExecutor executor = [] (const Node& node, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs,
                                      std::string&) -> bool {
        if (node.id == "a")
            throw std::runtime_error ("node exploded");
        outputs.emplace ("value", node.parameters.at ("value"));
        return true;
    };
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, executor);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_EQ (NodeExecutionState::Error, evaluator.Status ("a").state);
    EXPECT_NE (std::string::npos, evaluator.Status ("a").message.find ("node exploded"));
    // The independent branch still finished: a failure is scoped, not global.
    EXPECT_EQ (NodeExecutionState::Success, evaluator.Status ("b").state);
}

#if defined(_MSC_VER)
TEST (NodeGraphContainment, StructuredExceptionInNodeCodeFailsTheNodeInsteadOfTheProcess)
{
    // The reason FaultBarrier exists: catch(...) does not reliably catch this,
    // and reaching the assertion below means the process would otherwise be gone.
    const GuardOutcome outcome = RunGuarded ([] () -> bool {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
        return true;
    });
    EXPECT_FALSE (outcome.completed);
    EXPECT_NE (std::string::npos, outcome.fault.find ("access violation"));
}
#endif

TEST (NodeGraphContainment, RejectsAnEvaluationStartedFromInsideAnEvaluation)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    Evaluator evaluator;
    bool reentrantRejected = false;
    const NodeExecutor executor = [&] (const Node& node, const ValueMap&, const NodeExecutionContext&,
                                       ValueMap& outputs, std::string&) {
        RunContext inner;
        inner.runId = 99;
        const EvaluationOutcome nested =
            evaluator.Evaluate (graph, registry, NodeExecutor {}, EvaluationRequest {}, inner);
        reentrantRejected = !nested.succeeded && nested.error == "an evaluation is already running";
        outputs.emplace ("value", node.parameters.at ("value"));
        return true;
    };
    ASSERT_TRUE (RunGraph (evaluator, graph, registry, executor).succeeded);
    EXPECT_TRUE (reentrantRejected);
    EXPECT_FALSE (evaluator.IsRunning ());
}

TEST (NodeGraphContainment, OversizedOutputsFailTheirNode)
{
    NodeRegistry registry;
    NodeType producer;
    producer.id = "producer";
    producer.outputs.push_back ({ "value", "Value", ValueType::List });
    std::string error;
    ASSERT_TRUE (registry.Register (std::move (producer), error)) << error;

    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "big", "producer" } } }).accepted);

    Evaluator evaluator;
    const NodeExecutor wide = [] (const Node&, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs,
                                  std::string&) {
        std::vector<Value> list;
        for (int i = 0; i < 64; ++i)
            list.emplace_back (static_cast<int64_t> (i));
        outputs.emplace ("value", Argument::FromItems (std::move (list)));
        return true;
    };

    RunContext context;
    context.runId = 1;
    context.limits.maxOutputItems = 8;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, wide, EvaluationRequest {}, context);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, evaluator.Status ("big").message.find ("output ceiling"));

    // A NESTED result - a list whose items are themselves lists - used to be
    // refused at this same site by AnyTreeBuilder::Add ("a list is not an
    // item"), reached by building 40 levels of Value::List by hand. It is no
    // longer expressible at all: Argument::Items() is a flat std::vector<Value>,
    // and Value itself has no List alternative any more, so the invariant that
    // a tree cannot contain another tree (HANDOFF §7.3) is now enforced by the
    // type system rather than by this test.
}

TEST (NodeGraphContainment, RejectsAPlanLargerThanItsCeiling)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    for (int i = 0; i < 5; ++i)
        ASSERT_TRUE (AddNode (graph, registry, "n" + std::to_string (i), "number", i).accepted);

    RunContext context;
    context.runId = 1;
    context.limits.maxPlanNodes = 3;
    const PlanOutcome planned = BuildEvaluationPlan (graph, registry, EvaluationRequest {}, context);
    EXPECT_FALSE (planned.accepted);
    EXPECT_NE (std::string::npos, planned.error.find ("node ceiling"));
}

TEST (NodeGraphContainment, FailedNodeBlocksItsDownstreamWithANamedReason)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    Evaluator evaluator;
    const NodeExecutor executor = [] (const Node& node, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs,
                                      std::string& nodeError) {
        if (node.id == "a") {
            nodeError = "deliberate";
            return false;
        }
        outputs.emplace ("value", node.parameters.at ("value"));
        return true;
    };
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, executor);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_EQ ("a", outcome.failedNode);
    EXPECT_EQ (NodeExecutionState::Error, evaluator.Status ("a").state);
    EXPECT_EQ (NodeExecutionState::Blocked, evaluator.Status ("sum").state);
    EXPECT_NE (std::string::npos, evaluator.Status ("sum").message.find ("upstream node failed: a"));
    // b is independent of the failure and must not be collateral damage.
    EXPECT_EQ (NodeExecutionState::Success, evaluator.Status ("b").state);
}

TEST (NodeGraphRun, CacheHitsAreReportedSeparatelyFromExecutions)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    Evaluator evaluator;
    const NodeExecutor executor = [] (const Node& node, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs,
                                      std::string&) {
        outputs.emplace ("value", node.parameters.at ("value"));
        return true;
    };
    EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, executor, {}, 1);
    EXPECT_EQ (1U, outcome.executedCount);
    EXPECT_EQ (0U, outcome.cacheHitCount);
    EXPECT_FALSE (evaluator.Status ("a").cacheHit);

    outcome = RunGraph (evaluator, graph, registry, executor, {}, 2);
    EXPECT_EQ (0U, outcome.executedCount);
    EXPECT_EQ (1U, outcome.cacheHitCount);
    EXPECT_TRUE (evaluator.Status ("a").cacheHit);
    EXPECT_EQ (2U, evaluator.Status ("a").runId);

    // Forced mode is the diagnostic escape hatch and must bypass the cache.
    RunContext context;
    context.runId = 3;
    outcome = evaluator.Evaluate (graph, registry, executor, EvaluationRequest { {}, EvaluationMode::Forced }, context);
    EXPECT_EQ (1U, outcome.executedCount);
    EXPECT_EQ (0U, outcome.cacheHitCount);
}

// --- Stage B: the observation stream ---------------------------------------

namespace {

// Collects a run's events the way a client would, through a recorder.
std::vector<RunEventKind> KindsOf (const std::vector<RunEvent>& events)
{
    std::vector<RunEventKind> kinds;
    for (const RunEvent& event : events)
        kinds.push_back (event.kind);
    return kinds;
}

} // namespace

TEST (NodeGraphEvents, ARunDescribesItselfFromStartToFinish)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    RunRecorder recorder;
    RunContext context;
    context.runId = 4;
    context.graphId = "g1";
    context.events = recorder.SinkFor ("g1");

    const NodeExecutor executor = [] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                      ValueMap& outputs, std::string&) {
        if (node.nodeType == "number")
            outputs.emplace ("value", node.parameters.at ("value"));
        else
            outputs.emplace ("sum", Value (Integer (inputs.at ("left")) + Integer (inputs.at ("right"))));
        return true;
    };

    Evaluator evaluator;
    ASSERT_TRUE (evaluator.Evaluate (graph, registry, executor, EvaluationRequest {}, context).succeeded);

    const RunEventLog::Tail tail = recorder.Events ().Since (kNoEvent, 0);
    EXPECT_FALSE (tail.gap);
    const std::vector<RunEventKind> kinds = KindsOf (tail.events);
    ASSERT_FALSE (kinds.empty ());
    EXPECT_EQ (RunEventKind::RunStarted, kinds.front ());
    EXPECT_EQ (RunEventKind::RunCompleted, kinds.back ());
    EXPECT_EQ (3, std::count (kinds.begin (), kinds.end (), RunEventKind::NodeQueued));
    EXPECT_EQ (3, std::count (kinds.begin (), kinds.end (), RunEventKind::NodeCompleted));

    // Sequences are monotonic, and every event carries the correlation identity.
    EventSeq previous = kNoEvent;
    for (const RunEvent& event : tail.events) {
        EXPECT_GT (event.seq, previous);
        previous = event.seq;
        EXPECT_EQ ("g1", event.graphId);
        EXPECT_EQ (4U, event.runId);
        EXPECT_NE (0, event.timestampMs);
    }
    EXPECT_EQ (previous, tail.lastSeq);
}

TEST (NodeGraphEvents, SnapshotPlusDeltaDeliversEachEventExactlyOnce)
{
    RunEventLog log (64);
    for (int i = 0; i < 10; ++i) {
        RunEvent event;
        event.kind = RunEventKind::NodeCompleted;
        event.nodeId = "n" + std::to_string (i);
        log.Append (std::move (event));
    }

    // The client's loop: take the tail, remember lastSeq, ask again.
    RunEventLog::Tail first = log.Since (kNoEvent, 4);
    EXPECT_FALSE (first.gap);
    ASSERT_EQ (4U, first.events.size ());
    EXPECT_EQ (first.events.back ().seq, first.lastSeq);

    const RunEventLog::Tail second = log.Since (first.lastSeq, 4);
    ASSERT_EQ (4U, second.events.size ());
    EXPECT_EQ ("n4", second.events.front ().nodeId);

    const RunEventLog::Tail third = log.Since (second.lastSeq, 0);
    ASSERT_EQ (2U, third.events.size ());
    EXPECT_EQ ("n9", third.events.back ().nodeId);

    // Caught up: asking again yields nothing and no gap.
    const RunEventLog::Tail idle = log.Since (third.lastSeq, 0);
    EXPECT_TRUE (idle.events.empty ());
    EXPECT_FALSE (idle.gap);
}

TEST (NodeGraphEvents, AClientThatFellOffTheRingIsToldRatherThanQuietlyShorted)
{
    RunEventLog log (4);
    for (int i = 0; i < 12; ++i) {
        RunEvent event;
        event.kind = RunEventKind::NodeCompleted;
        log.Append (std::move (event));
    }

    // Sequence 1 was dropped long ago. Silently returning the surviving tail
    // would let the client stitch it onto state that never saw the middle.
    const RunEventLog::Tail stale = log.Since (1, 0);
    EXPECT_TRUE (stale.gap);

    // A fresh client is not in a gap - its snapshot covers what was dropped.
    const RunEventLog::Tail fresh = log.Since (kNoEvent, 0);
    EXPECT_FALSE (fresh.gap);
    EXPECT_EQ (4U, fresh.events.size ());
    EXPECT_EQ (12U, fresh.lastSeq);
}

TEST (NodeGraphHistory, RunRecordIsAFoldOverTheStreamAndAgreesWithIt)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    RunRecorder recorder;
    Evaluator evaluator;
    const NodeExecutor executor = [] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                      ValueMap& outputs, std::string& nodeError) {
        if (node.id == "b") {
            nodeError = "deliberate";
            return false;
        }
        if (node.nodeType == "number")
            outputs.emplace ("value", node.parameters.at ("value"));
        else
            outputs.emplace ("sum", Value (Integer (inputs.at ("left")) + Integer (inputs.at ("right"))));
        return true;
    };

    RunContext context;
    context.runId = 11;
    context.graphId = "g";
    context.events = recorder.SinkFor ("g");
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, executor, EvaluationRequest {}, context);
    ASSERT_FALSE (outcome.succeeded);

    const std::vector<RunRecord> recent = recorder.History ().Recent (0);
    ASSERT_EQ (1U, recent.size ());
    const RunRecord& record = recent.front ();
    EXPECT_EQ (11U, record.runId);
    EXPECT_EQ ("g", record.graphId);
    EXPECT_TRUE (record.finished);
    EXPECT_FALSE (record.succeeded);
    EXPECT_FALSE (record.cancelled);
    EXPECT_EQ ("b", record.failedNode);

    // The fold must agree with the evaluator's own counters - that agreement is
    // the reason history is derived from the stream rather than written beside it.
    EXPECT_EQ (outcome.executedCount, record.executedCount);
    EXPECT_EQ (outcome.failedCount, record.failedCount);
    EXPECT_EQ (outcome.blockedCount, record.blockedCount);
    EXPECT_EQ (outcome.plannedNodes.size (), record.plannedCount);

    // And per node, with the evaluator's live status.
    ASSERT_EQ (3U, record.nodes.size ());
    for (const NodeRunRecord& node : record.nodes)
        EXPECT_EQ (evaluator.Status (node.nodeId).state, node.finalState) << node.nodeId;
}

TEST (NodeGraphHistory, ARejectedPlanStillProducesAFinishedRunRecord)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    RunRecorder recorder;
    Evaluator evaluator;
    RunContext context;
    context.runId = 3;
    context.events = recorder.SinkFor ("g");
    // A plan rejection is the case most likely to leave a run hanging with no
    // record at all, which would read to a client as "still running".
    const EvaluationOutcome outcome =
        evaluator.Evaluate (graph, registry, NodeExecutor {}, EvaluationRequest { { "missing" } }, context);
    EXPECT_FALSE (outcome.succeeded);

    const std::vector<RunRecord> recent = recorder.History ().Recent (0);
    ASSERT_EQ (1U, recent.size ());
    EXPECT_TRUE (recent.front ().finished);
    EXPECT_FALSE (recent.front ().succeeded);
    EXPECT_NE (std::string::npos, recent.front ().error.find ("unknown evaluation target"));
}

TEST (NodeGraphHistory, CancellationIsRecordedAsCancelledRatherThanFailed)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);

    RunRecorder recorder;
    Evaluator evaluator;
    RunContext context;
    context.runId = 5;
    context.events = recorder.SinkFor ("g");
    const NodeExecutor executor = [&context] (const Node& node, const ValueMap&, const NodeExecutionContext&,
                                              ValueMap& outputs, std::string&) {
        outputs.emplace ("value", node.parameters.at ("value"));
        context.cancellation.Cancel ();
        return true;
    };
    EXPECT_TRUE (evaluator.Evaluate (graph, registry, executor, EvaluationRequest {}, context).cancelled);

    const std::optional<RunRecord> record = recorder.History ().Find (5);
    ASSERT_TRUE (record.has_value ());
    EXPECT_TRUE (record->cancelled);
    EXPECT_FALSE (record->succeeded);
    EXPECT_TRUE (record->finished);

    const RunEventLog::Tail tail = recorder.Events ().Since (kNoEvent, 0);
    EXPECT_EQ (RunEventKind::RunCancelled, tail.events.back ().kind);
}

TEST (NodeGraphRuntimeState, HoldsGraphsByIdWithIndependentDocumentsAndStreams)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();

    const GraphId first = "runtime-test-first";
    const GraphId second = "runtime-test-second";

    Node node { "n", "number" };
    node.parameters.emplace ("value", Value (2.0));
    ASSERT_TRUE (runtime.Apply (first, GraphEdit { AddNodeEdit { node } }).accepted);

    // The second graph is created on first reference and shares nothing.
    EXPECT_EQ (1U, runtime.Document (first).Nodes ().size ());
    EXPECT_EQ (0U, runtime.Document (second).Nodes ().size ());

    const EvaluationSummary summary = runtime.Evaluate (first, EvaluationRequest {});
    ASSERT_TRUE (summary.succeeded) << summary.error;
    EXPECT_EQ (first, summary.graphId);
    EXPECT_GT (summary.lastEventSeq, kNoEvent);

    // The snapshot's sequence and the delta feed line up: asking from the
    // snapshot's position yields nothing new.
    const ResultsSnapshot snapshot = runtime.Results (first);
    EXPECT_EQ (first, snapshot.graphId);
    EXPECT_TRUE (runtime.Events (first, snapshot.lastEventSeq, 0).events.empty ());
    EXPECT_FALSE (runtime.Events (first, kNoEvent, 0).events.empty ());

    // The other graph never ran, so its stream is empty.
    EXPECT_TRUE (runtime.Events (second, kNoEvent, 0).events.empty ());
    EXPECT_TRUE (runtime.RecentRuns (second, 0).empty ());

    ASSERT_EQ (1U, runtime.RecentRuns (first, 0).size ());
    EXPECT_EQ (summary.runId, runtime.RecentRuns (first, 0).front ().runId);
}

// --- Phase 0: batched transactions, expected revisions, and history ---------
//
// These go through GraphRuntimeState rather than ApplyEdit directly, because the
// whole point is the behaviour AROUND the edits: the rollback, the revision
// check and the undo stack all live in the runtime, not in the edit.

namespace {

/**
 * Read one numeric parameter out of a document.
 *
 * ⚠️ TAKES THE DOCUMENT BY VALUE AND KEEPS IT ALIVE FOR THE READ. GraphRuntimeState
 * hands out documents by value, so FindNode on a chained temporary returns a
 * pointer that is dangling by the time the caller reads through it.
 */
double ParameterOf (const GraphDocument& document, const std::string& nodeId, const std::string& parameterId)
{
    const Node* node = document.FindNode (nodeId);
    return node == nullptr ? std::numeric_limits<double>::quiet_NaN ()
                           : std::get<double> (node->parameters.at (parameterId).DataValue ());
}

GraphEdit AddNumberNode (const std::string& nodeId, double value)
{
    Node node { nodeId, "number" };
    node.parameters.emplace ("value", Value (value));
    return GraphEdit { AddNodeEdit { node } };
}

} // namespace

TEST (NodeGraphTransaction, AppliesEveryEditOrNoneOfThem)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "txn-all-or-nothing";

    // The third edit names a node type that does not exist, so the whole batch
    // must leave the document exactly as it found it - not two nodes in.
    const std::vector<GraphEdit> edits {
        AddNumberNode ("a", 1.0),
        AddNumberNode ("b", 2.0),
        GraphEdit { AddNodeEdit { Node { "c", "no.such.node.type" } } },
    };

    const uint64_t before = runtime.Document (graphId).Revision ();
    const BatchEditResult result = runtime.ApplyBatch (graphId, std::nullopt, edits);

    EXPECT_FALSE (result.accepted);
    EXPECT_EQ (2U, result.failedIndex) << "a client cannot report WHICH edit refused without this";
    EXPECT_TRUE (runtime.Document (graphId).Nodes ().empty ())
        << "a partly applied batch is the state this exists to prevent";

    // The revision still moved, and that is correct: the rollback is a new state
    // of the document, so a client watching the revision re-reads and sees the
    // graph it actually has rather than trusting a stale copy.
    EXPECT_GT (runtime.Document (graphId).Revision (), before);
}

TEST (NodeGraphTransaction, AcceptedBatchAppliesEveryEditAndReportsEveryDirtyNode)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "txn-accepted";

    const std::vector<GraphEdit> edits { AddNumberNode ("a", 1.0), AddNumberNode ("b", 2.0) };
    const BatchEditResult result = runtime.ApplyBatch (graphId, std::nullopt, edits, "Paste 2 nodes");

    ASSERT_TRUE (result.accepted) << result.error;
    EXPECT_EQ (2U, runtime.Document (graphId).Nodes ().size ());
    EXPECT_EQ (2U, result.dirtyNodes.size ());
}

TEST (NodeGraphTransaction, RefusesABatchBuiltAgainstARevisionThatHasMovedOn)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "txn-revision";

    const uint64_t stale = runtime.Document (graphId).Revision ();
    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("someone-else", 9.0)).accepted);

    const BatchEditResult result = runtime.ApplyBatch (graphId, stale, { AddNumberNode ("mine", 1.0) });
    EXPECT_FALSE (result.accepted);
    EXPECT_EQ (batchcode::kRevisionConflict, result.code);
    EXPECT_EQ (nullptr, runtime.Document (graphId).FindNode ("mine"));

    // The same batch against the revision the document really has is accepted,
    // which is what makes a conflict a retry rather than a dead end.
    const BatchEditResult retry =
        runtime.ApplyBatch (graphId, runtime.Document (graphId).Revision (), { AddNumberNode ("mine", 1.0) });
    EXPECT_TRUE (retry.accepted) << retry.error;
}

TEST (NodeGraphTransaction, RefusesAnEmptyBatchAndOneThatIsAbsurdlyLarge)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "txn-bounds";

    EXPECT_EQ (batchcode::kEmptyBatch, runtime.ApplyBatch (graphId, std::nullopt, {}).code);

    std::vector<GraphEdit> tooMany;
    for (size_t index = 0; index <= kMaxBatchEdits; ++index)
        tooMany.push_back (AddNumberNode ("n" + std::to_string (index), 0.0));
    EXPECT_EQ (batchcode::kBatchTooLarge, runtime.ApplyBatch (graphId, std::nullopt, tooMany).code);
}

TEST (NodeGraphTransaction, RefusesAReleaseInsideABatchBecauseItCannotBeRolledBack)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "txn-release";

    const std::vector<GraphEdit> edits { AddNumberNode ("a", 1.0), GraphEdit { ReleaseHoldingEdit { "a" } } };
    const BatchEditResult result = runtime.ApplyBatch (graphId, std::nullopt, edits);

    EXPECT_FALSE (result.accepted);
    EXPECT_EQ (batchcode::kUnbatchableEdit, result.code);
    // Refused BEFORE anything ran, so the legal first edit did not land either.
    EXPECT_TRUE (runtime.Document (graphId).Nodes ().empty ());
}

TEST (NodeGraphUndo, UndoRestoresADeletedNodeAndRedoRemovesItAgain)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-delete";

    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("doomed", 4.0), "Add node").accepted);
    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { RemoveNodeEdit { "doomed" } }, "Delete node").accepted);
    ASSERT_EQ (nullptr, runtime.Document (graphId).FindNode ("doomed"));

    EXPECT_TRUE (runtime.History (graphId).canUndo);
    EXPECT_EQ ("Delete node", runtime.History (graphId).undoLabel);

    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    // ⚠️ HELD IN A NAMED LOCAL, NOT CHAINED. Document() returns BY VALUE, so a
    // pointer taken straight out of runtime.Document (id).FindNode (...) points
    // into a temporary that dies at the end of that statement.
    const GraphDocument afterUndo = runtime.Document (graphId);
    const Node* restored = afterUndo.FindNode ("doomed");
    ASSERT_NE (nullptr, restored) << "the node came back";
    EXPECT_EQ (4.0, std::get<double> (restored->parameters.at ("value").DataValue ())) << "and so did what was in it";

    ASSERT_TRUE (runtime.Redo (graphId).accepted);
    EXPECT_EQ (nullptr, runtime.Document (graphId).FindNode ("doomed"));

    // Two consecutive undos walk back two steps rather than sticking on one.
    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    EXPECT_TRUE (runtime.Document (graphId).Nodes ().empty ());
    EXPECT_FALSE (runtime.History (graphId).canUndo);
    EXPECT_EQ (batchcode::kNothingToUndo, runtime.Undo (graphId).code);
}

TEST (NodeGraphUndo, UndoRestoresTheWiresAndNotJustTheNodes)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-wires";

    ASSERT_TRUE (runtime
                     .ApplyBatch (graphId, std::nullopt,
                                  { AddNumberNode ("a", 1.0), AddNumberNode ("b", 2.0),
                                    GraphEdit { AddNodeEdit { Node { "sum", "add" } } } },
                                  "Build")
                     .accepted);
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { ConnectEdit { Edge { "a", "value", "sum", "left" } } }, "Connect").accepted);
    ASSERT_EQ (1U, runtime.Document (graphId).Edges ().size ());

    // Deleting the node takes its wire with it. This is exactly the case a
    // hand-written inverse action gets wrong: it puts the node back and forgets
    // the edge, because removing the edge was a CONSEQUENCE rather than the edit.
    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { RemoveNodeEdit { "a" } }, "Delete node").accepted);
    ASSERT_TRUE (runtime.Document (graphId).Edges ().empty ());

    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    EXPECT_NE (nullptr, runtime.Document (graphId).FindNode ("a"));
    EXPECT_EQ (1U, runtime.Document (graphId).Edges ().size ()) << "the wire came back with the node";
}

TEST (NodeGraphUndo, OneSliderDragIsOneUndoStep)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-slider";

    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("n", 0.0), "Add node").accepted);

    // What a drag actually sends: many setParam edits under one gesture key.
    for (int step = 1; step <= 40; ++step)
        ASSERT_TRUE (runtime
                         .Apply (graphId, GraphEdit { SetParameterEdit { "n", "value", Value (double (step)) } },
                                 "Set value", "setParam:n:value:gesture-1")
                         .accepted);
    EXPECT_EQ (40.0, ParameterOf (runtime.Document (graphId), "n", "value"));

    // One Ctrl+Z returns to before the drag, not to 39.
    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    EXPECT_EQ (0.0, ParameterOf (runtime.Document (graphId), "n", "value"));

    // A SECOND DRAG IS A SECOND STEP. The key carries a gesture id precisely so
    // that two drags of the same slider do not collapse into one undo entry.
    ASSERT_TRUE (runtime.Redo (graphId).accepted);
    ASSERT_TRUE (runtime
                     .Apply (graphId, GraphEdit { SetParameterEdit { "n", "value", Value (99.0) } }, "Set value",
                             "setParam:n:value:gesture-2")
                     .accepted);
    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    EXPECT_EQ (40.0, ParameterOf (runtime.Document (graphId), "n", "value"))
        << "the second drag undid to the end of the first, not past it";
}

TEST (NodeGraphUndo, AnEditAfterAnUndoDiscardsTheRedoBranch)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-branch";

    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("a", 1.0), "Add a").accepted);
    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    ASSERT_TRUE (runtime.History (graphId).canRedo);

    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("b", 2.0), "Add b").accepted);
    EXPECT_FALSE (runtime.History (graphId).canRedo) << "a future that no longer follows is not offered";
    EXPECT_EQ (batchcode::kNothingToRedo, runtime.Redo (graphId).code);
}

TEST (NodeGraphUndo, ARefusedEditLeavesNoUndoStep)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-refused";

    ASSERT_FALSE (runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "x", "no.such.node.type" } } }).accepted);
    EXPECT_FALSE (runtime.History (graphId).canUndo)
        << "a Ctrl+Z that appears to do nothing is worse than a disabled one";
}

TEST (NodeGraphUndo, TheStackIsBoundedAndDropsTheOldestStep)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-bounded";
    EXPECT_EQ (kDefaultUndoDepth, runtime.History (graphId).depth) << "twenty steps unless asked otherwise";

    for (size_t index = 0; index < kDefaultUndoDepth + 10; ++index)
        ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("n" + std::to_string (index), 0.0)).accepted);

    size_t undone = 0;
    while (runtime.Undo (graphId).accepted)
        ++undone;
    EXPECT_EQ (kDefaultUndoDepth, undone);

    // The steps that fell off the old end are gone, so the graph does NOT come
    // back empty. Undo is bounded, and that is a promise rather than a defect.
    EXPECT_EQ (10U, runtime.Document (graphId).Nodes ().size ());
}

TEST (NodeGraphUndo, TheDepthIsASettingAndLoweringItTakesEffectAtOnce)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-depth";

    runtime.SetHistoryDepth (graphId, 3);
    EXPECT_EQ (3U, runtime.History (graphId).depth);

    for (int index = 0; index < 10; ++index)
        ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("n" + std::to_string (index), 0.0)).accepted);
    size_t undone = 0;
    while (runtime.Undo (graphId).accepted)
        ++undone;
    EXPECT_EQ (3U, undone);

    // Raising it does not invent steps that were never kept.
    runtime.SetHistoryDepth (graphId, 50);
    EXPECT_EQ (50U, runtime.History (graphId).depth);
    EXPECT_FALSE (runtime.History (graphId).canUndo);

    // Lowering discards immediately rather than waiting for the next edit, so
    // the setting means what it says as soon as it is applied.
    for (int index = 0; index < 8; ++index)
        ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("m" + std::to_string (index), 0.0)).accepted);
    runtime.SetHistoryDepth (graphId, 2);
    undone = 0;
    while (runtime.Undo (graphId).accepted)
        ++undone;
    EXPECT_EQ (2U, undone);
}

// A client keeps undo steps of its own for the things this document does not
// hold - node positions above all - and has to interleave them with these in
// the order the user performed them. `stepsRecorded` is what makes that
// possible, and these are the three cases where the stack SIZE cannot.
TEST (NodeGraphUndo, StepsRecordedCountsPushesRatherThanStackSize)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-steps-recorded";

    const uint64_t start = runtime.History (graphId).stepsRecorded;
    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("a", 1.0), "Add a").accepted);
    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("b", 2.0), "Add b").accepted);
    EXPECT_EQ (start + 2, runtime.History (graphId).stepsRecorded);
    EXPECT_EQ (2U, runtime.History (graphId).undoCount);

    // 1. A COALESCED EDIT records nothing, and must be reported as nothing: a
    //    client that added a timeline entry here would offer one Ctrl+Z too many.
    ASSERT_TRUE (runtime
                     .Apply (graphId, GraphEdit { SetParameterEdit { "a", "value", Value (5.0) } }, "Set value",
                             "gesture-1")
                     .accepted);
    const uint64_t afterFirstOfGesture = runtime.History (graphId).stepsRecorded;
    ASSERT_TRUE (runtime
                     .Apply (graphId, GraphEdit { SetParameterEdit { "a", "value", Value (6.0) } }, "Set value",
                             "gesture-1")
                     .accepted);
    EXPECT_EQ (afterFirstOfGesture, runtime.History (graphId).stepsRecorded) << "the second edit folded into the first";

    // 2. UNDO AND REDO move the stacks without recording anything new.
    const uint64_t beforeUndo = runtime.History (graphId).stepsRecorded;
    ASSERT_TRUE (runtime.Undo (graphId).accepted);
    EXPECT_EQ (beforeUndo, runtime.History (graphId).stepsRecorded);
    EXPECT_EQ (1U, runtime.History (graphId).redoCount);
    ASSERT_TRUE (runtime.Redo (graphId).accepted);
    EXPECT_EQ (beforeUndo, runtime.History (graphId).stepsRecorded);
    EXPECT_EQ (0U, runtime.History (graphId).redoCount);
}

TEST (NodeGraphUndo, StepsRecordedStillMovesWhenTheStackIsFull)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-steps-at-cap";
    runtime.SetHistoryDepth (graphId, 2);

    for (int index = 0; index < 2; ++index)
        ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("n" + std::to_string (index), 0.0)).accepted);

    const HistoryState full = runtime.History (graphId);
    ASSERT_EQ (2U, full.undoCount);

    ASSERT_TRUE (runtime.Apply (graphId, AddNumberNode ("n2", 0.0)).accepted);
    const HistoryState after = runtime.History (graphId);

    // THIS IS THE CASE THE COUNT CANNOT ANSWER: a step was recorded AND the
    // oldest was dropped, so the size is identical either way.
    EXPECT_EQ (full.undoCount, after.undoCount);
    EXPECT_EQ (full.stepsRecorded + 1, after.stepsRecorded);
}

TEST (NodeGraphUndo, AnAbsurdDepthIsClampedRatherThanObeyed)
{
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "history-clamp";

    runtime.SetHistoryDepth (graphId, 0);
    EXPECT_EQ (kMinUndoDepth, runtime.History (graphId).depth) << "zero would mean no undo at all";

    runtime.SetHistoryDepth (graphId, 100000);
    EXPECT_EQ (kMaxUndoDepth, runtime.History (graphId).depth) << "each step is a whole document copy";
}

// --- Stage C: Archicad binding ---------------------------------------------
//
// The Archicad nodes are written against IArchicadHost, so all of this runs
// offline against a stub. The only untested code is the ACAPI implementation of
// that interface.

namespace {

class StubGenerationSource final : public IProjectGenerationSource {
  public:
    bool available = true;
    uint64_t project = 1;
    uint64_t selection = 1;

    bool Sample (GenerationDomain domain, uint64_t& value, std::string& error) const override
    {
        if (!available) {
            error = "no project is open";
            return false;
        }
        value = domain == GenerationDomain::Selection ? selection : project;
        return true;
    }
};

class StubResolver final : public IReferenceResolver {
  public:
    std::set<std::string> present;
    mutable int resolveAllCalls = 0;

    ReferenceResolution Resolve (const Reference& reference) const override
    {
        ReferenceResolution resolution;
        if (present.contains (reference.id)) {
            resolution.status = ResolutionStatus::Resolved;
            return resolution;
        }
        resolution.status = ResolutionStatus::Missing;
        resolution.detail = "element " + reference.id + " is not in this project";
        return resolution;
    }

    std::vector<ReferenceResolution> ResolveAll (const std::vector<Reference>& references) const override
    {
        ++resolveAllCalls;
        return IReferenceResolver::ResolveAll (references);
    }
};

class StubHost final : public IArchicadHost {
  public:
    bool available = true;
    std::vector<ArchicadElementRef> selection;
    std::vector<ArchicadElementRef> applied;
    int setSelectionCalls = 0;
    bool setSelectionFails = false;

    StubGenerationSource generationSource;
    StubResolver resolver;

    bool IsAvailable () const override
    {
        return available;
    }
    const IProjectGenerationSource& Generations () const override
    {
        return generationSource;
    }
    const IReferenceResolver& References () const override
    {
        return resolver;
    }
    bool GetSelection (std::vector<ArchicadElementRef>& elements, std::string&) const override
    {
        elements = selection;
        return true;
    }
    // What the stub says each guid is. A guid with no entry reads back as
    // unclassified, which is the same answer the real host gives for a type this
    // build does not name.
    std::map<std::string, std::string> typeOf;
    bool describeFails = false;
    mutable int describeCalls = 0;

    bool DescribeElements (const std::vector<ArchicadElementRef>& elements,
                           std::vector<ElementDescription>& descriptions, std::string& error) const override
    {
        ++describeCalls;
        if (describeFails) {
            error = "Archicad did not respond";
            return false;
        }
        for (const ArchicadElementRef& element : elements) {
            ElementDescription description;
            description.guid = element.guid;
            description.available = resolver.present.contains (element.guid);
            if (!description.available) {
                description.detail = "element " + element.guid + " is not in this project";
                descriptions.push_back (std::move (description));
                continue;
            }
            const auto found = typeOf.find (element.guid);
            if (found != typeOf.end ()) {
                description.elementType = found->second;
                description.typeLabel = found->second;
            }
            descriptions.push_back (std::move (description));
        }
        return true;
    }
    bool SetSelection (const std::vector<ArchicadElementRef>& elements, std::string& error) override
    {
        ++setSelectionCalls;
        if (setSelectionFails) {
            error = "Archicad refused the selection";
            return false;
        }
        applied = elements;
        return true;
    }

    // Convenience: put these guids in the selection and make them resolvable.
    void Holds (std::initializer_list<std::string> guids)
    {
        selection.clear ();
        for (const std::string& guid : guids) {
            selection.push_back (ArchicadElementRef { guid });
            resolver.present.insert (guid);
        }
    }

    // The same, with a type each.
    void HoldsTyped (std::initializer_list<std::pair<std::string, std::string>> typed)
    {
        selection.clear ();
        for (const auto& [guid, type] : typed) {
            selection.push_back (ArchicadElementRef { guid });
            resolver.present.insert (guid);
            typeOf[guid] = type;
        }
    }
};

// A selection-set node already holding `guids`. The set is an ordinary
// parameter, so seeding one is an ordinary node with a parameter - which is the
// simplification the node's redesign bought.
Node SelectionSetNode (const std::string& nodeId, std::initializer_list<std::string> guids)
{
    std::vector<Value> elements;
    for (const std::string& guid : guids)
        elements.emplace_back (ArchicadElementRef { guid });
    Node node { nodeId, "archicad.getSelection" };
    node.parameters.emplace ("elements", Argument::FromItems (std::move (elements)));
    return node;
}

EvaluationOutcome RunWithHost (Evaluator& evaluator, const GraphDocument& graph, const NodeRegistry& registry,
                               const NodeExecutor& executor, IArchicadHost* host, bool allowSideEffects = false,
                               RunId runId = 1)
{
    RunContext context;
    context.runId = runId;
    context.archicad = host;
    EvaluationRequest request;
    request.allowSideEffects = allowSideEffects;
    return evaluator.Evaluate (graph, registry, executor, request, context);
}

} // namespace

TEST (NodeGraphArchicad, ASelectionSetEvaluatesToWhatItHoldsWithNoHostAtAll)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a", "guid-b" }) } })
            .accepted);

    Evaluator evaluator;
    // NO HOST. The set is a captured parameter, not a live read, so the node
    // evaluates offline - which is what makes a saved graph inspectable before
    // a project is even open.
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, nullptr);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("sel");
    ASSERT_NE (nullptr, result);
    EXPECT_EQ (2, std::get<int64_t> (Out (result, "count").DataValue ()));
    const Argument elementsValue = Out (result, "elements");
    const std::vector<Value>& elements = elementsValue.Items ();
    ASSERT_EQ (2U, elements.size ());
    EXPECT_EQ ("guid-a", std::get<ArchicadElementRef> (elements[0].DataValue ()).guid);
}

TEST (NodeGraphArchicad, ALibraryPartNodeSplitsItsStoredChoiceAndNeedsNoHost)
{
    // ⚠️ Pure AND HOSTLESS, like the selection set and for the same reason: the
    // choice is a thing the USER made and stored, not a question about the model.
    // That is what lets a saved workflow name its objects with no project open.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    Node node { "part", "archicad.libraryPart" };
    node.parameters.emplace ("part",
                             Value (std::string (R"({"name":"Armchair 01","unID":"1234-ABCD","type":"Object",)"
                                                 R"("file":"Armchair 01.gsm","location":"C:/lib/Armchair 01.gsm"})")));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { node } }).accepted);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, nullptr);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("part");
    ASSERT_NE (nullptr, result);
    EXPECT_EQ ("Armchair 01", std::get<std::string> (Out (result, "name").DataValue ()));
    // ⚠️ THE unID IS AN OUTPUT IN ITS OWN RIGHT. A document name is unique only
    // in that Archicad registers the newest part carrying it, so a downstream
    // node that recorded only the name would pick a different object the day a
    // second library loads one with the same name.
    EXPECT_EQ ("1234-ABCD", std::get<std::string> (Out (result, "unID").DataValue ()));
    EXPECT_EQ ("Object", std::get<std::string> (Out (result, "type").DataValue ()));
}

TEST (NodeGraphArchicad, AnUnchosenOrUnreadableLibraryPartIsEmptyRatherThanAFailedGraph)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    Evaluator evaluator;

    // Nothing chosen yet: the node is something you drop and then browse from,
    // and a red node for the seconds in between is noise.
    GraphDocument bare;
    ASSERT_TRUE (
        ApplyEdit (bare, registry, GraphEdit { AddNodeEdit { Node { "part", "archicad.libraryPart" } } }).accepted);
    EvaluationOutcome outcome = RunWithHost (evaluator, bare, registry, ExecuteRuntimeNode, nullptr);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ ("", std::get<std::string> (Out (evaluator.Result ("part"), "name").DataValue ()));

    // A blob that will not parse can only come from a hand-edited file. Losing
    // the choice is better than refusing to open the document.
    GraphDocument broken;
    Node node { "part", "archicad.libraryPart" };
    node.parameters.emplace ("part", Value (std::string ("{not json")));
    ASSERT_TRUE (ApplyEdit (broken, registry, GraphEdit { AddNodeEdit { node } }).accepted);
    Evaluator second;
    outcome = RunWithHost (second, broken, registry, ExecuteRuntimeNode, nullptr);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ ("", std::get<std::string> (Out (second.Result ("part"), "unID").DataValue ()));
}

TEST (NodeGraphArchicad, TheLibraryPartWidgetCarriesNoOptionsBecauseTheLibrariesAreArchicadsAnswer)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* type = registry.Find ("archicad.libraryPart");
    ASSERT_NE (nullptr, type);
    ASSERT_EQ (1U, type->parameters.size ());
    ASSERT_TRUE (type->parameters[0].ui.has_value ());
    EXPECT_EQ (ParameterWidget::LibraryPart, type->parameters[0].ui->widget);
    EXPECT_EQ (ValueType::String, type->parameters[0].valueType);
    // ⚠️ A NODE TYPE MUST NOT SHIP A LIST OF PART NAMES. What is loaded changes
    // with the open project, so a literal option list would be the catalog
    // asserting something about a project it has never seen. The registry refuses
    // one rather than leaving it to be noticed.
    EXPECT_TRUE (type->parameters[0].ui->options.empty ());
    EXPECT_EQ (ParameterOptionSource::None, type->parameters[0].ui->optionSource);

    NodeRegistry fresh;
    NodeType offender;
    offender.id = "offender";
    offender.label = "Offender";
    offender.category = "Test";
    ParameterSchema parameter { "part", "Object", ValueType::String, false, Value (std::string {}) };
    ParameterUi ui;
    ui.widget = ParameterWidget::LibraryPart;
    ui.options.push_back ({ "Armchair 01", Value (std::string ("Armchair 01")) });
    parameter.ui = ui;
    offender.parameters.push_back (parameter);
    std::string error;
    EXPECT_FALSE (fresh.Register (std::move (offender), error));
    EXPECT_NE (std::string::npos, error.find ("libraryPart"));
}

TEST (NodeGraphArchicad, AContainerKeepsOnlyItsOwnTypeAndReadsTheWholeListAtOnce)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "w", "s", "w2", "gone" }) } })
            .accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "walls", ElementContainerNodeType ("wall") } } })
            .accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Edge { "sel", "elements", "walls", "elements" } } })
            .accepted);

    StubHost host;
    host.HoldsTyped ({ { "w", "wall" }, { "s", "slab" }, { "w2", "wall" } });
    // "gone" is deliberately NOT resolvable: it is in the captured set but no
    // longer in the project.

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("walls");
    ASSERT_NE (nullptr, result);
    EXPECT_EQ (2, std::get<int64_t> (Out (result, "count").DataValue ()));
    const Argument keptValue = Out (result, "elements");
    const std::vector<Value>& kept = keptValue.Items ();
    ASSERT_EQ (2U, kept.size ());
    EXPECT_EQ ("w", std::get<ArchicadElementRef> (kept[0].DataValue ()).guid);
    EXPECT_EQ ("w2", std::get<ArchicadElementRef> (kept[1].DataValue ()).guid);

    // ⚠️ ONE READ FOR THE WHOLE LIST. A per-element describe of a 500-element
    // selection is 500 crossings of the main-thread gate, which MainThreadGate
    // measured at roughly 0.6-8ms each.
    EXPECT_EQ (1, host.describeCalls);
}

TEST (NodeGraphArchicad, AContainerDropsAnElementItCouldNotReadRatherThanGuessingItsType)
{
    // "I could not tell what this is" must not answer "it is a wall". A
    // container that quietly admitted unreadable elements would hand them
    // downstream to nodes that assume the type.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "gone" }) } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "walls", ElementContainerNodeType ("wall") } } })
            .accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Edge { "sel", "elements", "walls", "elements" } } })
            .accepted);

    StubHost host;
    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (0, std::get<int64_t> (Out (evaluator.Result ("walls"), "count").DataValue ()));
}

TEST (NodeGraphArchicad, AnUnwiredContainerIsEmptyRatherThanBroken)
{
    // It is a thing you drop on the canvas and wire up afterwards. A node
    // reporting a failure for the ten seconds between those two acts is noise.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "slabs", ElementContainerNodeType ("slab") } } })
            .accepted);

    StubHost host;
    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (0, std::get<int64_t> (Out (evaluator.Result ("slabs"), "count").DataValue ()));
    EXPECT_EQ (0, host.describeCalls);
}

TEST (NodeGraphArchicad, TheSelectionSetStaysCleanWhileTheUserClicksAroundInTheModel)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a" }) } }).accepted);

    StubHost host;
    host.Holds ({ "guid-a" });

    Evaluator evaluator;
    EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 1);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (1U, outcome.executedCount);

    // THE BEHAVIOUR CHANGE, and the reason the node was redesigned. The user
    // selects something else entirely and the selection generation moves. A node
    // that mirrored the live selection would go dirty here, re-run, and drag
    // every downstream node with it - so a graph silently answered differently
    // every time somebody clicked in the model. A captured set does not.
    host.Holds ({ "guid-a", "guid-c", "guid-d" });
    host.generationSource.selection = 2;
    outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 2);
    EXPECT_EQ (0U, outcome.executedCount);
    EXPECT_EQ (1U, outcome.cacheHitCount);
    EXPECT_EQ (1, std::get<int64_t> (Out (evaluator.Result ("sel"), "count").DataValue ()));
}

TEST (NodeGraphArchicad, AGraphNeedingArchicadIsRefusedAtTheDoorWithoutAHost)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    // Set Selection is the node that genuinely needs the host now: the set does
    // not, because it evaluates to what it holds.
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a" }) } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "set", "archicad.setSelection" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("sel", "elements", "set", "elements") } })
            .accepted);

    Evaluator evaluator;
    // No host at all - the offline and headless case.
    EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, nullptr);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, outcome.error.find ("needs an open Archicad project"));
    // Refused at the door means nothing executed.
    EXPECT_EQ (0U, outcome.executedCount);

    // A host that exists but has no project open is the same answer.
    StubHost host;
    host.available = false;
    outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 2);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, outcome.error.find ("needs an open Archicad project"));
}

TEST (NodeGraphArchicad, SetSelectionIsRefusedUnlessTheRunAsksForSideEffects)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a" }) } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "set", "archicad.setSelection" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("sel", "elements", "set", "elements") } })
            .accepted);

    StubHost host;
    host.Holds ({ "guid-a" });

    Evaluator evaluator;

    // A preview. The upstream read still runs - reading cannot surprise anyone -
    // but nothing touches the user's selection.
    EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 1);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (0, host.setSelectionCalls);
    EXPECT_FALSE (outcome.effectsCommitted);
    EXPECT_EQ ((std::vector<NodeId> { "set" }), outcome.skippedEffectNodes);
    // Stage F1 retired `Skipped`. A withheld side effect is Blocked - there is
    // no value - and the CODE is what says it was withheld on purpose rather
    // than lost to a broken upstream. Asserting the pair is the point: a client
    // that read only the state would show this node as an error.
    EXPECT_EQ (NodeExecutionState::Blocked, evaluator.Status ("set").state);
    EXPECT_EQ (statuscode::kBlockedSideEffects, evaluator.Status ("set").code);
    EXPECT_EQ (NodeExecutionState::Success, evaluator.Status ("sel").state);

    // A deliberate Run.
    outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, true, 2);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_TRUE (outcome.effectsCommitted);
    EXPECT_EQ (1, host.setSelectionCalls);
    ASSERT_EQ (1U, host.applied.size ());
    EXPECT_EQ ("guid-a", host.applied[0].guid);
}

TEST (NodeGraphArchicad, AFailedGraphNeverReachesTheSelection)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeType exploder;
    exploder.id = "exploder";
    exploder.outputs.push_back ({ "elements", "Elements", ValueType::List });
    std::string error;
    ASSERT_TRUE (registry.Register (std::move (exploder), error)) << error;

    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "bad", "exploder" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "set", "archicad.setSelection" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("bad", "elements", "set", "elements") } })
            .accepted);

    StubHost host;
    host.available = true;

    const NodeExecutor executor = [] (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                                      ValueMap& outputs, std::string& nodeError) {
        if (node.nodeType == "exploder") {
            nodeError = "deliberate";
            return false;
        }
        return ExecuteRuntimeNode (node, inputs, context, outputs, nodeError);
    };

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, executor, &host, true);
    EXPECT_FALSE (outcome.succeeded);
    // The user's selection is untouched. This is the rule the deferred phase
    // exists to enforce: a graph that failed does not go on to change Archicad.
    EXPECT_EQ (0, host.setSelectionCalls);
    EXPECT_FALSE (outcome.effectsCommitted);
    EXPECT_EQ (NodeExecutionState::Blocked, evaluator.Status ("set").state);
}

TEST (NodeGraphArchicad, AStaleReferenceFailsSetSelectionInsteadOfSelectingASubset)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a", "guid-b" }) } })
            .accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "set", "archicad.setSelection" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("sel", "elements", "set", "elements") } })
            .accepted);

    StubHost host;
    host.Holds ({ "guid-a", "guid-b" });

    Evaluator evaluator;
    ASSERT_TRUE (RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, true, 1).succeeded);
    ASSERT_EQ (2U, host.applied.size ());

    // One of them is deleted between runs. Selecting only the survivor would
    // look like a correct answer and would not be one.
    //
    // The PROJECT generation is what moves here, not the selection one: deleting
    // an element changes the model, and the selection set deliberately no longer
    // notices the user selecting something else. Set Selection declares Project,
    // so this is what re-runs it.
    host.resolver.present.erase ("guid-b");
    host.generationSource.project = 2;
    const int callsBefore = host.setSelectionCalls;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, true, 2);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_EQ (callsBefore, host.setSelectionCalls);
    EXPECT_NE (std::string::npos, evaluator.Status ("set").message.find ("guid-b"));
}

TEST (NodeGraphArchicad, ReferencesAreResolvedInOneBatchNotOnePerElement)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry,
                            GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "a", "b", "c", "d", "e", "f" }) } })
                     .accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "set", "archicad.setSelection" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("sel", "elements", "set", "elements") } })
            .accepted);

    StubHost host;
    host.Holds ({ "a", "b", "c", "d", "e", "f" });

    Evaluator evaluator;
    ASSERT_TRUE (RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, true).succeeded);
    // Six elements, ONE crossing. Against the real host each crossing is a
    // MainThreadGate round trip of roughly 0.6-8ms, so per-element resolution
    // would be seconds of pure marshalling on a large selection.
    EXPECT_EQ (1, host.resolver.resolveAllCalls);
}

TEST (NodeGraphReports, OnePassAnswersRunnabilityAndLoadabilityDifferently)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    // Set Selection, not the selection set: the set evaluates to what it holds
    // and so needs no project, which is the point of the redesign.
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.setSelection" } } }).accepted);

    // No host: the graph cannot RUN...
    const GraphResolution offline = ResolveGraph (graph, registry, nullptr);
    const GraphDependencyReport dependencies = MakeDependencyReport (graph, registry, offline);
    EXPECT_FALSE (dependencies.canEvaluate);
    EXPECT_EQ (1U, dependencies.nodesNeedingArchicad);

    // ...but it LOADS perfectly well. Refusing to open a file for editing
    // because no project happens to be open would be wrong, and that difference
    // is the reason these are two projections rather than one report.
    const CompatibilityReport compatibility = MakeCompatibilityReport (graph, registry, offline, 0);
    EXPECT_EQ (CompatibilityStatus::Compatible, compatibility.status);
    EXPECT_TRUE (compatibility.missingNodeTypes.empty ());

    StubHost host;
    const GraphResolution online = ResolveGraph (graph, registry, &host);
    EXPECT_TRUE (MakeDependencyReport (graph, registry, online).canEvaluate);
}

TEST (NodeGraphReports, AMissingNodeTypeIsBothUnrunnableAndUnloadable)
{
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    // Author against a registry that has the type, then evaluate against one
    // that does not - the after-upgrade case.
    NodeType vanishing;
    vanishing.id = "vanishing";
    vanishing.outputs.push_back ({ "value", "Value", ValueType::Double });
    std::string error;
    ASSERT_TRUE (registry.Register (std::move (vanishing), error)) << error;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "gone", "vanishing" } } }).accepted);

    const NodeRegistry upgraded = MakeRuntimeNodeRegistry ();
    const GraphResolution resolution = ResolveGraph (graph, upgraded, nullptr);

    EXPECT_FALSE (MakeDependencyReport (graph, upgraded, resolution).canEvaluate);

    const CompatibilityReport compatibility = MakeCompatibilityReport (graph, upgraded, resolution, 0);
    EXPECT_EQ (CompatibilityStatus::MissingNodeType, compatibility.status);
    ASSERT_EQ (1U, compatibility.missingNodeTypes.size ());
    EXPECT_EQ ("vanishing", compatibility.missingNodeTypes.front ());
}

TEST (NodeGraphReports, FormatVersionsAreJudgedBeforeAnythingElse)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    const GraphResolution resolution = ResolveGraph (graph, registry, nullptr);

    EXPECT_EQ (CompatibilityStatus::Compatible,
               MakeCompatibilityReport (graph, registry, resolution, kGraphFormatVersion).status);
    // Not stated - an in-memory document - is treated as current.
    EXPECT_EQ (CompatibilityStatus::Compatible, MakeCompatibilityReport (graph, registry, resolution, 0).status);
    EXPECT_EQ (CompatibilityStatus::UnsupportedFormat,
               MakeCompatibilityReport (graph, registry, resolution, kGraphFormatVersion + 1).status);
}

TEST (NodeGraphArchicad, TheProjectClosingMidRunFailsTheNodeRatherThanTheProcess)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a" }) } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "set", "archicad.setSelection" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("sel", "elements", "set", "elements") } })
            .accepted);

    StubHost host;
    host.Holds ({ "guid-a" });

    // The plan is built while the project is open; it closes before the node
    // runs. ExecuteArchicadNode re-checks for exactly this.
    const NodeExecutor closing = [&host] (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                                          ValueMap& outputs, std::string& nodeError) {
        host.available = false;
        return ExecuteRuntimeNode (node, inputs, context, outputs, nodeError);
    };

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, closing, &host, true);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, evaluator.Status ("set").message.find ("no longer available"));
}

// --- The Panel node --------------------------------------------------------

TEST (NodeGraphPanel, AcceptsAnyValueTypeThroughOneWildcardInput)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();

    // One panel, three source types. Without the wildcard input this would need
    // a panel variant per value type.
    for (const char* source : { "number", "makeList", "archicad.getSelection" }) {
        GraphDocument graph;
        Node sourceNode = std::string (source) == "archicad.getSelection" ? SelectionSetNode ("src", { "guid-a" })
                                                                          : Node { "src", source };
        sourceNode.id = "src";
        if (std::string (source) == "number")
            sourceNode.parameters.emplace ("value", Value (1.0));
        ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { sourceNode } }).accepted) << source;
        ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);

        const char* port = std::string (source) == "archicad.getSelection" ? "elements" : "value";
        const EditResult connected =
            ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("src", port, "panel", "value") } });
        EXPECT_TRUE (connected.accepted) << source << ": " << connected.error;
    }
}

TEST (NodeGraphPanel, PassesItsInputThroughWITHOUTReshapingTheTree)
{
    // The whole contract of the node in one assertion: what a panel publishes is
    // the tree it was handed, branch for branch. A value-level check (same items,
    // same order) would pass for a panel that silently flattened what crossed it,
    // which is the one thing an inspector on a wire must never do.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    Node two { "two", "number" };
    two.parameters.emplace ("value", Value (2.0));
    Node three { "three", "number" };
    three.parameters.emplace ("value", Value (3.0));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { two } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { three } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "list", "makeList" } } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "graft", "tree.graft" } } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("two", "value", "list", "items") } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("three", "value", "list", "items") } })
                     .accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("list", "value", "graft", "tree") } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("graft", "tree", "panel", "value") } })
                     .accepted);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteRuntimeNode, { "panel" });
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> panelResult = evaluator.Result ("panel");
    const std::shared_ptr<const NodeResult> graftResult = evaluator.Result ("graft");
    ASSERT_NE (nullptr, panelResult);
    ASSERT_NE (nullptr, graftResult);
    const data::TreeValue& shown = panelResult->outputs.at ("value");
    const data::TreeValue& upstream = graftResult->outputs.at ("tree");
    ASSERT_TRUE (shown.IsPresent ());
    // Two branches, one item each - a graft, not a list - and the SAME pointer
    // the graft published: the panel copies nothing and reshapes nothing.
    EXPECT_EQ (2U, shown.tree->ListCount ());
    EXPECT_EQ (upstream.tree.get (), shown.tree.get ());

    // ONE output, and it is the data. The four derived readouts the node used to
    // publish are gone: a client renders them from this output's `text`,
    // `summary` and `branches` in GraphGetNodeResults.
    ASSERT_EQ (1U, registry.Find ("panel")->outputs.size ());
    EXPECT_EQ ("value", registry.Find ("panel")->outputs.front ().id);
}

TEST (NodeGraphPanel, TakesSeveralWiresIntoItsOneInputAndShowsTheMergedTree)
{
    // Grasshopper's panel is a parameter several sources can feed at once. The
    // port declares acceptsMultiple, so the merge is the runtime's documented
    // fan-in rather than a rule this node invents (InputGathering.cpp).
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    Node two { "two", "number" };
    two.parameters.emplace ("value", Value (2.0));
    Node three { "three", "number" };
    three.parameters.emplace ("value", Value (3.0));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { two } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { three } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("two", "value", "panel", "value") } }).accepted);
    const EditResult second =
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("three", "value", "panel", "value") } });
    EXPECT_TRUE (second.accepted) << second.error;

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteRuntimeNode, { "panel" });
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("panel");
    ASSERT_NE (nullptr, result);
    const Argument shown = Out (result, "value");
    ASSERT_EQ (2U, shown.Items ().size ());
    EXPECT_EQ (2.0, std::get<double> (shown.Items ()[0].DataValue ()));
    EXPECT_EQ (3.0, std::get<double> (shown.Items ()[1].DataValue ()));
}

TEST (NodeGraphPanel, WithNothingWiredReportsTheEmptyTreeRatherThanFailing)
{
    // An unwired panel is a panel waiting to be used. Marking the input required
    // would make the evaluator refuse it before the body ever ran, and a node
    // that turns red for being new is a node people delete.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteRuntimeNode, { "panel" });
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    const std::shared_ptr<const NodeResult> result = evaluator.Result ("panel");
    ASSERT_NE (nullptr, result);
    EXPECT_TRUE (Out (result, "value").Items ().empty ());
}

TEST (NodeGraphPanel, RendersEveryValueTypeReadably)
{
    EXPECT_EQ ("(none)", FormatValue (Value {}));
    EXPECT_EQ ("true", FormatValue (Value (true)));
    EXPECT_EQ ("42", FormatValue (Value (int64_t { 42 })));
    // Not "1.500000" - a panel full of trailing zeroes is a panel a person has
    // to squint past.
    EXPECT_EQ ("1.5", FormatValue (Value (1.5)));
    EXPECT_EQ ("hello", FormatValue (Value (std::string ("hello"))));
    EXPECT_EQ ("(1, 2, 3)", FormatValue (Value (Point3 { 1.0, 2.0, 3.0 })));
    EXPECT_EQ ("guid-a", FormatValue (Value (ArchicadElementRef { "guid-a" })));
    EXPECT_EQ ("[1, 2]", FormatValue (Argument::FromItems ({ Value (1.0), Value (2.0) })));

    auto mesh = std::make_shared<geomsrv::Mesh> ();
    mesh->vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh->triangles = { 0, 1, 2 };
    EXPECT_EQ ("Mesh (3 vertices, 1 triangles)", FormatValue (Value (Value::ImmutableMesh (mesh))));

    // An empty list says so rather than rendering as nothing at all.
    EXPECT_EQ ((std::vector<std::string> { "(empty list)" }), FormatValueLines (Argument::FromItems ({})));
}

TEST (NodeGraphPanel, TruncatesLargeAndDeepValuesAndSaysThatItDid)
{
    std::vector<Value> big;
    for (int i = 0; i < 40; ++i)
        big.emplace_back (static_cast<int64_t> (i));

    const std::vector<std::string> lines = FormatValueLines (Argument::FromItems (big), 10);
    ASSERT_EQ (10U, lines.size ());
    // A quietly shortened list reads as a wrong answer, so the last line says
    // how many were left out.
    EXPECT_NE (std::string::npos, lines.back ().find ("more of 40"));

    // Inline rendering caps items too.
    EXPECT_NE (std::string::npos, FormatValue (Argument::FromItems (big)).find ("more"));

    // Depth-based truncation ("past the limit a nested list renders as its
    // shape") used to be reachable by building eight levels of Value::List by
    // hand. It no longer is: a Value cannot hold a List at all, so a branch's
    // items are always scalar and FormatArgument's depth can never exceed one
    // level - the same §7.3 invariant this file adapts elsewhere.
}

TEST (NodeGraphPanel, ATypedInputStillRejectsTheWrongType)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    Node number { "num", "number" };
    number.parameters.emplace ("value", Value (1.0));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { number } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "watch", "watch" } } }).accepted);

    // Watch takes a List. The wildcard is a property of Absent inputs only - it
    // must not have loosened type checking everywhere.
    const EditResult rejected =
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("num", "value", "watch", "value") } });
    EXPECT_FALSE (rejected.accepted);
    EXPECT_NE (std::string::npos, rejected.error.find ("type mismatch"));
}

// ---------------------------------------------------------------------------
// Stage D - domains and parallelism.
//
// ADR-007's gate is that pure nodes DEMONSTRABLY execute concurrently, so these
// MEASURE overlap rather than assert that a pool exists. The measurement is peak
// observed concurrency, counted by the node bodies themselves: a wall-clock
// ratio on a loaded build machine is a flaky test, and a rendezvous that cannot
// complete unless N bodies are genuinely inside at once is not.
// ---------------------------------------------------------------------------

namespace {

// How many bodies can be inside a level at once on THIS machine. The pool is
// sized from hardware concurrency, so a test that hard-codes four passes on a
// developer's desktop and hangs on a two-core build agent.
size_t AvailableConcurrency ()
{
    return SharedWorkerPool ().ThreadCount () + 1;
}

// Blocks each body until `expected` of them have arrived, which is what proves
// overlap without sleeping for a fixed time.
class Rendezvous {
  public:
    void Expect (size_t expected)
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        expected_ = expected;
        peak_ = 0;
        inside_ = 0;
        arrivals_ = 0;
    }

    void Enter ()
    {
        std::unique_lock<std::mutex> lock (mutex_);
        ++inside_;
        // `arrivals_` is monotonic and `inside_` is not, and the wait must use
        // the monotonic one: the last body to arrive decrements `inside_` on its
        // way out, so a predicate reading it would go false again under the
        // bodies still waiting and cost every one of them the full timeout.
        ++arrivals_;
        peak_ = std::max (peak_, inside_);
        if (arrivals_ >= expected_) {
            arrived_.notify_all ();
        }
        else {
            // Bounded, so a run that CANNOT reach the count fails an assertion
            // rather than hanging the whole suite.
            arrived_.wait_for (lock, std::chrono::seconds (2), [this] () { return arrivals_ >= expected_; });
        }
        --inside_;
    }

    size_t Peak () const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return peak_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    size_t inside_ = 0;
    size_t arrivals_ = 0;
    size_t peak_ = 0;
    size_t expected_ = 1;
};

Rendezvous& SharedRendezvous ()
{
    static Rendezvous rendezvous;
    return rendezvous;
}

NodeRegistry MakeParallelRegistry ()
{
    NodeRegistry registry;
    std::string error;

    NodeType slow;
    slow.id = "slow";
    slow.label = "Slow";
    slow.executionDomain = ExecutionDomain::Worker;
    slow.outputs.push_back ({ "value", "Value", ValueType::Integer });
    EXPECT_TRUE (registry.Register (std::move (slow), error)) << error;

    return registry;
}

bool ExecuteSlowNode (const Node&, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs, std::string&)
{
    SharedRendezvous ().Enter ();
    outputs.emplace ("value", Value (int64_t { 1 }));
    return true;
}

// The number/add registry's bodies, as a named executor several Stage D tests
// share.
bool ExecuteNumberAddNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext&, ValueMap& outputs,
                           std::string&)
{
    if (node.nodeType == "number")
        outputs.emplace ("value", node.parameters.at ("value"));
    else
        outputs.emplace ("sum", Value (Integer (inputs.at ("left")) + Integer (inputs.at ("right"))));
    return true;
}

GraphDocument MakeIndependentSlowNodes (const NodeRegistry& registry, size_t count)
{
    GraphDocument graph;
    for (size_t i = 0; i < count; ++i) {
        const std::string id (1, static_cast<char> ('a' + static_cast<int> (i)));
        EXPECT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { id, "slow" } } }).accepted);
    }
    return graph;
}

} // namespace

TEST (NodeGraphWorkerPool, RunsEveryTaskExactlyOnceAndHonoursTheCap)
{
    for (const size_t cap : { size_t { 1 }, size_t { 2 }, size_t { 4 } }) {
        WorkerPool pool (3);
        std::vector<std::atomic<int>> visits (32);
        std::atomic<size_t> inFlight { 0 };
        std::atomic<size_t> peak { 0 };

        pool.RunBatch (visits.size (), cap, [&] (size_t index) {
            const size_t now = inFlight.fetch_add (1) + 1;
            size_t seen = peak.load ();
            while (now > seen && !peak.compare_exchange_weak (seen, now)) {
            }
            visits[index].fetch_add (1);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
            inFlight.fetch_sub (1);
        });

        for (const std::atomic<int>& visited : visits)
            EXPECT_EQ (1, visited.load ());
        EXPECT_LE (peak.load (), cap) << "cap " << cap << " was exceeded";
    }
}

TEST (NodeGraphWorkerPool, ContainsAThrowingTaskRatherThanStrandingTheBatch)
{
    WorkerPool pool (2);
    std::atomic<int> completed { 0 };
    pool.RunBatch (8, 3, [&] (size_t index) {
        if (index % 2 == 0)
            throw std::runtime_error ("task failed");
        completed.fetch_add (1);
    });
    EXPECT_EQ (4, completed.load ());
}

TEST (NodeGraphWorkerPool, ZeroThreadsRunsTheBatchOnTheSubmitter)
{
    WorkerPool pool (0);
    const std::thread::id submitter = std::this_thread::get_id ();
    std::atomic<int> ran { 0 };
    pool.RunBatch (5, 0, [&] (size_t) {
        EXPECT_EQ (submitter, std::this_thread::get_id ());
        ran.fetch_add (1);
    });
    EXPECT_EQ (5, ran.load ());
}

TEST (NodeGraphParallelism, IndependentPureNodesRunConcurrently)
{
    const size_t width = AvailableConcurrency ();
    if (width < 2)
        GTEST_SKIP () << "this machine offers no concurrency to measure";

    const NodeRegistry registry = MakeParallelRegistry ();
    // Nodes with no edge between them: one topological level, `width` ways.
    const GraphDocument graph = MakeIndependentSlowNodes (registry, width);
    SharedRendezvous ().Expect (width);

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    EvaluationRequest request;
    request.maxParallel = width;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, ExecuteSlowNode, request, context);

    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (width, outcome.executedCount);
    // The gate itself: every body was inside at one instant, which cannot happen
    // unless they genuinely ran at the same time.
    EXPECT_EQ (width, outcome.parallelism.peakConcurrency);
    EXPECT_EQ (width, SharedRendezvous ().Peak ());
    ASSERT_EQ (1U, outcome.parallelism.levels.size ());
    EXPECT_EQ (width, outcome.parallelism.levels.front ().workerNodeCount);
    EXPECT_EQ (0U, outcome.parallelism.levels.front ().hostNodeCount);
    // Work exceeds wall clock exactly to the extent the level overlapped.
    EXPECT_GT (outcome.parallelism.Speedup (), 1.0);
}

TEST (NodeGraphParallelism, MaxParallelOneIsTheSequentialArm)
{
    const NodeRegistry registry = MakeParallelRegistry ();
    const GraphDocument graph = MakeIndependentSlowNodes (registry, 4);
    // Nothing waits for anybody: at maxParallel 1 a rendezvous of four could
    // never complete, and the point of the arm is that the run still does.
    SharedRendezvous ().Expect (1);

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    EvaluationRequest request;
    request.maxParallel = 1;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, ExecuteSlowNode, request, context);

    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (4U, outcome.executedCount);
    EXPECT_EQ (1U, outcome.parallelism.peakConcurrency);
    EXPECT_EQ (1U, outcome.parallelism.maxParallel);
}

TEST (NodeGraphParallelism, ADependencyChainHasNoLevelToParallelise)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteNumberAddNode);

    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    // Two levels: the two numbers, then the add that consumes them. The second
    // has one member, so there is nothing in it to overlap - which is a property
    // of the graph and not a defect in the pool.
    ASSERT_EQ (2U, outcome.parallelism.levels.size ());
    EXPECT_EQ (2U, outcome.parallelism.levels[0].executedCount);
    EXPECT_EQ (1U, outcome.parallelism.levels[1].executedCount);
    EXPECT_EQ (3U, outcome.executedCount);
    EXPECT_EQ (3, Integer (Out (evaluator.Result ("sum"), "sum")));
}

TEST (NodeGraphParallelism, PublicationOrderFollowsTheLevelNotTheScheduler)
{
    const size_t width = AvailableConcurrency ();
    if (width < 2)
        GTEST_SKIP () << "this machine offers no concurrency to measure";

    const NodeRegistry registry = MakeParallelRegistry ();
    const GraphDocument graph = MakeIndependentSlowNodes (registry, width);
    SharedRendezvous ().Expect (width);

    RunEventLog log;
    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    context.events = [&log] (RunEvent&& event) { log.Append (std::move (event)); };
    EvaluationRequest request;
    request.maxParallel = width;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, ExecuteSlowNode, request, context);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    // Whatever order the bodies finished in, completions are published in level
    // order - otherwise the stream is a different document on every run and no
    // client can hold a snapshot against it.
    std::vector<NodeId> completed;
    for (const RunEvent& event : log.Since (kNoEvent, 1000).events) {
        if (event.kind == RunEventKind::NodeCompleted)
            completed.push_back (event.nodeId);
    }
    std::vector<NodeId> expected;
    for (size_t i = 0; i < width; ++i)
        expected.push_back (std::string (1, static_cast<char> ('a' + static_cast<int> (i))));
    EXPECT_EQ (expected, completed);
}

TEST (NodeGraphParallelism, AFaultInAPooledNodeFailsThatNodeOnly)
{
    NodeRegistry registry = MakeParallelRegistry ();
    std::string error;
    NodeType faulting;
    faulting.id = "faulting";
    faulting.label = "Faulting";
    faulting.outputs.push_back ({ "value", "Value", ValueType::Integer });
    ASSERT_TRUE (registry.Register (std::move (faulting), error)) << error;

    GraphDocument graph = MakeIndependentSlowNodes (registry, 2);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "boom", "faulting" } } }).accepted);
    SharedRendezvous ().Expect (1);

    const NodeExecutor executor = [] (const Node& node, const ValueMap& inputs, const NodeExecutionContext& execution,
                                      ValueMap& outputs, std::string& nodeError) {
        if (node.nodeType == "faulting")
            throw std::runtime_error ("the node threw on a pool thread");
        return ExecuteSlowNode (node, inputs, execution, outputs, nodeError);
    };

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    EvaluationRequest request;
    request.maxParallel = 3;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, executor, request, context);

    EXPECT_FALSE (outcome.succeeded);
    EXPECT_EQ (1U, outcome.failedCount);
    EXPECT_EQ ("boom", outcome.failedNode);
    // The independent siblings still completed: a fault contained on a pool
    // thread must not take the rest of its level with it.
    EXPECT_EQ (NodeExecutionState::Success, evaluator.Status ("a").state);
    EXPECT_EQ (NodeExecutionState::Success, evaluator.Status ("b").state);
}

TEST (NodeGraphParallelism, WorkerDomainNodesAreNeverGivenTheHost)
{
    NodeRegistry registry;
    std::string error;
    NodeType probe;
    probe.id = "probe";
    probe.label = "Probe";
    probe.executionDomain = ExecutionDomain::Worker;
    probe.outputs.push_back ({ "value", "Value", ValueType::Integer });
    ASSERT_TRUE (registry.Register (std::move (probe), error)) << error;

    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "p", "probe" } } }).accepted);

    StubHost host;
    bool sawHost = true;
    bool sawResolver = false;
    const NodeExecutor executor = [&] (const Node&, const ValueMap&, const NodeExecutionContext& execution,
                                       ValueMap& outputs, std::string&) {
        sawHost = execution.archicad != nullptr;
        // A worker-domain node still gets A resolver - the one that answers
        // Missing with a reason - rather than a null pointer it would have to
        // check for.
        sawResolver = execution.references != nullptr;
        outputs.emplace ("value", Value (int64_t { 1 }));
        return true;
    };

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    context.archicad = &host;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, executor, EvaluationRequest {}, context);

    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    // The structural half of "no worker thread calls ACAPI": the pointer is not
    // there to be called.
    EXPECT_FALSE (sawHost);
    EXPECT_TRUE (sawResolver);
}

TEST (NodeGraphParallelism, HostDomainNodesRunOnTheCoordinatorAndAreCountedSeparately)
{
    NodeRegistry registry = MakeParallelRegistry ();
    std::string error;
    // A test-local host-domain node rather than whichever production node
    // happens to be in that domain: this test is about the PARTITION, and
    // coupling it to the catalog is what made it break when Get Selection
    // stopped needing the host.
    NodeType hostNode;
    hostNode.id = "hostProbe";
    hostNode.label = "Host Probe";
    hostNode.executionDomain = ExecutionDomain::ArchicadMainThread;
    hostNode.effect = EffectKind::ReadModel;
    hostNode.outputs.push_back ({ "value", "Value", ValueType::Integer });
    ASSERT_TRUE (registry.Register (std::move (hostNode), error)) << error;

    GraphDocument graph = MakeIndependentSlowNodes (registry, 2);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "probe", "hostProbe" } } }).accepted);
    SharedRendezvous ().Expect (1);

    StubHost host;

    const std::thread::id coordinator = std::this_thread::get_id ();
    std::thread::id hostNodeThread;
    const NodeExecutor executor = [&] (const Node& node, const ValueMap& inputs, const NodeExecutionContext& execution,
                                       ValueMap& outputs, std::string& nodeError) {
        if (node.nodeType == "hostProbe") {
            hostNodeThread = std::this_thread::get_id ();
            // The host reaches a host-domain node and nothing else; that is the
            // other half of the partition.
            EXPECT_NE (nullptr, execution.archicad);
            outputs.emplace ("value", Value (int64_t { 1 }));
            return true;
        }
        return ExecuteSlowNode (node, inputs, execution, outputs, nodeError);
    };

    Evaluator evaluator;
    RunContext context;
    context.runId = 1;
    context.archicad = &host;
    EvaluationRequest request;
    request.maxParallel = 3;
    const EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, executor, request, context);

    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    // On the coordinator, so MainThreadGate::Invoke keeps its inline path when
    // the coordinator is Archicad's own thread.
    EXPECT_EQ (coordinator, hostNodeThread);
    ASSERT_EQ (1U, outcome.parallelism.levels.size ());
    EXPECT_EQ (2U, outcome.parallelism.levels.front ().workerNodeCount);
    EXPECT_EQ (1U, outcome.parallelism.levels.front ().hostNodeCount);
}

// ---------------------------------------------------------------------------
// Stage E1 - persistence.
//
// The format is DevKit-free on purpose, so these run offline: a workflow library
// that outlives the project it was authored in is exactly the thing that must be
// testable without a host.
// ---------------------------------------------------------------------------

namespace {

// A registry with one node type per persistable value kind, so a round trip test
// covers the vocabulary rather than the two types the other tests happen to use.
NodeRegistry MakeStorageRegistry ()
{
    NodeRegistry registry = MakeRegistry ();
    std::string error;

    NodeType parameters;
    parameters.id = "parameters";
    parameters.label = "Parameters";
    parameters.outputs.push_back ({ "out", "Out", ValueType::Integer });
    parameters.parameters.push_back ({ "flag", "Flag", ValueType::Bool });
    parameters.parameters.push_back ({ "count", "Count", ValueType::Integer });
    parameters.parameters.push_back ({ "ratio", "Ratio", ValueType::Double });
    parameters.parameters.push_back ({ "label", "Label", ValueType::String });
    parameters.parameters.push_back ({ "origin", "Origin", ValueType::Point3 });
    parameters.parameters.push_back ({ "path", "Path", ValueType::Polyline });
    parameters.parameters.push_back ({ "outline", "Outline", ValueType::Polygon });
    parameters.parameters.push_back ({ "element", "Element", ValueType::ArchicadElementRef });
    parameters.parameters.push_back ({ "items", "Items", ValueType::List });
    EXPECT_TRUE (registry.Register (std::move (parameters), error)) << error;

    return registry;
}

} // namespace

TEST (NodeGraphJson, RoundTripsEveryScalarKindAndPreservesIntegerness)
{
    using namespace evp::nodegraph::json;

    JsonObject root;
    root.emplace ("flag", JsonValue::Bool (true));
    root.emplace ("count", JsonValue::Integer (-42));
    root.emplace ("ratio", JsonValue::Double (0.1));
    root.emplace ("text", JsonValue::String ("a \"quoted\" line\nand a tab\t."));
    root.emplace ("nothing", JsonValue {});
    root.emplace ("items", JsonValue::Array (JsonArray { JsonValue::Integer (1), JsonValue::Integer (2) }));

    const std::string text = Write (JsonValue::Object (std::move (root)));
    const ParseResult parsed = Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;

    bool flag = false;
    EXPECT_TRUE (parsed.value.Find ("flag")->AsBool (flag));
    EXPECT_TRUE (flag);

    int64_t count = 0;
    EXPECT_TRUE (parsed.value.Find ("count")->AsInteger (count));
    EXPECT_EQ (-42, count);
    EXPECT_TRUE (parsed.value.Find ("count")->IsIntegral ());

    double ratio = 0.0;
    EXPECT_TRUE (parsed.value.Find ("ratio")->AsDouble (ratio));
    // 17 significant digits, so a stored parameter reloads to the same double
    // rather than to one that computes a slightly different model.
    EXPECT_DOUBLE_EQ (0.1, ratio);
    EXPECT_FALSE (parsed.value.Find ("ratio")->IsIntegral ());

    std::string decoded;
    EXPECT_TRUE (parsed.value.Find ("text")->AsString (decoded));
    EXPECT_EQ ("a \"quoted\" line\nand a tab\t.", decoded);

    EXPECT_TRUE (parsed.value.Find ("nothing")->IsNull ());
    ASSERT_NE (nullptr, parsed.value.Find ("items")->AsArray ());
    EXPECT_EQ (2U, parsed.value.Find ("items")->AsArray ()->size ());
}

TEST (NodeGraphJson, RejectsMalformedDocumentsWithAReason)
{
    using namespace evp::nodegraph::json;

    for (const char* bad : { "{", "{\"a\":}", "[1,]", "{\"a\":1}trailing", "\"unterminated", "{\"a\":1,\"a\":2}" })
        EXPECT_FALSE (Parse (bad).ok) << "accepted: " << bad;

    const ParseResult repeated = Parse ("{\"a\":1,\"a\":2}");
    EXPECT_NE (std::string::npos, repeated.error.find ("repeated"));
}

TEST (NodeGraphJson, RefusesToRecurseIntoADeeplyNestedDocument)
{
    using namespace evp::nodegraph::json;

    std::string deep;
    for (int i = 0; i < 500; ++i)
        deep += '[';
    for (int i = 0; i < 500; ++i)
        deep += ']';

    // Bounded rather than recursed into: a corrupt file must not be able to
    // overflow the stack inside Archicad's process.
    const ParseResult parsed = Parse (deep);
    EXPECT_FALSE (parsed.ok);
    EXPECT_NE (std::string::npos, parsed.error.find ("deeply"));
}

TEST (NodeGraphPersistence, RoundTripsAGraphThroughTextUnchanged)
{
    const NodeRegistry registry = MakeStorageRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 7).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 5).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    // A modifier travels with the document, because it changes what the graph
    // COMPUTES: a saved graph that lost one would open and quietly give a
    // different answer from the one its author saw.
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { SetPortModifierEdit { "sum", "left", PortModifier::Graft } }).accepted);

    GraphMetadata metadata;
    metadata.label = "Sum two numbers";
    metadata.description = "the smallest graph that does anything";
    metadata.nodeLayout["a"] = { { "x", "10" }, { "y", "20" } };

    const SerializeResult written = SerializeGraph (graph, metadata);
    ASSERT_TRUE (written.ok) << written.error;

    const DeserializeResult read = DeserializeGraph (written.text, registry);
    ASSERT_TRUE (read.ok) << read.error;

    EXPECT_EQ (graph.Nodes ().size (), read.graph.document.Nodes ().size ());
    EXPECT_EQ (graph.Edges ().size (), read.graph.document.Edges ().size ());
    EXPECT_EQ ("Sum two numbers", read.graph.metadata.label);
    EXPECT_EQ ("20", read.graph.metadata.nodeLayout.at ("a").at ("y"));
    EXPECT_EQ (7, Integer (read.graph.document.FindNode ("a")->parameters.at ("value")));
    EXPECT_EQ (read.graph.document.FindNode ("sum")->inputModifiers.at ("left"), PortModifier::Graft);
    // A port nobody modified carries no entry at all, so a graph that uses no
    // modifiers reads exactly as it did before they existed.
    EXPECT_FALSE (read.graph.document.FindNode ("sum")->inputModifiers.contains ("right"));

    // Writing what was read produces the same text: the format has no member
    // whose meaning depends on having been written by this process.
    const SerializeResult rewritten = SerializeGraph (read.graph.document, read.graph.metadata);
    ASSERT_TRUE (rewritten.ok) << rewritten.error;
    EXPECT_EQ (written.text, rewritten.text);

    // The reloaded graph still evaluates to the same answer, which is the only
    // property a user actually cares about.
    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, read.graph.document, registry, ExecuteNumberAddNode);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (12, Integer (Out (evaluator.Result ("sum"), "sum")));
}

TEST (NodeGraphPersistence, RoundTripsEveryPersistableValueKind)
{
    const NodeRegistry registry = MakeStorageRegistry ();
    GraphDocument graph;
    Node node { "p", "parameters" };
    node.parameters.emplace ("flag", Value (true));
    node.parameters.emplace ("count", Value (int64_t { -9 }));
    node.parameters.emplace ("ratio", Value (2.5));
    node.parameters.emplace ("label", Value (std::string ("wall thickness")));
    node.parameters.emplace ("origin", Value (Point3 { 1.0, 2.0, 3.0 }));
    node.parameters.emplace ("path", Value (Polyline { { Point3 { 0, 0, 0 }, Point3 { 1, 1, 1 } } }));
    node.parameters.emplace ("outline", Value (Polygon { { Point3 { 0, 0, 0 }, Point3 { 1, 0, 0 } } }));
    node.parameters.emplace ("element", Value (ArchicadElementRef { "guid-1" }));
    node.parameters.emplace ("items", Argument::FromItems ({ Value (int64_t { 1 }), Value (std::string ("two")) }));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { node } }).accepted);

    const SerializeResult written = SerializeGraph (graph, GraphMetadata {});
    ASSERT_TRUE (written.ok) << written.error;
    const DeserializeResult read = DeserializeGraph (written.text, registry);
    ASSERT_TRUE (read.ok) << read.error;

    const Node& reloaded = *read.graph.document.FindNode ("p");
    EXPECT_EQ (ValueType::Bool, reloaded.parameters.at ("flag").Type ());
    EXPECT_EQ (-9, Integer (reloaded.parameters.at ("count")));
    EXPECT_DOUBLE_EQ (2.5, std::get<double> (reloaded.parameters.at ("ratio").DataValue ()));
    EXPECT_EQ ("wall thickness", std::get<std::string> (reloaded.parameters.at ("label").DataValue ()));
    EXPECT_DOUBLE_EQ (3.0, std::get<Point3> (reloaded.parameters.at ("origin").DataValue ()).z);
    EXPECT_EQ (2U, std::get<Polyline> (reloaded.parameters.at ("path").DataValue ()).points.size ());
    EXPECT_EQ ("guid-1", std::get<ArchicadElementRef> (reloaded.parameters.at ("element").DataValue ()).guid);
    EXPECT_EQ (2U, reloaded.parameters.at ("items").Items ().size ());

    // Integer and Double stay distinct across the file. A format that rounded
    // one into the other would change what the node computes.
    EXPECT_EQ (ValueType::Integer, reloaded.parameters.at ("count").Type ());
    EXPECT_EQ (ValueType::Double, reloaded.parameters.at ("ratio").Type ());
}

TEST (NodeGraphPersistence, RefusesToStoreAMeshAsAParameter)
{
    NodeRegistry registry;
    std::string error;
    NodeType holder;
    holder.id = "holder";
    holder.label = "Holder";
    holder.outputs.push_back ({ "out", "Out", ValueType::Integer });
    holder.parameters.push_back ({ "mesh", "Mesh", ValueType::Mesh });
    ASSERT_TRUE (registry.Register (std::move (holder), error)) << error;

    GraphDocument graph;
    Node node { "m", "holder" };
    node.parameters.emplace ("mesh", Value (std::make_shared<const geomsrv::Mesh> ()));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { node } }).accepted);

    // A mesh is a RESULT. Storing one inside the program that computes it would
    // be a cache wearing a parameter's clothes.
    const SerializeResult written = SerializeGraph (graph, GraphMetadata {});
    EXPECT_FALSE (written.ok);
    EXPECT_NE (std::string::npos, written.error.find ("mesh"));
    EXPECT_NE (std::string::npos, written.error.find ("m"));
}

TEST (NodeGraphPersistence, RejectsAFileThatWouldProduceAnInvalidGraph)
{
    const NodeRegistry registry = MakeRegistry ();

    const auto load = [&registry] (const std::string& text) { return DeserializeGraph (text, registry); };

    EXPECT_NE (std::string::npos, load ("{}").error.find ("not a Tapioca node graph"));

    EXPECT_NE (std::string::npos,
               load (R"({"format":"tapioca-nodegraph","formatVersion":99,"nodes":[]})").error.find ("newer"));

    // An unknown node type is caught by the same edit validation an interactive
    // add goes through, so the message names the node.
    const DeserializeResult unknown = load (
        R"({"format":"tapioca-nodegraph","formatVersion":1,"nodes":[{"id":"x","nodeType":"nosuch"}],"edges":[]})");
    EXPECT_FALSE (unknown.ok);
    EXPECT_NE (std::string::npos, unknown.error.find ("x"));

    // A dangling edge, likewise: never silently dropped.
    const DeserializeResult dangling = load (
        R"({"format":"tapioca-nodegraph","formatVersion":1,"nodes":[{"id":"a","nodeType":"number","parameters":{"value":{"valueType":"integer","value":1}}}],)"
        R"("edges":[{"sourceNode":"a","sourcePort":"value","targetNode":"ghost","targetPort":"left"}]})");
    EXPECT_FALSE (dangling.ok);
    EXPECT_NE (std::string::npos, dangling.error.find ("ghost"));

    // And a cycle, which the document must never be able to hold however it was
    // constructed.
    const DeserializeResult cyclic =
        load (R"({"format":"tapioca-nodegraph","formatVersion":1,"nodes":[{"id":"s","nodeType":"add"}],)"
              R"("edges":[{"sourceNode":"s","sourcePort":"sum","targetNode":"s","targetPort":"left"}]})");
    EXPECT_FALSE (cyclic.ok);
    EXPECT_NE (std::string::npos, cyclic.error.find ("cycle"));
}

TEST (NodeGraphStore, SavesLoadsListsAndDeletes)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 3).accepted);

    GraphMetadata metadata;
    metadata.label = "One number";

    MemoryGraphStore store;
    EXPECT_FALSE (store.Exists ("daylight"));
    EXPECT_EQ (StoreStatus::NotFound, store.Delete ("daylight").status);

    SerializedGraph loaded;
    EXPECT_EQ (StoreStatus::NotFound, store.Load ("daylight", registry, loaded).status);

    ASSERT_TRUE (store.Save ("daylight", graph, metadata).Ok ());
    EXPECT_TRUE (store.Exists ("daylight"));

    ASSERT_TRUE (store.Load ("daylight", registry, loaded).Ok ());
    EXPECT_EQ (1U, loaded.document.Nodes ().size ());
    EXPECT_EQ (3, Integer (loaded.document.FindNode ("a")->parameters.at ("value")));
    EXPECT_EQ ("One number", loaded.metadata.label);

    const std::vector<StoredGraphInfo> listing = store.List ();
    ASSERT_EQ (1U, listing.size ());
    EXPECT_EQ ("daylight", listing.front ().graphId);
    EXPECT_EQ ("One number", listing.front ().label);
    // A listing must not have to load every graph to say how big it is.
    EXPECT_EQ (1U, listing.front ().nodeCount);

    ASSERT_TRUE (store.Delete ("daylight").Ok ());
    EXPECT_FALSE (store.Exists ("daylight"));
    EXPECT_TRUE (store.List ().empty ());
}

TEST (NodeGraphStore, SavingTwiceReplacesRatherThanAccumulates)
{
    const NodeRegistry registry = MakeRegistry ();
    MemoryGraphStore store;

    GraphDocument first;
    ASSERT_TRUE (AddNode (first, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (store.Save ("w", first, GraphMetadata {}).Ok ());

    GraphDocument second;
    ASSERT_TRUE (AddNode (second, registry, "a", "number", 2).accepted);
    ASSERT_TRUE (AddNode (second, registry, "b", "number", 3).accepted);
    ASSERT_TRUE (store.Save ("w", second, GraphMetadata {}).Ok ());

    SerializedGraph loaded;
    ASSERT_TRUE (store.Load ("w", registry, loaded).Ok ());
    EXPECT_EQ (2U, loaded.document.Nodes ().size ());
    EXPECT_EQ (1U, store.List ().size ());
}

TEST (NodeGraphStore, RefusesAGraphNameNoBackendCouldHold)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    MemoryGraphStore store;
    // The rule is a file-backed backend's rule applied here too, so a name that
    // saves to memory today cannot fail to save to the library tomorrow.
    for (const char* bad : { "", "..", "../escape", "with/slash", "back\\slash", ".hidden", "a b" })
        EXPECT_EQ (StoreStatus::InvalidId, store.Save (bad, graph, GraphMetadata {}).status) << "accepted: " << bad;

    for (const char* good : { "daylight", "Massing_v2", "site.analysis", "a-b-c" })
        EXPECT_TRUE (store.Save (good, graph, GraphMetadata {}).Ok ()) << "refused: " << good;
}

TEST (NodeGraphStore, AStoredGraphNamingAnUnknownNodeTypeFailsAtLoadNotAtRun)
{
    NodeRegistry writer = MakeRegistry ();
    std::string error;
    NodeType extra;
    extra.id = "extra";
    extra.label = "Extra";
    extra.outputs.push_back ({ "value", "Value", ValueType::Integer });
    ASSERT_TRUE (writer.Register (std::move (extra), error)) << error;

    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, writer, GraphEdit { AddNodeEdit { Node { "e", "extra" } } }).accepted);

    MemoryGraphStore store;
    ASSERT_TRUE (store.Save ("w", graph, GraphMetadata {}).Ok ());

    // A build without that node type refuses the load and names the node, rather
    // than loading a graph that fails at its first evaluation.
    const NodeRegistry reader = MakeRegistry ();
    SerializedGraph loaded;
    const StoreResult result = store.Load ("w", reader, loaded);
    EXPECT_EQ (StoreStatus::Invalid, result.status);
    EXPECT_NE (std::string::npos, result.error.find ("e"));
}

namespace {

// A directory of its own per test, removed afterwards, so a failing test cannot
// leave state that makes the next one pass or fail for the wrong reason.
class TemporaryLibrary {
  public:
    TemporaryLibrary ()
    {
        static std::atomic<int> counter { 0 };
        path_ =
            std::filesystem::temp_directory_path () /
            ("tapioca-graphstore-" + std::to_string (counter.fetch_add (1)) + "-" +
             std::to_string (static_cast<long long> (std::chrono::steady_clock::now ().time_since_epoch ().count ())));
    }

    ~TemporaryLibrary ()
    {
        std::error_code code;
        std::filesystem::remove_all (path_, code);
    }

    std::string Root () const
    {
        return path_.string ();
    }

    const std::filesystem::path& Path () const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST (NodeGraphFileStore, SurvivesTheProcessThatWroteIt)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 4).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 6).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "sum", "add").accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("a", "value", "sum", "left") } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("b", "value", "sum", "right") } }).accepted);

    GraphMetadata metadata;
    metadata.label = "Daylight";

    const TemporaryLibrary library;
    {
        FileGraphStore store (library.Root ());
        // The library directory does not exist yet: the first save creates it.
        ASSERT_TRUE (store.Save ("daylight", graph, metadata).Ok ());
    }

    // A DIFFERENT store object over the same directory, which is what "restart
    // Archicad and load it again" reduces to.
    FileGraphStore reopened (library.Root ());
    EXPECT_TRUE (reopened.Exists ("daylight"));

    SerializedGraph loaded;
    ASSERT_TRUE (reopened.Load ("daylight", registry, loaded).Ok ());
    EXPECT_EQ (3U, loaded.document.Nodes ().size ());
    EXPECT_EQ (2U, loaded.document.Edges ().size ());
    EXPECT_EQ ("Daylight", loaded.metadata.label);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, loaded.document, registry, ExecuteNumberAddNode);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (10, Integer (Out (evaluator.Result ("sum"), "sum")));
}

TEST (NodeGraphFileStore, ListsSortedAndWithoutLoading)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);
    ASSERT_TRUE (AddNode (graph, registry, "b", "number", 2).accepted);

    const TemporaryLibrary library;
    FileGraphStore store (library.Root ());

    // An empty library lists nothing rather than failing: a user who has never
    // saved a workflow should not be shown an error.
    EXPECT_TRUE (store.List ().empty ());

    GraphMetadata metadata;
    metadata.label = "Zed";
    ASSERT_TRUE (store.Save ("zed", graph, metadata).Ok ());
    metadata.label = "Alpha";
    ASSERT_TRUE (store.Save ("alpha", graph, metadata).Ok ());

    // Anything that is not one of ours is ignored rather than listed.
    {
        std::ofstream stray (library.Path () / "notes.txt");
        stray << "hello";
    }

    const std::vector<StoredGraphInfo> listing = store.List ();
    ASSERT_EQ (2U, listing.size ());
    EXPECT_EQ ("alpha", listing[0].graphId);
    EXPECT_EQ ("Alpha", listing[0].label);
    EXPECT_EQ (2U, listing[0].nodeCount);
    EXPECT_EQ ("zed", listing[1].graphId);
}

TEST (NodeGraphFileStore, ACorruptFileIsListedButRefusedOnLoad)
{
    const NodeRegistry registry = MakeRegistry ();
    const TemporaryLibrary library;
    std::error_code code;
    std::filesystem::create_directories (library.Path (), code);
    {
        std::ofstream broken (library.Path () / "broken.tapiocagraph.json");
        broken << "{ this is not json";
    }

    FileGraphStore store (library.Root ());

    // Listed by name, so the user can see it and repair or delete it. A library
    // that hides a graph is worse than one that shows a graph needing repair.
    const std::vector<StoredGraphInfo> listing = store.List ();
    ASSERT_EQ (1U, listing.size ());
    EXPECT_EQ ("broken", listing.front ().graphId);
    EXPECT_EQ (0U, listing.front ().nodeCount);

    SerializedGraph loaded;
    const StoreResult result = store.Load ("broken", registry, loaded);
    EXPECT_EQ (StoreStatus::Invalid, result.status);
    EXPECT_NE (std::string::npos, result.error.find ("JSON"));
}

TEST (NodeGraphFileStore, AFailedSaveLeavesThePreviousGraphIntact)
{
    NodeRegistry registry = MakeRegistry ();
    std::string error;
    NodeType holder;
    holder.id = "holder";
    holder.label = "Holder";
    holder.outputs.push_back ({ "out", "Out", ValueType::Integer });
    holder.parameters.push_back ({ "mesh", "Mesh", ValueType::Mesh });
    ASSERT_TRUE (registry.Register (std::move (holder), error)) << error;

    GraphDocument good;
    ASSERT_TRUE (AddNode (good, registry, "a", "number", 8).accepted);

    const TemporaryLibrary library;
    FileGraphStore store (library.Root ());
    ASSERT_TRUE (store.Save ("w", good, GraphMetadata {}).Ok ());

    // A graph that cannot be serialised at all - the save fails before any file
    // is touched.
    GraphDocument unserialisable;
    Node node { "m", "holder" };
    node.parameters.emplace ("mesh", Value (std::make_shared<const geomsrv::Mesh> ()));
    ASSERT_TRUE (ApplyEdit (unserialisable, registry, GraphEdit { AddNodeEdit { node } }).accepted);
    EXPECT_EQ (StoreStatus::Invalid, store.Save ("w", unserialisable, GraphMetadata {}).status);

    // The workflow that was already there is untouched, which is the whole
    // reason a save is write-then-rename.
    SerializedGraph loaded;
    ASSERT_TRUE (store.Load ("w", registry, loaded).Ok ());
    EXPECT_EQ (8, Integer (loaded.document.FindNode ("a")->parameters.at ("value")));

    // And no partial file was left lying about.
    EXPECT_FALSE (std::filesystem::exists (library.Path () / "w.tapiocagraph.json.partial"));
}

TEST (NodeGraphFileStore, DeletesAndReportsAMissingGraph)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    const TemporaryLibrary library;
    FileGraphStore store (library.Root ());
    EXPECT_EQ (StoreStatus::NotFound, store.Delete ("missing").status);

    ASSERT_TRUE (store.Save ("w", graph, GraphMetadata {}).Ok ());
    ASSERT_TRUE (store.Delete ("w").Ok ());
    EXPECT_FALSE (store.Exists ("w"));
    EXPECT_EQ (StoreStatus::NotFound, store.Delete ("w").status);
}

TEST (NodeGraphFileStore, RefusesAnUnusableNameBeforeTouchingTheFilesystem)
{
    const NodeRegistry registry = MakeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (AddNode (graph, registry, "a", "number", 1).accepted);

    const TemporaryLibrary library;
    FileGraphStore store (library.Root ());
    // The path-traversal shapes in particular: a name is never allowed to reach
    // outside the library.
    for (const char* bad : { "../escape", "..", "with/slash", "back\\slash", "" })
        EXPECT_EQ (StoreStatus::InvalidId, store.Save (bad, graph, GraphMetadata {}).status) << "accepted: " << bad;
    EXPECT_FALSE (std::filesystem::exists (library.Path ()));
}

TEST (NodeGraphWorkerPool, StoppingJoinsItsThreadsAndLeavesBatchesRunnable)
{
    WorkerPool pool (3);
    EXPECT_EQ (3U, pool.ThreadCount ());

    std::atomic<int> before { 0 };
    pool.RunBatch (6, 0, [&] (size_t) { before.fetch_add (1); });
    EXPECT_EQ (6, before.load ());

    // The add-on stops the pool on Archicad's quit and unload paths rather than
    // letting static destruction join threads under the loader lock.
    pool.Stop ();
    EXPECT_EQ (0U, pool.ThreadCount ());
    pool.Stop (); // idempotent

    // A batch submitted after the stop still runs - inline, on the submitter -
    // so an evaluation finishing during teardown completes instead of failing.
    const std::thread::id submitter = std::this_thread::get_id ();
    std::atomic<int> after { 0 };
    pool.RunBatch (4, 0, [&] (size_t) {
        EXPECT_EQ (submitter, std::this_thread::get_id ());
        after.fetch_add (1);
    });
    EXPECT_EQ (4, after.load ());
}

// ---------------------------------------------------------------------------
// The selection set's five actions.
//
// Update, Add, Remove, Reselect, Clear - the command palette's vocabulary, so a
// user who has captured a selection for a command does not learn a second set of
// words for a graph. These run through GraphRuntimeState because that is where
// the action lives: an action is a deliberate user act, not an evaluation, and
// it evaluates what it affects on the way out.
// ---------------------------------------------------------------------------

namespace {

using SelectionAction = GraphRuntimeState::SelectionAction;

// A graph id of its own per test: the runtime is a process singleton, so two
// tests sharing a name would share a document.
GraphId FreshGraphId (const char* label)
{
    static std::atomic<int> counter { 0 };
    return std::string ("test-") + label + "-" + std::to_string (counter.fetch_add (1));
}

// Installs `host` for the duration of the scope. An action reads the ACTIVE
// host rather than one passed in, because a button press has no run context to
// carry one.
class ScopedHost {
  public:
    explicit ScopedHost (IArchicadHost* host)
    {
        SetActiveArchicadHost (host);
    }
    ~ScopedHost ()
    {
        SetActiveArchicadHost (nullptr);
    }
    ScopedHost (const ScopedHost&) = delete;
    ScopedHost& operator= (const ScopedHost&) = delete;
};

std::vector<std::string> GuidsOf (const GraphDocument& document, const NodeId& nodeId)
{
    std::vector<std::string> guids;
    const Node* node = document.FindNode (nodeId);
    if (node == nullptr)
        return guids;
    const auto parameter = node->parameters.find ("elements");
    if (parameter == node->parameters.end ())
        return guids;
    for (const ArchicadElementRef& element : ElementsFromValue (parameter->second))
        guids.push_back (element.guid);
    return guids;
}

} // namespace

TEST (NodeGraphSelectionSet, UpdateReplacesAddUnionsAndRemoveSubtracts)
{
    const GraphId graphId = FreshGraphId ("mutations");
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

    StubHost host;
    const ScopedHost installed (&host);

    host.Holds ({ "a", "b" });
    auto result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (2U, result.count);
    EXPECT_EQ ((std::vector<std::string> { "a", "b" }), GuidsOf (runtime.Document (graphId), "sel"));

    // Add UNIONS rather than appending: pressing Add twice on the same selection
    // must not leave an element in the set twice.
    host.Holds ({ "b", "c" });
    result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Add);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (3U, result.count);
    EXPECT_EQ (1U, result.changed);
    EXPECT_EQ ((std::vector<std::string> { "a", "b", "c" }), GuidsOf (runtime.Document (graphId), "sel"));

    result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Add);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (3U, result.count);
    EXPECT_EQ (0U, result.changed);

    host.Holds ({ "a" });
    result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Remove);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ ((std::vector<std::string> { "b", "c" }), GuidsOf (runtime.Document (graphId), "sel"));

    // Update replaces outright, including down to nothing.
    host.Holds ({});
    result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ (0U, result.count);
}

namespace {

std::vector<std::string> CapturedTypesOf (const GraphDocument& document, const NodeId& nodeId)
{
    const Node* node = document.FindNode (nodeId);
    if (node == nullptr)
        return {};
    const auto parameter = node->parameters.find (kSelectionTypesParameter);
    if (parameter == node->parameters.end ())
        return {};
    return TypesFromValue (parameter->second);
}

} // namespace

TEST (NodeGraphSelectionSet, TheTypeOfEachElementIsCapturedWithItAndStaysParallel)
{
    // ⚠️ WHY THE CAPTURE EXISTS AT ALL. The per-type containers need every
    // element's type; asking Archicad for it at evaluation time would make the
    // selection node depend on the model and go dirty whenever the user clicked
    // in it - which is the exact behaviour the captured set was built to avoid.
    // So the type is read on the button press that had to cross to the host
    // anyway, and stored beside the guid.
    const GraphId graphId = FreshGraphId ("captured-types");
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

    StubHost host;
    const ScopedHost installed (&host);
    host.HoldsTyped ({ { "a", "wall" }, { "b", "slab" }, { "c", "wall" } });

    ASSERT_TRUE (runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update).ok);
    EXPECT_EQ ((std::vector<std::string> { "wall", "slab", "wall" }),
               CapturedTypesOf (runtime.Document (graphId), "sel"));
    EXPECT_EQ ((std::vector<std::string> { "a", "b", "c" }), GuidsOf (runtime.Document (graphId), "sel"));

    // The two lists move TOGETHER. Leaving the old types beside a new guid list
    // would file every element under its predecessor's container - a stack that
    // looks plausible and is entirely wrong.
    host.HoldsTyped ({ { "d", "column" } });
    ASSERT_TRUE (runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update).ok);
    EXPECT_EQ ((std::vector<std::string> { "column" }), CapturedTypesOf (runtime.Document (graphId), "sel"));
    EXPECT_EQ (1U, GuidsOf (runtime.Document (graphId), "sel").size ());

    // And Clear empties both rather than leaving a stale type list behind.
    ASSERT_TRUE (runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Clear).ok);
    EXPECT_TRUE (CapturedTypesOf (runtime.Document (graphId), "sel").empty ());
}

TEST (NodeGraphSelectionSet, AFailedTypeReadStillCapturesTheSelection)
{
    // The set changed correctly; only the GROUPING is unknown. Refusing the
    // capture over a grouping would lose the user's actual selection to fix a
    // panel, which is the wrong trade - so every element falls into the
    // unclassified container and the next Update repairs it.
    const GraphId graphId = FreshGraphId ("describe-fails");
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

    StubHost host;
    const ScopedHost installed (&host);
    host.HoldsTyped ({ { "a", "wall" } });
    host.describeFails = true;

    const auto result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update);
    ASSERT_TRUE (result.ok) << result.error;
    EXPECT_EQ ((std::vector<std::string> { "a" }), GuidsOf (runtime.Document (graphId), "sel"));
    EXPECT_EQ ((std::vector<std::string> { kUnclassifiedElementTypeId }),
               CapturedTypesOf (runtime.Document (graphId), "sel"));
}

TEST (NodeGraphSelectionSet, AnActionEvaluatesWhatItAffectsSoNobodyPressesEvaluate)
{
    const GraphId graphId = FreshGraphId ("evaluates");
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);
    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { ConnectEdit { Connect ("sel", "elements", "panel", "value") } }).accepted);

    StubHost host;
    const ScopedHost installed (&host);
    host.Holds ({ "a", "b", "c" });

    const auto result = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update);
    ASSERT_TRUE (result.ok) << result.error;
    ASSERT_TRUE (result.evaluation.has_value ());
    EXPECT_TRUE (result.evaluation->succeeded) << result.evaluation->error;

    // The CONSUMER already carries the new answer. Without this the set would
    // change and the panel would keep showing the previous one until somebody
    // pressed Evaluate, which is exactly the complaint this design answers.
    const ResultsSnapshot snapshot = runtime.Results (graphId);
    const auto panel = std::find_if (snapshot.nodes.begin (), snapshot.nodes.end (),
                                     [] (const RuntimeNodeResult& node) { return node.nodeId == "panel"; });
    ASSERT_NE (snapshot.nodes.end (), panel);
    EXPECT_EQ (NodeExecutionState::Success, panel->status.state);
    ASSERT_NE (nullptr, panel->result);
}

TEST (NodeGraphSelectionSet, ClearNeedsNoProjectAndReselectRefusesAStaleSet)
{
    const GraphId graphId = FreshGraphId ("reselect");
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (
        runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

    StubHost host;
    {
        const ScopedHost installed (&host);
        host.Holds ({ "a", "b" });
        ASSERT_TRUE (runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Update).ok);

        ASSERT_TRUE (runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Reselect).ok);
        EXPECT_EQ (1, host.setSelectionCalls);
        EXPECT_EQ (2U, host.applied.size ());

        // One element deleted. Selecting the survivor would look like a correct
        // answer and would not be one - the same rule Set Selection follows.
        host.resolver.present.erase ("b");
        const int callsBefore = host.setSelectionCalls;
        const auto stale = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Reselect);
        EXPECT_FALSE (stale.ok);
        EXPECT_EQ (callsBefore, host.setSelectionCalls);
        ASSERT_EQ (1U, stale.missing.size ());
        EXPECT_EQ ("b", stale.missing.front ());
    }

    // Clear is the one action that needs no project: a set you can no longer
    // resolve is exactly the one you want to be able to empty.
    const auto cleared = runtime.ApplySelectionAction (graphId, "sel", SelectionAction::Clear);
    ASSERT_TRUE (cleared.ok) << cleared.error;
    EXPECT_EQ (0U, cleared.count);
    EXPECT_TRUE (GuidsOf (runtime.Document (graphId), "sel").empty ());
}

TEST (NodeGraphSelectionSet, RefusesANodeThatIsNotASelectionSet)
{
    const GraphId graphId = FreshGraphId ("wrongnode");
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    ASSERT_TRUE (runtime.Apply (graphId, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);

    StubHost host;
    const ScopedHost installed (&host);

    auto result = runtime.ApplySelectionAction (graphId, "panel", SelectionAction::Update);
    EXPECT_FALSE (result.ok);
    EXPECT_NE (std::string::npos, result.error.find ("not a selection set"));

    result = runtime.ApplySelectionAction (graphId, "ghost", SelectionAction::Update);
    EXPECT_FALSE (result.ok);
    EXPECT_NE (std::string::npos, result.error.find ("no node called"));
}

TEST (NodeGraphSelectionSet, TheSetSurvivesASaveAndLoadBecauseItIsAnOrdinaryParameter)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { SelectionSetNode ("sel", { "guid-a", "guid-b" }) } })
            .accepted);

    MemoryGraphStore store;
    ASSERT_TRUE (store.Save ("captured", graph, GraphMetadata {}).Ok ());

    SerializedGraph loaded;
    ASSERT_TRUE (store.Load ("captured", registry, loaded).Ok ());
    // No extra machinery: the set persists because it IS a parameter, which is
    // the simplification that made this design worth choosing over a side store.
    const std::vector<ArchicadElementRef> elements =
        ElementsFromValue (loaded.document.FindNode ("sel")->parameters.at ("elements"));
    ASSERT_EQ (2U, elements.size ());
    EXPECT_EQ ("guid-b", elements[1].guid);
}

// ===========================================================================
// Stage F - flow control.
//
// Every test below has the same shape, because it is the only shape that proves
// anything: build a graph whose DOWNSTREAM node reports what the mode did.
// Asserting the modal node's own status would pass just as well against an
// implementation in which the mode is a label and nothing else.
// ===========================================================================

namespace {

struct FlowFixture {
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    Evaluator evaluator;

    bool Add (const char* id, const char* nodeType, double value = 0.0)
    {
        Node node { id, nodeType };
        if (node.nodeType == "number")
            node.parameters.emplace ("value", Value (value));
        return ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { std::move (node) } }).accepted;
    }

    bool Wire (const Edge& edge)
    {
        return ApplyEdit (graph, registry, GraphEdit { ConnectEdit { edge } }).accepted;
    }

    EditResult Mode (const char* nodeId, ExecutionMode mode)
    {
        return ApplyEdit (graph, registry, GraphEdit { SetExecutionModeEdit { nodeId, mode } });
    }

    EvaluationOutcome Run (RunId runId = 1)
    {
        RunContext context;
        context.runId = runId;
        return evaluator.Evaluate (graph, registry, ExecuteBuiltinNode, EvaluationRequest {}, context);
    }
};

// left(2) + right(40) -> sum. `sum` is the node whose mode each test changes,
// and the two operands differ so that forwarding the wrong one is visible.
//
// Filled through a reference rather than returned: FlowFixture owns an Evaluator,
// whose in-flight flag is a std::atomic and therefore neither copyable nor
// movable. That is a property worth keeping - an evaluator being copied mid-run
// is not a thing that should compile.
void BuildArithmeticChain (FlowFixture& fixture)
{
    EXPECT_TRUE (fixture.Add ("left", "number", 2.0));
    EXPECT_TRUE (fixture.Add ("right", "number", 40.0));
    EXPECT_TRUE (fixture.Add ("sum", "add"));
    EXPECT_TRUE (fixture.Wire (Connect ("left", "value", "sum", "left")));
    EXPECT_TRUE (fixture.Wire (Connect ("right", "value", "sum", "right")));
}

double OutputNumber (const Evaluator& evaluator, const NodeId& nodeId, const PortId& portId)
{
    const std::shared_ptr<const NodeResult> result = evaluator.Result (nodeId);
    EXPECT_NE (nullptr, result) << nodeId << " published nothing";
    if (result == nullptr)
        return 0.0;
    EXPECT_TRUE (result->outputs.contains (portId));
    return std::get<double> (Out (result, portId.c_str ()).DataValue ());
}

void BuildDammedChain (FlowFixture& fixture)
{
    EXPECT_TRUE (fixture.Add ("one", "number", 1.0));
    EXPECT_TRUE (fixture.Add ("collect", "makeList"));
    EXPECT_TRUE (fixture.Add ("dam", "dataDam"));
    EXPECT_TRUE (fixture.Add ("tail", "watch"));
    EXPECT_TRUE (fixture.Wire (Connect ("one", "value", "collect", "items")));
    EXPECT_TRUE (fixture.Wire (Connect ("collect", "value", "dam", "value")));
    EXPECT_TRUE (fixture.Wire (Connect ("dam", "value", "tail", "value")));
}

} // namespace

TEST (NodeGraphFlowControl, DisabledPublishesNothingAndDoesNotFailTheRun)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    ASSERT_TRUE (fixture.Mode ("sum", ExecutionMode::Disabled).accepted);
    const EvaluationOutcome outcome = fixture.Run ();

    // The run SUCCEEDED. Disabling is an instruction, not a fault, and a graph
    // with a switched-off branch must not report itself as broken - that is what
    // would make "did anything fail" useless as a signal.
    EXPECT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (0U, outcome.failedCount);

    EXPECT_EQ (NodeExecutionState::Disabled, fixture.evaluator.Status ("sum").state);
    EXPECT_EQ (statuscode::kDisabled, fixture.evaluator.Status ("sum").code);
    EXPECT_EQ (nullptr, fixture.evaluator.Result ("sum")) << "a disabled node must publish no output";

    // Upstream is untouched: only what DEPENDS on the disabled node is affected.
    EXPECT_EQ (NodeExecutionState::Success, fixture.evaluator.Status ("left").state);
}

TEST (NodeGraphFlowControl, DisabledDoesNotRequireItsInputsToBeWired)
{
    FlowFixture fixture;
    ASSERT_TRUE (fixture.Add ("sum", "add"));
    // `add` has two REQUIRED inputs and neither is connected. Enabled, that is a
    // hard failure; disabled it is none of the run's business, and the
    // difference is between switching a node off and being told off for it.
    ASSERT_TRUE (fixture.Mode ("sum", ExecutionMode::Disabled).accepted);

    const EvaluationOutcome outcome = fixture.Run ();
    EXPECT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (NodeExecutionState::Disabled, fixture.evaluator.Status ("sum").state);
}

TEST (NodeGraphFlowControl, DownstreamOfDisabledIsBlockedRatherThanFailed)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    ASSERT_TRUE (fixture.Add ("collect", "makeList"));
    ASSERT_TRUE (fixture.Wire (Connect ("sum", "value", "collect", "items")));
    ASSERT_TRUE (fixture.Mode ("sum", ExecutionMode::Disabled).accepted);

    fixture.Run ();
    const NodeStatus downstream = fixture.evaluator.Status ("collect");
    EXPECT_EQ (NodeExecutionState::Blocked, downstream.state);
    EXPECT_EQ (statuscode::kBlockedUpstreamDisabled, downstream.code);
}

TEST (NodeGraphFlowControl, BypassForwardsTheDeclaredInputWithoutRunningTheBody)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    ASSERT_TRUE (fixture.Mode ("sum", ExecutionMode::Bypassed).accepted);

    const EvaluationOutcome outcome = fixture.Run ();
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    // 2, not 42. `add` declares left -> value, so bypassing forwards the LEFT
    // operand; had the body run this would be 42. That is the assertion which
    // distinguishes a bypass from a decorative label.
    EXPECT_DOUBLE_EQ (2.0, OutputNumber (fixture.evaluator, "sum", "value"));
    EXPECT_EQ (NodeExecutionState::Bypassed, fixture.evaluator.Status ("sum").state);
    EXPECT_EQ (statuscode::kBypassed, fixture.evaluator.Status ("sum").code);

    // Nothing executed, so the run's work counters must not claim otherwise.
    EXPECT_EQ (2U, outcome.executedCount) << "only the two number nodes ran";
}

TEST (NodeGraphFlowControl, BypassKeepsDownstreamRunnable)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    ASSERT_TRUE (fixture.Add ("collect", "makeList"));
    ASSERT_TRUE (fixture.Wire (Connect ("sum", "value", "collect", "items")));
    ASSERT_TRUE (fixture.Mode ("sum", ExecutionMode::Bypassed).accepted);

    fixture.Run ();
    // The whole difference between Bypassed and Disabled: consumers still run.
    EXPECT_EQ (NodeExecutionState::Success, fixture.evaluator.Status ("collect").state);
}

TEST (NodeGraphFlowControl, BypassIsRefusedForATypeThatDeclaresNoMapping)
{
    FlowFixture fixture;
    ASSERT_TRUE (fixture.Add ("readout", "panel"));

    // `panel` declares no mapping, and a type that cannot say what bypass means
    // simply cannot be bypassed. The refusal IS the feature: guessing would make
    // bypass mean something different on every node it touched.
    const EditResult rejected = fixture.Mode ("readout", ExecutionMode::Bypassed);
    EXPECT_FALSE (rejected.accepted);
    EXPECT_EQ ("mode.bypassUnsupported", rejected.code);
    // Atomic: a refused edit leaves the document exactly as it was.
    EXPECT_EQ (ExecutionMode::Enabled, fixture.graph.FindNode ("readout")->executionMode);
}

TEST (NodeGraphRegistry, RefusesAnAmbiguousOrIllTypedBypassTable)
{
    NodeRegistry registry;
    std::string error;

    NodeType twoOntoOne;
    twoOntoOne.id = "twoOntoOne";
    twoOntoOne.inputs.push_back ({ "a", "A", ValueType::Double });
    twoOntoOne.inputs.push_back ({ "b", "B", ValueType::Double });
    twoOntoOne.outputs.push_back ({ "out", "Out", ValueType::Double });
    twoOntoOne.bypassMappings = { { "a", "out" }, { "b", "out" } };
    EXPECT_FALSE (registry.Register (std::move (twoOntoOne), error));
    EXPECT_NE (std::string::npos, error.find ("ambiguous")) << error;

    NodeType mistyped;
    mistyped.id = "mistyped";
    mistyped.inputs.push_back ({ "a", "A", ValueType::Double });
    mistyped.outputs.push_back ({ "out", "Out", ValueType::String });
    mistyped.bypassMappings = { { "a", "out" } };
    EXPECT_FALSE (registry.Register (std::move (mistyped), error));
    EXPECT_NE (std::string::npos, error.find ("type-compatible")) << error;

    NodeType partial;
    partial.id = "partial";
    partial.inputs.push_back ({ "a", "A", ValueType::Double });
    partial.outputs.push_back ({ "out", "Out", ValueType::Double });
    partial.outputs.push_back ({ "other", "Other", ValueType::Double });
    partial.bypassMappings = { { "a", "out" } };
    // `other` is required and unfed, which reads to a consumer as a broken node
    // rather than a bypassed one.
    EXPECT_FALSE (registry.Register (std::move (partial), error));
    EXPECT_NE (std::string::npos, error.find ("unfed")) << error;
}

TEST (NodeGraphFlowControl, HoldingStagesInsteadOfPublishingAndBlocksUntilReleased)
{
    FlowFixture fixture;
    BuildDammedChain (fixture);
    ASSERT_TRUE (fixture.Mode ("dam", ExecutionMode::Holding).accepted);
    fixture.Run (1);

    // Before the first release the dam HAS run, so something is staged - but it
    // published nothing, so its consumer has no value to work with.
    EXPECT_EQ (NodeExecutionState::Holding, fixture.evaluator.Status ("dam").state);
    EXPECT_EQ (statuscode::kHoldingStaged, fixture.evaluator.Status ("dam").code);
    EXPECT_EQ (nullptr, fixture.evaluator.Result ("dam"));
    EXPECT_EQ (NodeExecutionState::Blocked, fixture.evaluator.Status ("tail").state);
    EXPECT_EQ (statuscode::kBlockedUpstreamHolding, fixture.evaluator.Status ("tail").code);
}

TEST (NodeGraphFlowControl, ReleasePromotesTheStagedValueAndDirtiesDownstream)
{
    FlowFixture fixture;
    BuildDammedChain (fixture);
    ASSERT_TRUE (fixture.Mode ("dam", ExecutionMode::Holding).accepted);
    fixture.Run (1);

    std::string error;
    std::string code;
    ASSERT_TRUE (fixture.evaluator.CanRelease (fixture.graph, "dam", error, code)) << error;
    const std::vector<NodeId> dirtied = fixture.evaluator.ReleaseHolding (fixture.graph, "dam");
    EXPECT_EQ ((std::vector<NodeId> { "tail" }), dirtied) << "the dam itself must not be dirtied by its own release";

    fixture.evaluator.Invalidate (fixture.graph, dirtied);
    fixture.Run (2);

    EXPECT_EQ (statuscode::kHoldingReleased, fixture.evaluator.Status ("dam").code);
    EXPECT_NE (nullptr, fixture.evaluator.Result ("dam"));
    EXPECT_EQ (NodeExecutionState::Success, fixture.evaluator.Status ("tail").state);
}

TEST (NodeGraphFlowControl, ReleaseIsRefusedWhenThereIsNothingToPromote)
{
    FlowFixture fixture;
    ASSERT_TRUE (fixture.Add ("dam", "dataDam"));
    ASSERT_TRUE (fixture.Add ("sum", "add"));

    std::string error;
    std::string code;
    // Holding but never evaluated: nothing to promote, and accepting would dirty
    // the downstream closure in exchange for no new data.
    ASSERT_TRUE (fixture.Mode ("dam", ExecutionMode::Holding).accepted);
    EXPECT_FALSE (fixture.evaluator.CanRelease (fixture.graph, "dam", error, code));
    EXPECT_EQ ("release.nothingStaged", code);

    EXPECT_FALSE (fixture.evaluator.CanRelease (fixture.graph, "sum", error, code));
    EXPECT_EQ ("release.notHolding", code);

    EXPECT_FALSE (fixture.evaluator.CanRelease (fixture.graph, "absent", error, code));
    EXPECT_EQ ("release.unknownNode", code);
}

TEST (NodeGraphFlowControl, HoldIsRefusedForATypeThatIsNotHoldCapable)
{
    FlowFixture fixture;
    ASSERT_TRUE (fixture.Add ("sum", "add"));
    const EditResult rejected = fixture.Mode ("sum", ExecutionMode::Holding);
    EXPECT_FALSE (rejected.accepted);
    EXPECT_EQ ("mode.holdUnsupported", rejected.code);
}

TEST (NodeGraphFlowControl, AModeChangeInvalidatesTheCachedResultItWouldOtherwiseReuse)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    fixture.Run (1);
    EXPECT_DOUBLE_EQ (42.0, OutputNumber (fixture.evaluator, "sum", "value"));

    // The regression this pins: the mode is folded into the cache key, so a
    // switch to bypass cannot be answered out of the enabled run's cache. A key
    // that ignored the mode would return 42 here, and the bypass would look as
    // though it had silently failed.
    const EditResult accepted = fixture.Mode ("sum", ExecutionMode::Bypassed);
    ASSERT_TRUE (accepted.accepted);
    fixture.evaluator.Invalidate (fixture.graph, accepted.dirtyNodes);
    fixture.Run (2);
    EXPECT_DOUBLE_EQ (2.0, OutputNumber (fixture.evaluator, "sum", "value"));
}

TEST (NodeGraphFlowControl, SettingTheModeItAlreadyHasIsRefusedRatherThanBumpingTheRevision)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    const uint64_t before = fixture.graph.Revision ();
    const EditResult rejected = fixture.Mode ("sum", ExecutionMode::Enabled);
    EXPECT_FALSE (rejected.accepted);
    EXPECT_EQ ("mode.unchanged", rejected.code);
    EXPECT_EQ (before, fixture.graph.Revision ());
}

TEST (NodeGraphFlowControl, ModePersistsThroughSerializationAndRetainedValuesDoNot)
{
    FlowFixture fixture;
    BuildArithmeticChain (fixture);
    ASSERT_TRUE (fixture.Add ("dam", "dataDam"));
    ASSERT_TRUE (fixture.Mode ("sum", ExecutionMode::Disabled).accepted);
    ASSERT_TRUE (fixture.Mode ("dam", ExecutionMode::Holding).accepted);

    const SerializeResult written = SerializeGraph (fixture.graph, GraphMetadata {});
    ASSERT_TRUE (written.ok) << written.error;

    const DeserializeResult read = DeserializeGraph (written.text, fixture.registry);
    ASSERT_TRUE (read.ok) << read.error;
    EXPECT_EQ (ExecutionMode::Disabled, read.graph.document.FindNode ("sum")->executionMode);
    EXPECT_EQ (ExecutionMode::Holding, read.graph.document.FindNode ("dam")->executionMode);

    // The retained half is session cache by contract, so nothing about a staged
    // or released value may reach the file.
    EXPECT_EQ (std::string::npos, written.text.find ("staged"));
    EXPECT_EQ (std::string::npos, written.text.find ("released"));
}

TEST (NodeGraphFlowControl, AGraphFileCannotSmuggleInAModeTheTypeDoesNotSupport)
{
    // The second ingress. A hand-edited file saying "holding" on a type that
    // cannot hold would otherwise load into a state no command could produce,
    // after which the evaluator is reasoning about a node whose type never
    // agreed to it.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    Node node { "sum", "add" };
    node.executionMode = ExecutionMode::Holding;
    const EditResult rejected = ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { std::move (node) } });
    EXPECT_FALSE (rejected.accepted);
    EXPECT_EQ ("mode.holdUnsupported", rejected.code);
}

// ---------------------------------------------------------------------------
// UI-1. The parameter edit descriptor, and the input/geometry node library it
// exists to describe.
//
// The descriptor is metadata, so nothing here asserts on rendering. What IS
// testable, and what these pin, is that malformed metadata cannot enter the
// catalog, and that the nodes it describes answer with the value their own
// contract promises rather than the one a client happened to send.
// ---------------------------------------------------------------------------

namespace {

ParameterSchema NumberParameterWith (ParameterUi ui, ValueType valueType = ValueType::Double)
{
    ParameterSchema parameter { "value", "Value", valueType, false, std::nullopt };
    parameter.ui = std::move (ui);
    return parameter;
}

bool RegisterOne (NodeType nodeType, std::string& error)
{
    NodeRegistry registry;
    return registry.Register (std::move (nodeType), error);
}

} // namespace

TEST (NodeGraphRegistry, RefusesAWidgetTheParameterTypeCannotWear)
{
    std::string error;

    NodeType sliderOnText;
    sliderOnText.id = "sliderOnText";
    ParameterUi slider;
    slider.widget = ParameterWidget::Slider;
    slider.minimum = 0.0;
    slider.maximum = 1.0;
    sliderOnText.parameters.push_back (NumberParameterWith (slider, ValueType::String));
    EXPECT_FALSE (RegisterOne (std::move (sliderOnText), error));
    EXPECT_NE (std::string::npos, error.find ("integer or double")) << error;

    NodeType vectorOnDouble;
    vectorOnDouble.id = "vectorOnDouble";
    ParameterUi vector;
    vector.widget = ParameterWidget::Vector;
    vectorOnDouble.parameters.push_back (NumberParameterWith (vector, ValueType::Double));
    EXPECT_FALSE (RegisterOne (std::move (vectorOnDouble), error));
    EXPECT_NE (std::string::npos, error.find ("point3")) << error;
}

TEST (NodeGraphRegistry, RefusesASliderWithNothingToDragAlong)
{
    std::string error;
    NodeType unbounded;
    unbounded.id = "unbounded";
    ParameterUi slider;
    slider.widget = ParameterWidget::Slider;
    slider.minimum = 0.0; // upper bound missing
    unbounded.parameters.push_back (NumberParameterWith (slider));
    EXPECT_FALSE (RegisterOne (std::move (unbounded), error));
    EXPECT_NE (std::string::npos, error.find ("minimum and a maximum")) << error;

    NodeType inverted;
    inverted.id = "inverted";
    ParameterUi backwards;
    backwards.widget = ParameterWidget::Number;
    backwards.minimum = 10.0;
    backwards.maximum = 1.0;
    inverted.parameters.push_back (NumberParameterWith (backwards));
    EXPECT_FALSE (RegisterOne (std::move (inverted), error));
    EXPECT_NE (std::string::npos, error.find ("greater than maximum")) << error;
}

TEST (NodeGraphRegistry, ABoundThatNamesASiblingMustNameARealNumericOne)
{
    std::string error;

    NodeType missing;
    missing.id = "missing";
    ParameterUi named;
    named.widget = ParameterWidget::Slider;
    named.minimumParameter = "nowhere";
    named.maximum = 1.0;
    missing.parameters.push_back (NumberParameterWith (named));
    EXPECT_FALSE (RegisterOne (std::move (missing), error));
    EXPECT_NE (std::string::npos, error.find ("numeric sibling")) << error;

    // A self-reference is a cycle a client would have to resolve while
    // rendering, which is exactly the work the descriptor exists to remove.
    NodeType itself;
    itself.id = "itself";
    ParameterUi selfNamed;
    selfNamed.widget = ParameterWidget::Slider;
    selfNamed.minimumParameter = "value";
    selfNamed.maximum = 1.0;
    itself.parameters.push_back (NumberParameterWith (selfNamed));
    EXPECT_FALSE (RegisterOne (std::move (itself), error));
    EXPECT_NE (std::string::npos, error.find ("its own parameter")) << error;
}

TEST (NodeGraphRegistry, RefusesOptionsThatCannotIdentifyTheirChoice)
{
    std::string error;

    NodeType duplicated;
    duplicated.id = "duplicated";
    ParameterUi options;
    options.widget = ParameterWidget::Select;
    options.options = { { "First", Value (std::string ("a")) }, { "Second", Value (std::string ("a")) } };
    ParameterSchema text { "value", "Value", ValueType::String, false, std::nullopt };
    text.ui = options;
    duplicated.parameters.push_back (text);
    EXPECT_FALSE (RegisterOne (std::move (duplicated), error));
    EXPECT_NE (std::string::npos, error.find ("same value")) << error;

    NodeType mistyped;
    mistyped.id = "mistyped";
    ParameterUi wrongType;
    wrongType.widget = ParameterWidget::Select;
    wrongType.options = { { "One", Value (static_cast<int64_t> (1)) } };
    ParameterSchema stringParameter { "value", "Value", ValueType::String, false, std::nullopt };
    stringParameter.ui = wrongType;
    mistyped.parameters.push_back (stringParameter);
    EXPECT_FALSE (RegisterOne (std::move (mistyped), error));
    EXPECT_NE (std::string::npos, error.find ("value type")) << error;

    NodeType both;
    both.id = "both";
    ParameterUi conflicting;
    conflicting.widget = ParameterWidget::Select;
    conflicting.options = { { "One", Value (std::string ("a")) } };
    conflicting.optionSource = ParameterOptionSource::Layer;
    ParameterSchema conflicted { "value", "Value", ValueType::String, false, std::nullopt };
    conflicted.ui = conflicting;
    both.parameters.push_back (conflicted);
    EXPECT_FALSE (RegisterOne (std::move (both), error));
    EXPECT_NE (std::string::npos, error.find ("not both")) << error;
}

TEST (NodeGraphRegistry, AParameterWithoutADescriptorStaysLegal)
{
    // Every parameter registered before UI-1 is in this state, and the catalog
    // has to keep admitting them - the client falls back to the value type.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* add = registry.Find ("add");
    ASSERT_NE (nullptr, add);
    for (const ParameterSchema& parameter : add->parameters)
        EXPECT_FALSE (parameter.ui.has_value ());

    const NodeType* slider = registry.Find ("numberSlider");
    ASSERT_NE (nullptr, slider);
    const ParameterSchema* value = FindParameter (*slider, "value");
    ASSERT_NE (nullptr, value);
    ASSERT_TRUE (value->ui.has_value ());
    EXPECT_EQ (ParameterWidget::Slider, value->ui->widget);
    EXPECT_EQ ("minimum", value->ui->minimumParameter);
    EXPECT_EQ ("maximum", value->ui->maximumParameter);
    EXPECT_EQ ("step", value->ui->stepParameter);
    EXPECT_EQ ("decimals", value->ui->decimalsParameter);

    // The constants matter as well as the parameter names: a client that has
    // not merged the catalog defaults in yet must still find a bounded range,
    // because a slider with no track is not a slider.
    ASSERT_TRUE (value->ui->minimum.has_value ());
    ASSERT_TRUE (value->ui->maximum.has_value ());
    EXPECT_LT (*value->ui->minimum, *value->ui->maximum);

    // Every one of the range parameters carries a default, which is what the
    // editor merges in so a freshly placed node draws its track at once.
    for (const char* id : { "value", "minimum", "maximum", "step", "decimals" }) {
        const ParameterSchema* parameter = FindParameter (*slider, id);
        ASSERT_NE (nullptr, parameter) << id;
        EXPECT_TRUE (parameter->defaultValue.has_value ()) << id;
    }
}

TEST (NodeGraphInputLibrary, TheSliderClampsAndRoundsInTheNodeRatherThanInTheControl)
{
    // A range a client honours is not a range: a graph loaded from a file, a
    // pasted value or a second client must not be able to produce an
    // out-of-range answer either.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    Node node { "s", "numberSlider" };
    node.parameters.emplace ("value", Value (999.0));
    node.parameters.emplace ("minimum", Value (0.0));
    node.parameters.emplace ("maximum", Value (10.0));
    node.parameters.emplace ("decimals", Value (static_cast<int64_t> (2)));

    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    ASSERT_TRUE (ExecuteRuntimeNode (node, {}, context, outputs, error)) << error;
    EXPECT_DOUBLE_EQ (10.0, std::get<double> (outputs.at ("value").DataValue ()));

    Node rounded { "r", "numberSlider" };
    rounded.parameters.emplace ("value", Value (1.23456));
    rounded.parameters.emplace ("minimum", Value (0.0));
    rounded.parameters.emplace ("maximum", Value (10.0));
    rounded.parameters.emplace ("decimals", Value (static_cast<int64_t> (2)));
    ValueMap roundedOutputs;
    ASSERT_TRUE (ExecuteRuntimeNode (rounded, {}, context, roundedOutputs, error)) << error;
    EXPECT_DOUBLE_EQ (1.23, std::get<double> (roundedOutputs.at ("value").DataValue ()));
}

TEST (NodeGraphInputLibrary, APointAssemblesIndependentlyWiredCoordinates)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* pointType = registry.Find ("point");
    ASSERT_NE (nullptr, pointType);
    ASSERT_EQ (3U, pointType->inputs.size ());
    EXPECT_EQ ("x", pointType->inputs[0].id);
    EXPECT_EQ ("y", pointType->inputs[1].id);
    EXPECT_EQ ("z", pointType->inputs[2].id);
    EXPECT_TRUE (std::all_of (pointType->inputs.begin (), pointType->inputs.end (),
                              [] (const PortSchema& port) { return port.valueType == ValueType::Double; }));

    Node node { "p", "point" };
    node.parameters.emplace ("x", Value (1.0));
    node.parameters.emplace ("y", Value (2.0));
    node.parameters.emplace ("z", Value (3.0));

    ValueMap typedOutputs;
    std::string error;
    NodeExecutionContext context;
    ASSERT_TRUE (ExecuteRuntimeNode (node, {}, context, typedOutputs, error)) << error;
    EXPECT_DOUBLE_EQ (2.0, std::get<Point3> (typedOutputs.at ("point").DataValue ()).y);

    ValueMap inputs;
    inputs.emplace ("y", Value (8.0));
    ValueMap wiredOutputs;
    ASSERT_TRUE (ExecuteRuntimeNode (node, inputs, context, wiredOutputs, error)) << error;
    const Point3 wired = std::get<Point3> (wiredOutputs.at ("point").DataValue ());
    EXPECT_DOUBLE_EQ (1.0, wired.x);
    EXPECT_DOUBLE_EQ (8.0, wired.y);
    EXPECT_DOUBLE_EQ (3.0, wired.z);
}

TEST (NodeGraphInputLibrary, AVectorReportsItsLengthBesideItself)
{
    Node node { "v", "vector" };
    node.parameters.emplace ("x", Value (3.0));
    node.parameters.emplace ("y", Value (4.0));
    node.parameters.emplace ("z", Value (0.0));
    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    ASSERT_TRUE (ExecuteRuntimeNode (node, {}, context, outputs, error)) << error;
    EXPECT_DOUBLE_EQ (5.0, std::get<double> (outputs.at ("length").DataValue ()));
}

TEST (NodeGraphGeometry, VectorAndPolygonNodesUseTheGeometryEngine)
{
    NodeExecutionContext context;
    std::string error;

    Node cross { "cross", "geom.vectorCross" };
    ValueMap crossOutputs;
    ASSERT_TRUE (ExecuteRuntimeNode (
        cross, { { "left", Value (Point3 { 1.0, 0.0, 0.0 }) }, { "right", Value (Point3 { 0.0, 1.0, 0.0 }) } }, context,
        crossOutputs, error))
        << error;
    EXPECT_DOUBLE_EQ (1.0, std::get<Point3> (crossOutputs.at ("vector").DataValue ()).z);

    Polygon subject { { { 0.0, 0.0, 0.0 }, { 2.0, 0.0, 0.0 }, { 2.0, 2.0, 0.0 }, { 0.0, 2.0, 0.0 } } };
    Polygon clip { { { 1.0, 0.0, 0.0 }, { 3.0, 0.0, 0.0 }, { 3.0, 2.0, 0.0 }, { 1.0, 2.0, 0.0 } } };
    Node unite { "union", "geom.polygonUnion" };
    ValueMap unionOutputs;
    ASSERT_TRUE (ExecuteRuntimeNode (unite, { { "subject", Value (subject) }, { "clip", Value (clip) } }, context,
                                     unionOutputs, error))
        << error;
    const std::vector<Value>& polygons = unionOutputs.at ("polygons").Items ();
    ASSERT_EQ (1U, polygons.size ());
    EXPECT_EQ (ValueType::Polygon, polygons.front ().Type ());
}

TEST (NodeGraphInputLibrary, EveryAttributePickerIsPureAndAnswersWithWhatWasPicked)
{
    // The pickers must not need Archicad to EVALUATE. What needs Archicad is
    // listing the choices, and that is a separate native verb - so a graph
    // carrying a layer name still runs with no project open.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    for (const char* id : { "attribute.layer", "attribute.fill", "attribute.lineType", "attribute.surface",
                            "attribute.buildingMaterial", "attribute.composite", "attribute.profile" }) {
        const NodeType* type = registry.Find (id);
        ASSERT_NE (nullptr, type) << id;
        EXPECT_EQ (EffectKind::Pure, type->effect) << id;
        EXPECT_EQ (ExecutionDomain::Worker, type->executionDomain) << id;
        EXPECT_TRUE (type->generations.Empty ()) << id;
        const ParameterSchema* parameter = FindParameter (*type, "value");
        ASSERT_NE (nullptr, parameter) << id;
        ASSERT_TRUE (parameter->ui.has_value ()) << id;
        EXPECT_EQ (ParameterWidget::Select, parameter->ui->widget) << id;
        EXPECT_NE (ParameterOptionSource::None, parameter->ui->optionSource) << id;
        EXPECT_TRUE (parameter->ui->options.empty ()) << id;
    }

    // A pen is the one picked by number, because a pen IS its number.
    const NodeType* pen = registry.Find ("attribute.pen");
    ASSERT_NE (nullptr, pen);
    EXPECT_EQ (ValueType::Integer, FindParameter (*pen, "value")->valueType);

    Node node { "layer", "attribute.layer" };
    node.parameters.emplace ("value", Value (std::string ("Existing Structures")));
    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    ASSERT_TRUE (ExecuteRuntimeNode (node, {}, context, outputs, error)) << error;
    EXPECT_EQ ("Existing Structures", std::get<std::string> (outputs.at ("value").DataValue ()));
}

// ---------------------------------------------------------------------------
// The preview component: a run's results -> what the viewport draws.
// ---------------------------------------------------------------------------

namespace {

// A Preview node with something wired into it.
//
// ⚠️ THE GEOMETRY IS ON THE NODE UPSTREAM, which is the whole shape of this
// feature: a Preview is a TERMINAL with no outputs, so the projection follows the
// edge into it rather than reading a result of its own. The fixture therefore
// builds a source node too, and the Preview's own result carries nothing but the
// fact that it RAN - which is what says the last evaluation reached it.
struct PreviewFixture {
    GraphDocument document;
    NodeRegistry registry = MakeRuntimeNodeRegistry ();
    std::map<NodeId, std::shared_ptr<const NodeResult>> results;

    void AddPreview (const std::string& nodeId, const Argument& geometry, std::map<std::string, Value> parameters = {})
    {
        GraphEdit edit;
        AddNodeEdit add;
        add.node.id = nodeId;
        add.node.nodeType = kPreviewNodeType;
        for (auto& [id, value] : parameters)
            add.node.parameters.emplace (id, std::move (value));
        edit.data = add;
        const EditResult result = ApplyEdit (document, registry, edit);
        EXPECT_TRUE (result.accepted) << result.error;

        // A source whose output the Preview is wired to. `panel` is used because
        // its input is Absent - "any type" - so the edit rules accept a wire from
        // it into the Preview whatever the geometry happens to be.
        const std::string sourceId = nodeId + "-source";
        GraphEdit sourceEdit;
        AddNodeEdit source;
        source.node.id = sourceId;
        source.node.nodeType = "panel";
        sourceEdit.data = source;
        EXPECT_TRUE (ApplyEdit (document, registry, sourceEdit).accepted);

        GraphEdit connectEdit;
        ConnectEdit wire;
        wire.edge = Edge { sourceId, "value", nodeId, kPreviewGeometryInput };
        connectEdit.data = wire;
        EXPECT_TRUE (ApplyEdit (document, registry, connectEdit).accepted);

        auto upstream = std::make_shared<NodeResult> ();
        upstream->outputs.emplace ("value", AsTree (geometry));
        results[sourceId] = upstream;
        // The Preview publishes nothing; its result exists only to say it ran.
        results[nodeId] = std::make_shared<NodeResult> ();
    }

    PreviewResultLookup Lookup ()
    {
        return [this] (const NodeId& nodeId) -> std::shared_ptr<const NodeResult> {
            const auto found = results.find (nodeId);
            return found == results.end () ? nullptr : found->second;
        };
    }
};

Value PolylineValue (size_t points)
{
    Polyline polyline;
    for (size_t index = 0; index < points; ++index)
        polyline.points.push_back (Point3 { static_cast<double> (index), 0.0, 0.0 });
    return Value (std::move (polyline));
}

} // namespace

TEST (NodeGraphPreview, ProjectsGeometryByValueType)
{
    PreviewFixture fixture;
    Polygon polygon;
    polygon.points = { Point3 { 0, 0, 0 }, Point3 { 1, 0, 0 }, Point3 { 1, 1, 0 } };
    fixture.AddPreview ("p", Argument::FromItems (
                                 { Value (Point3 { 1.0, 2.0, 3.0 }), PolylineValue (4), Value (std::move (polygon)) }));

    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());

    ASSERT_EQ (3U, projection.primitives.size ());
    EXPECT_EQ (evp::preview::PreviewKind::PointMarker, projection.primitives[0]->kind);
    EXPECT_EQ (evp::preview::PreviewKind::Polyline3D, projection.primitives[1]->kind);
    EXPECT_FALSE (projection.primitives[1]->closed);
    // A polygon is a CLOSED polyline and says so with the flag rather than by
    // repeating its first point.
    EXPECT_EQ (evp::preview::PreviewKind::Polyline3D, projection.primitives[2]->kind);
    EXPECT_TRUE (projection.primitives[2]->closed);
    EXPECT_EQ (9U, projection.primitives[2]->positions.size ());
    EXPECT_EQ (1U, projection.enabledNodes);
    EXPECT_EQ (0U, projection.nonGeometricValues);
    EXPECT_FALSE (projection.truncated);
}

TEST (NodeGraphPreview, CountsValuesThatHaveNoGeometry)
{
    // "I wired a number into Preview" and "my geometry never arrived" look the
    // same in an empty viewport. They must not be the same in the report.
    PreviewFixture fixture;
    fixture.AddPreview ("p", Argument::FromItems ({ Value (42.0), Value (std::string ("hello")) }));

    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());

    EXPECT_TRUE (projection.primitives.empty ());
    EXPECT_EQ (2U, projection.nonGeometricValues);
    EXPECT_EQ (1U, projection.enabledNodes);
}

TEST (NodeGraphPreview, ShowSwitchAndExecutionModeBothSuppressIt)
{
    PreviewFixture fixture;
    fixture.AddPreview ("off", PolylineValue (2), { { kPreviewEnabledParameter, Value (false) } });
    fixture.AddPreview ("on", PolylineValue (2));

    PreviewProjection projection = ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    EXPECT_EQ (2U, projection.previewNodes);
    EXPECT_EQ (1U, projection.enabledNodes);
    ASSERT_EQ (1U, projection.primitives.size ());

    // A node disabled on the canvas did not run, so its cached result describes a
    // graph that no longer exists. Drawing it would be a picture of geometry the
    // graph is not producing.
    GraphEdit edit;
    SetExecutionModeEdit mode;
    mode.nodeId = "on";
    mode.mode = ExecutionMode::Disabled;
    edit.data = mode;
    ASSERT_TRUE (ApplyEdit (fixture.document, fixture.registry, edit).accepted);

    projection = ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    EXPECT_EQ (0U, projection.enabledNodes);
    EXPECT_TRUE (projection.primitives.empty ());
}

TEST (NodeGraphPreview, PrimitiveIdsAreStableAndCannotCollideWithGrasshopper)
{
    PreviewFixture fixture;
    fixture.AddPreview ("p", Argument::FromItems ({ PolylineValue (2), PolylineValue (3) }));

    const PreviewProjection first =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    const PreviewProjection again =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    ASSERT_EQ (2U, first.primitives.size ());
    EXPECT_EQ (first.primitives[0]->id, again.primitives[0]->id);
    EXPECT_NE (first.primitives[0]->id, first.primitives[1]->id);

    // The worker allocates ids from a counter, so the top bit partitions the two
    // producers by construction rather than by hoping.
    for (const auto& primitive : first.primitives)
        EXPECT_NE (0ull, primitive->id & evp::preview::kGraphPreviewIdBit);

    // A different graph with the same node id is a different primitive.
    const PreviewProjection other =
        ProjectGraphPreview ("other", fixture.document, fixture.registry, fixture.Lookup ());
    EXPECT_NE (first.primitives[0]->id, other.primitives[0]->id);
}

TEST (NodeGraphPreview, ColourIsTheNodesWhenItParsesAndTheStylesWhenItDoesNot)
{
    uint32_t rgba = 0;
    EXPECT_TRUE (ParsePreviewColour ("#FF8000", rgba));
    EXPECT_EQ (0xFF8000FFu, rgba);
    EXPECT_TRUE (ParsePreviewColour ("#ff800080", rgba));
    EXPECT_EQ (0xFF800080u, rgba);
    EXPECT_FALSE (ParsePreviewColour ("orange", rgba));
    EXPECT_FALSE (ParsePreviewColour ("#FF80", rgba));
    EXPECT_FALSE (ParsePreviewColour ("#GG0000", rgba));

    PreviewFixture fixture;
    fixture.AddPreview ("good", PolylineValue (2), { { kPreviewColorParameter, Value (std::string ("#123456")) } });
    fixture.AddPreview ("bad", PolylineValue (2), { { kPreviewColorParameter, Value (std::string ("nonsense")) } });

    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    ASSERT_EQ (2U, projection.primitives.size ());
    // Ordered by node id, so "bad" comes before "good". A colour that does not
    // parse leaves the primitive to the viewport style: a mistyped colour should
    // show the geometry, not hide it.
    EXPECT_FALSE (projection.primitives[0]->hasOwnColour);
    EXPECT_TRUE (projection.primitives[1]->hasOwnColour);
    EXPECT_EQ (0x123456FFu, projection.primitives[1]->rgba);
}

TEST (NodeGraphPreview, TruncatesRatherThanAllocatingWithoutBound)
{
    PreviewFixture fixture;
    std::vector<Value> many;
    for (int index = 0; index < 50; ++index)
        many.push_back (PolylineValue (2));
    fixture.AddPreview ("p", Argument::FromItems (std::move (many)));

    PreviewProjectionLimits limits;
    limits.maxPrimitives = 10;
    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup (), limits);
    EXPECT_EQ (10U, projection.primitives.size ());
    EXPECT_TRUE (projection.truncated);
}

TEST (NodeGraphPreview, StoreReplacesAGraphsWholeSetAndDropsItWhenEmpty)
{
    // A store of its own, not GraphPreviewStore::Get (): the accessor is an
    // accessor and not a singleton contract, and a test that used the
    // process-wide one would leak state into whatever ran next.
    evp::preview::GraphPreviewStore store;
    EXPECT_EQ (nullptr, store.SnapshotCopy ());

    auto primitive = std::make_shared<evp::preview::GhPreviewPrimitive> ();
    store.PublishGraph ("a", { primitive, primitive });
    store.PublishGraph ("b", { primitive });
    EXPECT_EQ (2U, store.GraphCount ());
    EXPECT_EQ (3U, store.Count ());
    ASSERT_NE (nullptr, store.SnapshotCopy ());
    EXPECT_EQ (3U, store.SnapshotCopy ()->primitives.size ());

    // Publishing nothing is how a graph says it draws nothing. It is the same
    // state as having been cleared, so it is the same entry - otherwise
    // GraphCount would count graphs that draw nothing.
    const uint64_t before = store.Generation ();
    store.PublishGraph ("a", {});
    EXPECT_EQ (1U, store.GraphCount ());
    EXPECT_EQ (1U, store.Count ());
    EXPECT_GT (store.Generation (), before);

    // ... and republishing nothing changes nothing, so the renderer's generation
    // compare does not rebuild a layer for it.
    const uint64_t settled = store.Generation ();
    store.PublishGraph ("a", {});
    EXPECT_EQ (settled, store.Generation ());

    store.ClearAll ();
    EXPECT_EQ (0U, store.GraphCount ());
    EXPECT_TRUE (store.SnapshotCopy ()->primitives.empty ());
}

TEST (NodeGraphPreview, MergingTheTwoProducersKeepsBothAndTracksEitherChanging)
{
    // The renderer rebuilds its buffers only when the combined generation moves,
    // so the merge has to notice EITHER producer changing. A sum would not: two
    // generations moving in opposite directions by the same amount would freeze
    // the viewport on old geometry with nothing in the log to say so.
    auto ghPrimitive = std::make_shared<evp::preview::GhPreviewPrimitive> ();
    ghPrimitive->id = 7;
    auto graphPrimitive = std::make_shared<evp::preview::GhPreviewPrimitive> ();
    graphPrimitive->id = evp::preview::GraphPreviewPrimitiveId ("g", "n", 0);

    auto gh = std::make_shared<evp::preview::GhPreviewSnapshot> ();
    gh->generation = 4;
    gh->primitives.push_back (ghPrimitive);
    auto graph = std::make_shared<evp::preview::GhPreviewSnapshot> ();
    graph->generation = 9;
    graph->primitives.push_back (graphPrimitive);

    // Neither: nothing to draw and no layer to build.
    EXPECT_EQ (0U, evp::preview::MergePreviewSnapshots (nullptr, nullptr).generation);
    EXPECT_EQ (nullptr, evp::preview::MergePreviewSnapshots (nullptr, nullptr).snapshot);

    // One side alone is handed straight through rather than copied.
    EXPECT_EQ (gh, evp::preview::MergePreviewSnapshots (gh, nullptr).snapshot);
    EXPECT_EQ (graph, evp::preview::MergePreviewSnapshots (nullptr, graph).snapshot);

    const evp::preview::MergedPreview both = evp::preview::MergePreviewSnapshots (gh, graph);
    ASSERT_NE (nullptr, both.snapshot);
    EXPECT_EQ (2U, both.snapshot->primitives.size ());
    EXPECT_NE (0U, both.generation);

    auto ghMoved = std::make_shared<evp::preview::GhPreviewSnapshot> (*gh);
    ghMoved->generation = 5;
    auto graphMoved = std::make_shared<evp::preview::GhPreviewSnapshot> (*graph);
    graphMoved->generation = 8;
    // Each half moving alone changes it ...
    EXPECT_NE (both.generation, evp::preview::MergePreviewSnapshots (ghMoved, graph).generation);
    EXPECT_NE (both.generation, evp::preview::MergePreviewSnapshots (gh, graphMoved).generation);
    // ... and so does the pair that a sum would have called equal.
    EXPECT_NE (both.generation, evp::preview::MergePreviewSnapshots (ghMoved, graphMoved).generation);
}

TEST (NodeGraphPreview, RuntimePublishesOnEveryRunAndWithdrawsOnDelete)
{
    // The end-to-end shape, through the runtime rather than the projection: an
    // ordinary evaluation must fill the viewport, and DELETING the node must
    // empty it without waiting for something else to run. Stale preview is worse
    // than none, because nothing about it says it is stale.
    GraphRuntimeState& runtime = GraphRuntimeState::Get ();
    const GraphId graphId = "preview-test-graph";

    GraphEdit addPoint;
    AddNodeEdit point;
    point.node.id = "pt";
    point.node.nodeType = "point";
    point.node.parameters = { { "x", Value (1.0) }, { "y", Value (2.0) }, { "z", Value (3.0) } };
    addPoint.data = point;
    ASSERT_TRUE (runtime.Apply (graphId, addPoint).accepted);

    GraphEdit addPreview;
    AddNodeEdit previewNode;
    previewNode.node.id = "pv";
    previewNode.node.nodeType = kPreviewNodeType;
    addPreview.data = previewNode;
    ASSERT_TRUE (runtime.Apply (graphId, addPreview).accepted);

    GraphEdit connect;
    ConnectEdit wire;
    wire.edge = Edge { "pt", "point", "pv", "geometry" };
    connect.data = wire;
    ASSERT_TRUE (runtime.Apply (graphId, connect).accepted);

    EvaluationRequest request;
    const EvaluationSummary summary = runtime.Evaluate (graphId, request);
    ASSERT_TRUE (summary.succeeded) << summary.error;

    auto snapshot = evp::preview::GraphPreviewStore::Get ().SnapshotCopy ();
    ASSERT_NE (nullptr, snapshot);
    ASSERT_EQ (1U, snapshot->primitives.size ());
    EXPECT_EQ (evp::preview::PreviewKind::PointMarker, snapshot->primitives[0]->kind);

    GraphEdit remove;
    RemoveNodeEdit removal;
    removal.nodeId = "pv";
    remove.data = removal;
    ASSERT_TRUE (runtime.Apply (graphId, remove).accepted);

    snapshot = evp::preview::GraphPreviewStore::Get ().SnapshotCopy ();
    ASSERT_NE (nullptr, snapshot);
    EXPECT_TRUE (snapshot->primitives.empty ());

    evp::preview::GraphPreviewStore::Get ().ClearAll ();
}

TEST (NodeGraphPreview, ThePreviewNodeIsATerminalWithNoOutputs)
{
    // ⚠️ NO OUTPUTS, AND THE PROJECTION FOLLOWS THE WIRE INSTEAD. The node had a
    // pass-through output only because outputs are the only thing the evaluator
    // caches - which put two ports on a terminal that nobody would ever wire and
    // made "List of 1" appear beside a node whose whole job is to show a shape.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* preview = registry.Find (kPreviewNodeType);
    ASSERT_NE (nullptr, preview);
    EXPECT_TRUE (preview->outputs.empty ());
    ASSERT_EQ (1U, preview->inputs.size ());
    EXPECT_EQ (kPreviewGeometryInput, preview->inputs.front ().id);

    // And it still projects, from the node upstream of it.
    PreviewFixture fixture;
    fixture.AddPreview ("p", PolylineValue (3));
    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    EXPECT_EQ (1U, projection.enabledNodes);
    EXPECT_EQ (1U, projection.primitives.size ());
}

TEST (NodeGraphPreview, TheTargetParameterDecidesWhetherArchicadDrawsIt)
{
    // ONE switch for both halves. The runtime reads it for the overlay and the
    // editor reads the same parameter for the node viewport; a switch each would
    // let them disagree, and "showing nothing" would have two causes that look
    // the same.
    EXPECT_TRUE (PreviewTargetDrawsInArchicad ("both"));
    EXPECT_TRUE (PreviewTargetDrawsInArchicad ("archicad"));
    EXPECT_FALSE (PreviewTargetDrawsInArchicad ("node"));
    // An absent or unrecognised target DRAWS: a graph saved by a later build
    // should show its geometry rather than hide it, because a viewport that
    // silently shows nothing is indistinguishable from a node that produced
    // nothing and the user would debug the wrong half.
    EXPECT_TRUE (PreviewTargetDrawsInArchicad (""));
    EXPECT_TRUE (PreviewTargetDrawsInArchicad ("hologram"));

    PreviewFixture fixture;
    fixture.AddPreview ("here", PolylineValue (2), { { kPreviewTargetParameter, Value (std::string ("archicad")) } });
    fixture.AddPreview ("nodeOnly", PolylineValue (2), { { kPreviewTargetParameter, Value (std::string ("node")) } });

    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());
    EXPECT_EQ (2U, projection.previewNodes);
    ASSERT_EQ (1U, projection.primitives.size ());
    // The node-only one is still a preview node - it is previewing, just not
    // here - so it is counted and not drawn.
    EXPECT_EQ (1U, projection.enabledNodes);
}

TEST (NodeGraphPreview, ThePreviewTargetWidgetIsPinnedToItsThreeSpellings)
{
    // A client dispatches on this widget and then reads the VALUE. A node type
    // offering "archicadOverlay" instead of "archicad" would register happily
    // and then draw in the wrong place, which is a picture rather than an error.
    NodeRegistry registry;
    std::string error;

    const auto typeWith = [] (std::vector<ParameterOption> options, ValueType valueType) {
        NodeType type { "t", "T", "Core", "" };
        ParameterSchema parameter { "target", "Draw in", valueType, false, Value (std::string ("both")) };
        ParameterUi ui;
        ui.widget = ParameterWidget::PreviewTarget;
        ui.options = std::move (options);
        parameter.ui = std::move (ui);
        type.parameters.push_back (std::move (parameter));
        type.outputs.push_back ({ "out", "Out", ValueType::String });
        return type;
    };

    const std::vector<ParameterOption> good { { "Node", Value (std::string ("node")) },
                                              { "Archicad", Value (std::string ("archicad")) },
                                              { "Both", Value (std::string ("both")) } };
    EXPECT_TRUE (registry.Register (typeWith (good, ValueType::String), error)) << error;

    NodeRegistry other;
    const std::vector<ParameterOption> misspelt { { "Node", Value (std::string ("node")) },
                                                  { "Archicad", Value (std::string ("archicadOverlay")) },
                                                  { "Both", Value (std::string ("both")) } };
    EXPECT_FALSE (other.Register (typeWith (misspelt, ValueType::String), error));
    EXPECT_FALSE (other.Register (typeWith ({ good[0], good[1] }, ValueType::String), error));
    EXPECT_FALSE (other.Register (typeWith (good, ValueType::Bool), error));
}

// ---------------------------------------------------------------------------
// Solids
// ---------------------------------------------------------------------------

TEST (NodeGraphGeometry, TheSolidNodesProduceMeshesAndRefuseDegenerateOnes)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node box;
    box.id = "b";
    box.nodeType = "geom.box";
    box.parameters = { { "centre", Value (Point3 { 1.0, 2.0, 3.0 }) },
                       { "width", Value (2.0) },
                       { "depth", Value (2.0) },
                       { "height", Value (2.0) } };
    ASSERT_TRUE (ExecuteRuntimeNode (box, {}, context, outputs, error)) << error;
    ASSERT_EQ (ValueType::Mesh, outputs.at ("mesh").Type ());
    const auto mesh = std::get<Value::ImmutableMesh> (outputs.at ("mesh").DataValue ());
    ASSERT_NE (nullptr, mesh);
    // 24 vertices, not 8: shared corners would average three face normals and
    // the box would render as a rounded lump with no edges.
    EXPECT_EQ (24U, mesh->VertexCount ());
    EXPECT_EQ (12U, mesh->TriangleCount ());
    EXPECT_EQ (mesh->vertices.size (), mesh->normals.size ());
    // Centred on the point it was given, not anchored at it.
    EXPECT_NEAR (0.0, mesh->bounds.mn[0], 1e-9);
    EXPECT_NEAR (2.0, mesh->bounds.mx[0], 1e-9);
    // Centre z = 3, height 2, so the box spans 2..4 on Z.
    EXPECT_NEAR (2.0, mesh->bounds.mn[2], 1e-9);
    EXPECT_NEAR (4.0, mesh->bounds.mx[2], 1e-9);

    // A zero extent is REFUSED rather than built: it is far more often a wire
    // into the wrong port than a request, and a degenerate solid hides that.
    Node flat = box;
    flat.parameters["height"] = Value (0.0);
    outputs.clear ();
    error.clear ();
    EXPECT_FALSE (ExecuteRuntimeNode (flat, {}, context, outputs, error));
    EXPECT_FALSE (error.empty ());

    Node sphere;
    sphere.id = "s";
    sphere.nodeType = "geom.sphere";
    sphere.parameters = { { "centre", Value (Point3 { 0.0, 0.0, 0.0 }) },
                          { "radius", Value (1.0) },
                          { "segments", Value (static_cast<int64_t> (8)) } };
    outputs.clear ();
    error.clear ();
    ASSERT_TRUE (ExecuteRuntimeNode (sphere, {}, context, outputs, error)) << error;
    const auto ball = std::get<Value::ImmutableMesh> (outputs.at ("mesh").DataValue ());
    ASSERT_NE (nullptr, ball);
    EXPECT_GT (ball->TriangleCount (), 0U);
    // Every vertex is on the sphere, and its normal points out of the centre.
    for (std::size_t index = 0; index + 2 < ball->vertices.size (); index += 3) {
        const double x = ball->vertices[index];
        const double y = ball->vertices[index + 1];
        const double z = ball->vertices[index + 2];
        EXPECT_NEAR (1.0, std::sqrt (x * x + y * y + z * z), 1e-9);
        EXPECT_NEAR (x, static_cast<double> (ball->normals[index]), 1e-6);
    }

    // ⚠️ THE SEGMENT COUNT IS AN INTEGER PARAMETER, AND IT HAS TO REACH THE
    // BUILDER. A reader that accepted only Double silently returned the default
    // instead - the node ran, produced a plausible sphere, and ignored what the
    // user typed.
    Node coarse = sphere;
    coarse.parameters["segments"] = Value (static_cast<int64_t> (4));
    outputs.clear ();
    ASSERT_TRUE (ExecuteRuntimeNode (coarse, {}, context, outputs, error)) << error;
    const auto lumpy = std::get<Value::ImmutableMesh> (outputs.at ("mesh").DataValue ());
    EXPECT_LT (lumpy->TriangleCount (), ball->TriangleCount ());

    // A ceiling, not a clamp: the cost is quadratic, and a typed extra zero
    // would allocate gigabytes inside Archicad before anything could report it.
    Node absurd = sphere;
    absurd.parameters["segments"] = Value (static_cast<int64_t> (100000));
    outputs.clear ();
    error.clear ();
    EXPECT_FALSE (ExecuteRuntimeNode (absurd, {}, context, outputs, error));
    EXPECT_FALSE (error.empty ());
}

TEST (NodeGraphPreview, ASolidReachesTheViewportAsOneMeshPrimitive)
{
    // The whole chain the solids exist for: a mesh value becomes a mesh
    // primitive with its normals intact, so the renderer flat-shades it rather
    // than drawing a silhouette.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;
    Node box;
    box.id = "b";
    box.nodeType = "geom.box";
    ASSERT_TRUE (ExecuteRuntimeNode (box, {}, context, outputs, error)) << error;

    PreviewFixture fixture;
    fixture.AddPreview ("p", outputs.at ("mesh"));
    const PreviewProjection projection =
        ProjectGraphPreview ("graph", fixture.document, fixture.registry, fixture.Lookup ());

    ASSERT_EQ (1U, projection.primitives.size ());
    EXPECT_EQ (evp::preview::PreviewKind::TriangleMesh, projection.primitives[0]->kind);
    EXPECT_EQ (24U * 3U, projection.primitives[0]->positions.size ());
    EXPECT_EQ (projection.primitives[0]->positions.size (), projection.primitives[0]->normals.size ());
    EXPECT_EQ (12U * 3U, projection.primitives[0]->indices.size ());
}

// ---------------------------------------------------------------------------
// A node has to work the moment it is placed.
// ---------------------------------------------------------------------------

// ⚠️ THE INVARIANT THIS WHOLE CATALOG RESTS ON. A freshly placed node stores no
// parameters at all - the catalog's defaultValue is what the editor SHOWS in the
// mini-UI, not something written onto the node - so a node whose ports were
// declared required failed the instant it was dropped on the canvas, reporting
// "required input is unconnected" while its own controls sat there full of
// perfectly good numbers.
//
// A Box knows how big it is. Its ports exist so something upstream can take
// over, not so the node is useless without them. NodeRegistry::Register derives
// the flag from "does this input have a defaulted parameter of the same id",
// and this is what proves it holds for every type at once rather than for the
// twenty somebody remembered.
TEST (NodeGraphBuiltins, EveryNodeWithTypedInDefaultsEvaluatesWithNothingWired)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;

    for (const auto& [id, nodeType] : registry.Types ()) {
        // Only the types that claim to be self-sufficient: every input either
        // has a defaulted parameter behind it or is optional.
        bool selfSufficient = true;
        for (const PortSchema& input : nodeType.inputs) {
            if (input.required)
                selfSufficient = false;
        }
        if (!selfSufficient || nodeType.executionDomain != ExecutionDomain::Worker ||
            nodeType.effect != EffectKind::Pure) {
            continue;
        }
        // ⚠️ A SCRIPT NODE IS THE ONE TYPE THIS TEST'S PREMISE DOES NOT COVER, and
        // excluding it is the honest answer rather than a weakening. Every other
        // node in the catalog carries its own behaviour, so "placed and left
        // alone, it works" is a property worth enforcing. A script node's
        // behaviour is authored in a FILE that does not exist until the user
        // makes one, so a freshly placed one correctly reports that it has no
        // file yet. Making it pass here would mean inventing a default body,
        // which is a worse thing to own than this exception.
        if (IsScriptNodeType (id))
            continue;
        // ⚠️ A TREE-NATIVE TYPE IS THE OTHER TYPE THIS TEST'S PREMISE DOES NOT
        // COVER. Its behaviour lives on NodeType::treeBody, not on an
        // ExecuteRuntimeNode case (see TreeNodes.hpp) - calling
        // ExecuteRuntimeNode directly, as this test does, is exactly the value-
        // level path these nodes exist to opt out of, and it correctly reports
        // "unknown node type" for one. Evaluating them from their own defaults
        // is what TreeNodes.GraftAndFlattenActuallyChangeBranchStructure... and
        // its siblings in test_treenodes.cpp already do, through the real
        // evaluator and RunLiftedNode's treeBody short-circuit.
        if (nodeType.treeBody)
            continue;

        Node node;
        node.id = "n";
        node.nodeType = id;
        // The parameters the runtime would resolve from the catalog. Applied
        // here because the EDITOR shows them and the evaluator resolves them;
        // the node body has to agree with both.
        for (const ParameterSchema& parameter : nodeType.parameters) {
            if (parameter.defaultValue.has_value ())
                node.parameters[parameter.id] = *parameter.defaultValue;
        }

        ValueMap inputs;
        for (const PortSchema& input : nodeType.inputs) {
            const auto stored = node.parameters.find (input.id);
            inputs.emplace (input.id, stored == node.parameters.end () ? Value {} : stored->second);
        }

        ValueMap outputs;
        std::string error;
        EXPECT_TRUE (ExecuteRuntimeNode (node, inputs, context, outputs, error))
            << id << " cannot evaluate from its own defaults: " << error;
    }
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

TEST (NodeGraphTransform, AMirrorReversesMeshWindingAsWellAsPositions)
{
    // A reflected solid whose triangles keep their order is inside-out. It looks
    // correct until something culls back faces, and then half of it disappears -
    // a picture, with nothing in any log.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node box;
    box.id = "b";
    box.nodeType = "geom.box";
    ASSERT_TRUE (ExecuteRuntimeNode (box, {}, context, outputs, error)) << error;
    const Argument mesh = outputs.at ("mesh");
    const auto before = std::get<Value::ImmutableMesh> (mesh.DataValue ());

    Node mirror;
    mirror.id = "m";
    mirror.nodeType = "geom.mirror";
    mirror.parameters = { { "origin", Value (Point3 { 0.0, 0.0, 0.0 }) },
                          { "normal", Value (Point3 { 1.0, 0.0, 0.0 }) } };
    ValueMap mirrorInputs;
    mirrorInputs.emplace ("geometry", mesh);
    outputs.clear ();
    ASSERT_TRUE (ExecuteRuntimeNode (mirror, mirrorInputs, context, outputs, error)) << error;

    const std::vector<Value>& moved = outputs.at ("geometry").Items ();
    ASSERT_EQ (1U, moved.size ());
    const auto after = std::get<Value::ImmutableMesh> (moved[0].DataValue ());
    ASSERT_NE (nullptr, after);
    ASSERT_EQ (before->triangles.size (), after->triangles.size ());

    // X is negated ...
    EXPECT_NEAR (-before->vertices[0], after->vertices[0], 1e-9);
    EXPECT_NEAR (before->vertices[1], after->vertices[1], 1e-9);
    // ... and the winding of every triangle is reversed.
    for (std::size_t index = 0; index + 2 < before->triangles.size (); index += 3) {
        EXPECT_EQ (before->triangles[index], after->triangles[index]);
        EXPECT_EQ (before->triangles[index + 1], after->triangles[index + 2]);
        EXPECT_EQ (before->triangles[index + 2], after->triangles[index + 1]);
    }
}

TEST (NodeGraphTransform, ANonUniformScaleKeepsNormalsPerpendicular)
{
    // The inverse-transpose case, and the reason ApplyToNormal exists at all: a
    // normal transformed as a direction stops being perpendicular to its surface
    // the moment the axes scale differently, and the shading then goes visibly
    // wrong on exactly one axis.
    geomsrv::engine::Transform transform;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::Scaling ({ 0, 0, 0 }, { 4.0, 1.0, 1.0 }, transform, error)) << error;

    // A surface running diagonally in XY: the edge (1,1,0), whose normal is
    // (1,-1,0) before the scale.
    const geomsrv::engine::Vector3 edge = geomsrv::engine::ApplyToDirection (transform, { 1.0, 1.0, 0.0 });
    const geomsrv::engine::Vector3 normal = geomsrv::engine::ApplyToNormal (transform, { 1.0, -1.0, 0.0 });
    EXPECT_NEAR (0.0, geomsrv::engine::Dot (edge, normal), 1e-9);

    // Transformed as a direction instead, it would NOT be - which is the bug
    // this test exists to keep out.
    const geomsrv::engine::Vector3 wrong = geomsrv::engine::ApplyToDirection (transform, { 1.0, -1.0, 0.0 });
    EXPECT_GT (std::fabs (geomsrv::engine::Dot (edge, wrong)), 1e-6);
}

TEST (NodeGraphTransform, RotationTurnsAboutItsOwnOriginRatherThanTheWorlds)
{
    // Rotating a facade about the world origin instead of about its own corner
    // moves it a long way rather than a little, and the result still looks like
    // geometry - just somewhere else.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node rotate;
    rotate.id = "r";
    rotate.nodeType = "geom.rotate";
    rotate.parameters = { { "origin", Value (Point3 { 10.0, 0.0, 0.0 }) },
                          { "axis", Value (Point3 { 0.0, 0.0, 1.0 }) },
                          { "angle", Value (90.0) } };
    ValueMap inputs;
    inputs.emplace ("geometry", Value (Point3 { 11.0, 0.0, 0.0 }));
    ASSERT_TRUE (ExecuteRuntimeNode (rotate, inputs, context, outputs, error)) << error;

    const std::vector<Value>& moved = outputs.at ("geometry").Items ();
    ASSERT_EQ (1U, moved.size ());
    const Point3 point = std::get<Point3> (moved[0].DataValue ());
    // One metre out along +X from (10,0,0), turned a quarter turn, is one metre
    // out along +Y from the same place.
    EXPECT_NEAR (10.0, point.x, 1e-9);
    EXPECT_NEAR (1.0, point.y, 1e-9);
}

TEST (NodeGraphTransform, AnArrayCountsTheOriginalAsOneOfTheCopies)
{
    // A count of five means five things. The other reading puts every array one
    // bay too long, which reads as a modelling decision rather than an
    // off-by-one.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node array;
    array.id = "a";
    array.nodeType = "geom.arrayLinear";
    array.parameters = { { "step", Value (Point3 { 2.0, 0.0, 0.0 }) }, { "count", Value (static_cast<int64_t> (5)) } };
    ValueMap inputs;
    inputs.emplace ("geometry", Value (Point3 { 0.0, 0.0, 0.0 }));
    ASSERT_TRUE (ExecuteRuntimeNode (array, inputs, context, outputs, error)) << error;

    EXPECT_EQ (5, std::get<int64_t> (outputs.at ("count").DataValue ()));
    const std::vector<Value>& copies = outputs.at ("geometry").Items ();
    ASSERT_EQ (5U, copies.size ());
    // The first copy is the original, in place. A transformed point stays a
    // point - the array does not wrap each copy in a list of its own, which
    // would make every downstream node strip a level that carries no meaning.
    EXPECT_NEAR (0.0, std::get<Point3> (copies[0].DataValue ()).x, 1e-9);
    EXPECT_NEAR (8.0, std::get<Point3> (copies[4].DataValue ()).x, 1e-9);
}

// ---------------------------------------------------------------------------
// Curves
// ---------------------------------------------------------------------------

TEST (NodeGraphCurves, AHorizontalArcDoesNotCollapse)
{
    // ⚠️ THE CASE A FIXED SEED AXIS GETS WRONG. Building the arc's plane by
    // crossing its normal with a constant axis gives a zero-length result when
    // the normal IS that axis - which for a Z-up model is a horizontal arc, the
    // single commonest one there is. The arc then collapses to its centre point.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node arc;
    arc.id = "a";
    arc.nodeType = "geom.arc";
    arc.parameters = { { "centre", Value (Point3 { 0.0, 0.0, 0.0 }) },
                       { "normal", Value (Point3 { 0.0, 0.0, 1.0 }) },
                       { "radius", Value (2.0) },
                       { "start", Value (0.0) },
                       { "sweep", Value (360.0) },
                       { "segments", Value (static_cast<int64_t> (16)) } };
    ASSERT_TRUE (ExecuteRuntimeNode (arc, {}, context, outputs, error)) << error;

    const Polyline& curve = std::get<Polyline> (outputs.at ("curve").DataValue ());
    ASSERT_EQ (17U, curve.points.size ());
    for (const Point3& point : curve.points) {
        EXPECT_NEAR (2.0, std::sqrt (point.x * point.x + point.y * point.y), 1e-9);
        EXPECT_NEAR (0.0, point.z, 1e-9);
    }
}

TEST (NodeGraphCurves, CurvesAreSampledByLengthRatherThanByPointIndex)
{
    // "The midpoint of this curve" has to mean the same place whether the
    // polyline was drawn evenly or not. Indexing gives an answer that drifts
    // towards wherever the points happen to be dense - here, the short end.
    Polyline uneven;
    uneven.points = { Point3 { 0, 0, 0 }, Point3 { 1, 0, 0 }, Point3 { 2, 0, 0 }, Point3 { 10, 0, 0 } };

    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node pointOn;
    pointOn.id = "p";
    pointOn.nodeType = "geom.pointOnCurve";
    pointOn.parameters = { { "t", Value (0.5) } };
    ValueMap inputs;
    inputs.emplace ("curve", Value (uneven));
    ASSERT_TRUE (ExecuteRuntimeNode (pointOn, inputs, context, outputs, error)) << error;

    // Half of ten metres is five metres along, not the second of four points.
    EXPECT_NEAR (5.0, std::get<Point3> (outputs.at ("point").DataValue ()).x, 1e-9);

    Node divide;
    divide.id = "d";
    divide.nodeType = "geom.divideCurve";
    divide.parameters = { { "count", Value (static_cast<int64_t> (10)) }, { "includeEnds", Value (true) } };
    outputs.clear ();
    ASSERT_TRUE (ExecuteRuntimeNode (divide, inputs, context, outputs, error)) << error;
    // N segments is N+1 points. An off-by-one here puts every facade panel half
    // a bay out.
    EXPECT_EQ (11, std::get<int64_t> (outputs.at ("count").DataValue ()));
}

TEST (NodeGraphCurves, AConcaveOutlineExtrudesWithoutTrianglesOutsideIt)
{
    // ⚠️ THE REASON THE CAPS GO THROUGH A TRIANGULATOR. A fan from the first
    // vertex is only correct for a convex outline, and floor plates, cores and
    // L-shaped footprints are concave by default - a fan over one of those puts
    // triangles OUTSIDE the shape, which reads as the outline being wrong.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Polygon lShape;
    lShape.points = { Point3 { 0, 0, 0 }, Point3 { 4, 0, 0 }, Point3 { 4, 1, 0 },
                      Point3 { 1, 1, 0 }, Point3 { 1, 4, 0 }, Point3 { 0, 4, 0 } };

    Node extrude;
    extrude.id = "e";
    extrude.nodeType = "geom.extrude";
    extrude.parameters = { { "direction", Value (Point3 { 0.0, 0.0, 3.0 }) } };
    ValueMap inputs;
    inputs.emplace ("outline", Value (lShape));
    ASSERT_TRUE (ExecuteRuntimeNode (extrude, inputs, context, outputs, error)) << error;

    const auto mesh = std::get<Value::ImmutableMesh> (outputs.at ("mesh").DataValue ());
    ASSERT_NE (nullptr, mesh);
    EXPECT_GT (mesh->TriangleCount (), 0U);
    // The solid spans exactly the outline and the sweep, and nothing else. A fan
    // triangulation would still satisfy this - what it would break is the notch,
    // so the area is checked below rather than only the bounds.
    EXPECT_NEAR (0.0, mesh->bounds.mn[0], 1e-9);
    EXPECT_NEAR (4.0, mesh->bounds.mx[0], 1e-9);
    EXPECT_NEAR (0.0, mesh->bounds.mn[2], 1e-9);
    EXPECT_NEAR (3.0, mesh->bounds.mx[2], 1e-9);

    // The two caps together are twice the L's area of 7, not twice the area of
    // its convex hull. Summed as projected triangle areas in XY.
    double capArea = 0.0;
    for (std::size_t index = 0; index + 2 < mesh->triangles.size (); index += 3) {
        const uint32_t a = mesh->triangles[index];
        const uint32_t b = mesh->triangles[index + 1];
        const uint32_t c = mesh->triangles[index + 2];
        const double ax = mesh->vertices[a * 3], ay = mesh->vertices[a * 3 + 1], az = mesh->vertices[a * 3 + 2];
        const double bx = mesh->vertices[b * 3], by = mesh->vertices[b * 3 + 1], bz = mesh->vertices[b * 3 + 2];
        const double cx = mesh->vertices[c * 3], cy = mesh->vertices[c * 3 + 1], cz = mesh->vertices[c * 3 + 2];
        // Only the horizontal caps contribute; the walls are vertical.
        if (std::fabs (az - bz) > 1e-9 || std::fabs (az - cz) > 1e-9)
            continue;
        capArea += std::fabs ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
    }
    EXPECT_NEAR (14.0, capArea, 1e-6);
}

TEST (NodeGraphCurves, ALoftRefusesMismatchedCurvesRatherThanResamplingOne)
{
    // Resampling silently is the wrong favour: WHICH curve to resample onto
    // changes the shape, so a loft that quietly picked one would give an answer
    // nobody asked for and no way to see why.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Polyline bottom;
    bottom.points = { Point3 { 0, 0, 0 }, Point3 { 1, 0, 0 }, Point3 { 2, 0, 0 } };
    Polyline top;
    top.points = { Point3 { 0, 0, 1 }, Point3 { 2, 0, 1 } };

    Node loft;
    loft.id = "l";
    loft.nodeType = "geom.loft";
    ValueMap inputs;
    inputs.emplace ("from", Value (bottom));
    inputs.emplace ("to", Value (top));
    EXPECT_FALSE (ExecuteRuntimeNode (loft, inputs, context, outputs, error));
    EXPECT_NE (std::string::npos, error.find ("Divide Curve"));
}

// ---------------------------------------------------------------------------
// Numbers and flow
// ---------------------------------------------------------------------------

TEST (NodeGraphMath, RandomIsRepeatableFromItsSeed)
{
    // ⚠️ THE SOLUTION RUNS CONTINUOUSLY NOW. A Random that answered differently
    // on each evaluation would jitter the model on every keystroke anywhere
    // upstream, and nothing on screen would say why. The seed is what makes
    // "random" a value rather than an event.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    std::string error;

    Node random;
    random.id = "r";
    random.nodeType = "math.random";
    random.parameters = { { "seed", Value (static_cast<int64_t> (7)) },
                          { "count", Value (static_cast<int64_t> (5)) },
                          { "minimum", Value (0.0) },
                          { "maximum", Value (10.0) } };

    ValueMap first;
    ValueMap again;
    ASSERT_TRUE (ExecuteRuntimeNode (random, {}, context, first, error)) << error;
    ASSERT_TRUE (ExecuteRuntimeNode (random, {}, context, again, error)) << error;

    const std::vector<Value>& a = first.at ("values").Items ();
    const std::vector<Value>& b = again.at ("values").Items ();
    ASSERT_EQ (5U, a.size ());
    for (std::size_t index = 0; index < a.size (); ++index) {
        const double value = std::get<double> (a[index].DataValue ());
        EXPECT_EQ (value, std::get<double> (b[index].DataValue ()));
        EXPECT_GE (value, 0.0);
        EXPECT_LE (value, 10.0);
    }

    // A different seed is a different set, or the seed does nothing.
    Node other = random;
    other.parameters["seed"] = Value (static_cast<int64_t> (8));
    ValueMap different;
    ASSERT_TRUE (ExecuteRuntimeNode (other, {}, context, different, error)) << error;
    const std::vector<Value>& c = different.at ("values").Items ();
    EXPECT_NE (std::get<double> (a[0].DataValue ()), std::get<double> (c[0].DataValue ()));
}

TEST (NodeGraphMath, DivideAndRemapRefuseTheirDegenerateCasesRatherThanReturningInfinity)
{
    // An infinity propagates silently through every downstream node and surfaces
    // as geometry somewhere off in space; the node that produced it is then the
    // last place anyone looks.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    ValueMap outputs;
    std::string error;

    Node divide;
    divide.id = "d";
    divide.nodeType = "divide";
    ValueMap divideInputs;
    divideInputs.emplace ("left", Value (1.0));
    divideInputs.emplace ("right", Value (0.0));
    EXPECT_FALSE (ExecuteRuntimeNode (divide, divideInputs, context, outputs, error));

    Node remap;
    remap.id = "r";
    remap.nodeType = "math.remap";
    remap.parameters = { { "value", Value (0.5) },
                         { "sourceMin", Value (1.0) },
                         { "sourceMax", Value (1.0) },
                         { "targetMin", Value (0.0) },
                         { "targetMax", Value (10.0) } };
    outputs.clear ();
    error.clear ();
    EXPECT_FALSE (ExecuteRuntimeNode (remap, {}, context, outputs, error));

    // ... and the ordinary case works, with the clamp honoured.
    remap.parameters["sourceMax"] = Value (2.0);
    remap.parameters["value"] = Value (5.0);
    remap.parameters["clamp"] = Value (true);
    outputs.clear ();
    ASSERT_TRUE (ExecuteRuntimeNode (remap, {}, context, outputs, error)) << error;
    EXPECT_NEAR (10.0, std::get<double> (outputs.at ("value").DataValue ()), 1e-9);
}

TEST (NodeGraphFlowControl, IfPassesTheBranchItsConditionNames)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    NodeExecutionContext context;
    std::string error;

    Node conditional;
    conditional.id = "c";
    conditional.nodeType = "flow.if";
    ValueMap inputs;
    inputs.emplace ("ifTrue", Value (std::string ("yes")));
    inputs.emplace ("ifFalse", Value (std::string ("no")));

    for (const bool condition : { true, false }) {
        conditional.parameters["condition"] = Value (condition);
        ValueMap outputs;
        ASSERT_TRUE (ExecuteRuntimeNode (conditional, inputs, context, outputs, error)) << error;
        EXPECT_EQ (condition, std::get<bool> (outputs.at ("taken").DataValue ()));
        const std::vector<Value>& taken = outputs.at ("value").Items ();
        ASSERT_EQ (1U, taken.size ());
        EXPECT_EQ (condition ? "yes" : "no", std::get<std::string> (taken[0].DataValue ()));
    }
}
