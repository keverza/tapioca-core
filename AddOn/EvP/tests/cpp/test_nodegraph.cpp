#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/BuiltinNodes.hpp"
#include "NodeGraph/EvaluationPlan.hpp"
#include "NodeGraph/FaultBarrier.hpp"
#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/GraphReports.hpp"
#include "NodeGraph/GraphRuntimeState.hpp"
#include "NodeGraph/NodeExecution.hpp"
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

int64_t Integer (const Value& value)
{
    return std::get<int64_t> (value.DataValue ());
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
    EXPECT_EQ (3, Integer (evaluator.Result ("sum")->outputs.at ("sum")));

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
    EXPECT_EQ (7, Integer (evaluator.Result ("sum")->outputs.at ("sum")));
}

TEST (NodeGraphValue, HoldsRecursiveListsAndImmutableMeshes)
{
    auto mesh = std::make_shared<geomsrv::Mesh> ();
    mesh->guid = "guid";
    const Value value (Value::List { Value (true), Value (Point3 { 1.0, 2.0, 3.0 }),
                                     Value (Value::ImmutableMesh (mesh)),
                                     Value (ArchicadElementRef { "element-guid" }) });
    EXPECT_EQ (ValueType::List, value.Type ());
    EXPECT_NE (0U, value.Hash ());
    EXPECT_EQ (4U, std::get<Value::List> (value.DataValue ()).size ());
}

TEST (NodeGraphBuiltins, CatalogIsSchemaDrivenAndPure)
{
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
    // Counted by CATEGORY rather than as one number, because the total is now a
    // library that grows: a bare count told you a node had been added and
    // nothing about whether it landed where a user would look for it.
    std::map<std::string, size_t> byCategory;
    for (const auto& [id, nodeType] : registry.Types ())
        ++byCategory[nodeType.category];
    EXPECT_EQ (6U, byCategory["Core"]);      // number, add, multiply, makeList, scaleList, watch
    EXPECT_EQ (1U, byCategory["Inspect"]);   // panel
    EXPECT_EQ (1U, byCategory["Flow"]);      // dataDam - Stage F, so Holding is reachable
    EXPECT_EQ (2U, byCategory["Input"]);     // numberSlider, booleanToggle
    EXPECT_EQ (8U, byCategory["Archicad"]);  // one attribute picker per domain
    EXPECT_EQ (11U, byCategory["Geometry"]); // inputs, GLM vectors and Clipper2 polygons
    EXPECT_EQ (29U, registry.Types ().size ());
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
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    EXPECT_DOUBLE_EQ (42.0, std::get<double> (evaluator.Result ("product")->outputs.at ("value").DataValue ()));

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
    EXPECT_DOUBLE_EQ (70.0, std::get<double> (wired.Result ("product")->outputs.at ("value").DataValue ()));
}

TEST (NodeGraphBuiltins, EvaluatesArithmeticListMapAndWatchWorkflow)
{
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    const Value::List& values = std::get<Value::List> (evaluator.Result ("watch")->outputs.at ("value").DataValue ());
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
    EXPECT_EQ (1, Integer (evaluator.Result ("a")->outputs.at ("value")));
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

TEST (NodeGraphContainment, OversizedAndOverdeepOutputsFailTheirNode)
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
        Value::List list;
        for (int i = 0; i < 64; ++i)
            list.emplace_back (static_cast<int64_t> (i));
        outputs.emplace ("value", Value (std::move (list)));
        return true;
    };

    RunContext context;
    context.runId = 1;
    context.limits.maxOutputItems = 8;
    EvaluationOutcome outcome = evaluator.Evaluate (graph, registry, wide, EvaluationRequest {}, context);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, evaluator.Status ("big").message.find ("output ceiling"));

    const NodeExecutor deep = [] (const Node&, const ValueMap&, const NodeExecutionContext&, ValueMap& outputs,
                                  std::string&) {
        Value nested (int64_t { 0 });
        for (int i = 0; i < 40; ++i)
            nested = Value (Value::List { nested });
        outputs.emplace ("value", Value (Value::List { nested }));
        return true;
    };
    context.runId = 2;
    context.limits.maxOutputItems = 1000;
    context.limits.maxValueDepth = 8;
    outcome = evaluator.Evaluate (graph, registry, deep, EvaluationRequest {}, context);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, evaluator.Status ("big").message.find ("nests too deeply"));
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
};

// A selection-set node already holding `guids`. The set is an ordinary
// parameter, so seeding one is an ordinary node with a parameter - which is the
// simplification the node's redesign bought.
Node SelectionSetNode (const std::string& nodeId, std::initializer_list<std::string> guids)
{
    Value::List elements;
    for (const std::string& guid : guids)
        elements.emplace_back (ArchicadElementRef { guid });
    Node node { nodeId, "archicad.getSelection" };
    node.parameters.emplace ("elements", Value (std::move (elements)));
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
    EXPECT_EQ (2, std::get<int64_t> (result->outputs.at ("count").DataValue ()));
    const Value::List& elements = std::get<Value::List> (result->outputs.at ("elements").DataValue ());
    ASSERT_EQ (2U, elements.size ());
    EXPECT_EQ ("guid-a", std::get<ArchicadElementRef> (elements[0].DataValue ()).guid);
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
    EXPECT_EQ (1, std::get<int64_t> (evaluator.Result ("sel")->outputs.at ("count").DataValue ()));
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

TEST (NodeGraphPanel, RendersAListOneItemPerLineAndAlsoAsOneString)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "panel", "panel" } } }).accepted);

    Node two { "two", "number" };
    two.parameters.emplace ("value", Value (2.0));
    Node three { "three", "number" };
    three.parameters.emplace ("value", Value (3.0));
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { two } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { three } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "list", "makeList" } } }).accepted);
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("two", "value", "list", "items") } }).accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("three", "value", "list", "items") } })
                     .accepted);
    ASSERT_TRUE (ApplyEdit (graph, registry, GraphEdit { ConnectEdit { Connect ("list", "value", "panel", "value") } })
                     .accepted);

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunGraph (evaluator, graph, registry, ExecuteRuntimeNode, { "panel" });
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("panel");
    ASSERT_NE (nullptr, result);
    EXPECT_EQ ("2\n3", std::get<std::string> (result->outputs.at ("text").DataValue ()));
    EXPECT_EQ ("List of 2", std::get<std::string> (result->outputs.at ("summary").DataValue ()));
    EXPECT_EQ (2, std::get<int64_t> (result->outputs.at ("count").DataValue ()));
    const Value::List& lines = std::get<Value::List> (result->outputs.at ("lines").DataValue ());
    ASSERT_EQ (2U, lines.size ());
    EXPECT_EQ ("2", std::get<std::string> (lines[0].DataValue ()));
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
    EXPECT_EQ ("[1, 2]", FormatValue (Value (Value::List { Value (1.0), Value (2.0) })));

    auto mesh = std::make_shared<geomsrv::Mesh> ();
    mesh->vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh->triangles = { 0, 1, 2 };
    EXPECT_EQ ("Mesh (3 vertices, 1 triangles)", FormatValue (Value (Value::ImmutableMesh (mesh))));

    // An empty list says so rather than rendering as nothing at all.
    EXPECT_EQ ((std::vector<std::string> { "(empty list)" }), FormatValueLines (Value (Value::List {})));
}

TEST (NodeGraphPanel, TruncatesLargeAndDeepValuesAndSaysThatItDid)
{
    Value::List big;
    for (int i = 0; i < 40; ++i)
        big.emplace_back (static_cast<int64_t> (i));

    const std::vector<std::string> lines = FormatValueLines (Value (big), 10);
    ASSERT_EQ (10U, lines.size ());
    // A quietly shortened list reads as a wrong answer, so the last line says
    // how many were left out.
    EXPECT_NE (std::string::npos, lines.back ().find ("more of 40"));

    // Inline rendering caps items too.
    EXPECT_NE (std::string::npos, FormatValue (Value (big)).find ("more"));

    // And depth: past the limit a nested list renders as its shape.
    Value nested (int64_t { 0 });
    for (int i = 0; i < 8; ++i)
        nested = Value (Value::List { nested });
    EXPECT_NE (std::string::npos, FormatValue (nested).find ("items]"));
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
    EXPECT_EQ (3, Integer (evaluator.Result ("sum")->outputs.at ("sum")));
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
    EXPECT_EQ (12, Integer (evaluator.Result ("sum")->outputs.at ("sum")));
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
    node.parameters.emplace ("items", Value (Value::List { Value (int64_t { 1 }), Value (std::string ("two")) }));
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
    EXPECT_EQ (2U, std::get<Value::List> (reloaded.parameters.at ("items").DataValue ()).size ());

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
    EXPECT_EQ (10, Integer (evaluator.Result ("sum")->outputs.at ("sum")));
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
    NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    return std::get<double> (result->outputs.at (portId).DataValue ());
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

    // `panel` accepts any type and emits four unrelated ones, so no mapping is
    // derivable. The refusal IS the feature: guessing would make bypass mean
    // something different on every node it touched.
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
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
    Node node { "s", "numberSlider" };
    node.parameters.emplace ("value", Value (999.0));
    node.parameters.emplace ("minimum", Value (0.0));
    node.parameters.emplace ("maximum", Value (10.0));
    node.parameters.emplace ("decimals", Value (static_cast<int64_t> (2)));

    ValueMap outputs;
    std::string error;
    NodeExecutionContext context;
    ASSERT_TRUE (ExecuteBuiltinNode (node, {}, context, outputs, error)) << error;
    EXPECT_DOUBLE_EQ (10.0, std::get<double> (outputs.at ("value").DataValue ()));

    Node rounded { "r", "numberSlider" };
    rounded.parameters.emplace ("value", Value (1.23456));
    rounded.parameters.emplace ("minimum", Value (0.0));
    rounded.parameters.emplace ("maximum", Value (10.0));
    rounded.parameters.emplace ("decimals", Value (static_cast<int64_t> (2)));
    ValueMap roundedOutputs;
    ASSERT_TRUE (ExecuteBuiltinNode (rounded, {}, context, roundedOutputs, error)) << error;
    EXPECT_DOUBLE_EQ (1.23, std::get<double> (roundedOutputs.at ("value").DataValue ()));
}

TEST (NodeGraphInputLibrary, APointAssemblesIndependentlyWiredCoordinates)
{
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    ASSERT_TRUE (ExecuteBuiltinNode (node, {}, context, typedOutputs, error)) << error;
    EXPECT_DOUBLE_EQ (2.0, std::get<Point3> (typedOutputs.at ("point").DataValue ()).y);

    ValueMap inputs;
    inputs.emplace ("y", Value (8.0));
    ValueMap wiredOutputs;
    ASSERT_TRUE (ExecuteBuiltinNode (node, inputs, context, wiredOutputs, error)) << error;
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
    ASSERT_TRUE (ExecuteBuiltinNode (node, {}, context, outputs, error)) << error;
    EXPECT_DOUBLE_EQ (5.0, std::get<double> (outputs.at ("length").DataValue ()));
}

TEST (NodeGraphGeometry, VectorAndPolygonNodesUseTheGeometryEngine)
{
    NodeExecutionContext context;
    std::string error;

    Node cross { "cross", "geom.vectorCross" };
    ValueMap crossOutputs;
    ASSERT_TRUE (ExecuteBuiltinNode (
        cross, { { "left", Value (Point3 { 1.0, 0.0, 0.0 }) }, { "right", Value (Point3 { 0.0, 1.0, 0.0 }) } }, context,
        crossOutputs, error))
        << error;
    EXPECT_DOUBLE_EQ (1.0, std::get<Point3> (crossOutputs.at ("vector").DataValue ()).z);

    Polygon subject { { { 0.0, 0.0, 0.0 }, { 2.0, 0.0, 0.0 }, { 2.0, 2.0, 0.0 }, { 0.0, 2.0, 0.0 } } };
    Polygon clip { { { 1.0, 0.0, 0.0 }, { 3.0, 0.0, 0.0 }, { 3.0, 2.0, 0.0 }, { 1.0, 2.0, 0.0 } } };
    Node unite { "union", "geom.polygonUnion" };
    ValueMap unionOutputs;
    ASSERT_TRUE (ExecuteBuiltinNode (unite, { { "subject", Value (subject) }, { "clip", Value (clip) } }, context,
                                     unionOutputs, error))
        << error;
    const Value::List& polygons = std::get<Value::List> (unionOutputs.at ("polygons").DataValue ());
    ASSERT_EQ (1U, polygons.size ());
    EXPECT_EQ (ValueType::Polygon, polygons.front ().Type ());
}

TEST (NodeGraphInputLibrary, EveryAttributePickerIsPureAndAnswersWithWhatWasPicked)
{
    // The pickers must not need Archicad to EVALUATE. What needs Archicad is
    // listing the choices, and that is a separate native verb - so a graph
    // carrying a layer name still runs with no project open.
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
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
    ASSERT_TRUE (ExecuteBuiltinNode (node, {}, context, outputs, error)) << error;
    EXPECT_EQ ("Existing Structures", std::get<std::string> (outputs.at ("value").DataValue ()));
}
