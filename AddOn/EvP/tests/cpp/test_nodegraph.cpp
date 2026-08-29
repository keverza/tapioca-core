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

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
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

    std::map<std::string, int> executions;
    const NodeExecutor executor = [&executions] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                                 ValueMap& outputs, std::string&) {
        ++executions[node.id];
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

TEST (NodeGraphBuiltins, CatalogHasSevenSchemaDrivenPureNodes)
{
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
    EXPECT_EQ (7U, registry.Types ().size ());
    EXPECT_EQ (ExecutionDomain::Worker, registry.Find ("scaleList")->executionDomain);
    EXPECT_EQ (ValueType::List, registry.Find ("watch")->outputs.front ().valueType);
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
    EXPECT_EQ (NodeExecutionState::Failed, evaluator.Status ("one").state);
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

    std::map<std::string, int> executions;
    const NodeExecutor executor = [&executions] (const Node& node, const ValueMap& inputs, const NodeExecutionContext&,
                                                 ValueMap& outputs, std::string&) {
        ++executions[node.id];
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
    EXPECT_EQ (NodeExecutionState::Complete, evaluator.Status ("a").state);
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
    EXPECT_EQ (NodeExecutionState::Failed, evaluator.Status ("a").state);
    EXPECT_NE (std::string::npos, evaluator.Status ("a").message.find ("node exploded"));
    // The independent branch still finished: a failure is scoped, not global.
    EXPECT_EQ (NodeExecutionState::Complete, evaluator.Status ("b").state);
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
    EXPECT_EQ (NodeExecutionState::Failed, evaluator.Status ("a").state);
    EXPECT_EQ (NodeExecutionState::Blocked, evaluator.Status ("sum").state);
    EXPECT_NE (std::string::npos, evaluator.Status ("sum").message.find ("upstream node failed: a"));
    // b is independent of the failure and must not be collateral damage.
    EXPECT_EQ (NodeExecutionState::Complete, evaluator.Status ("b").state);
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

TEST (NodeGraphArchicad, GetSelectionReturnsWhatTheHostHolds)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

    StubHost host;
    host.Holds ({ "guid-a", "guid-b" });

    Evaluator evaluator;
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;

    const std::shared_ptr<const NodeResult> result = evaluator.Result ("sel");
    ASSERT_NE (nullptr, result);
    EXPECT_EQ (2, std::get<int64_t> (result->outputs.at ("count").DataValue ()));
    const Value::List& elements = std::get<Value::List> (result->outputs.at ("elements").DataValue ());
    ASSERT_EQ (2U, elements.size ());
    EXPECT_EQ ("guid-a", std::get<ArchicadElementRef> (elements[0].DataValue ()).guid);
}

TEST (NodeGraphArchicad, ChangingTheSelectionReRunsTheNodeAndNotChangingItDoesNot)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

    StubHost host;
    host.Holds ({ "guid-a" });

    Evaluator evaluator;
    EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 1);
    ASSERT_TRUE (outcome.succeeded) << outcome.error;
    EXPECT_EQ (1U, outcome.executedCount);

    // Nothing changed: the declared generation is identical, so this is a cache
    // hit rather than a second read of the host.
    outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 2);
    EXPECT_EQ (0U, outcome.executedCount);
    EXPECT_EQ (1U, outcome.cacheHitCount);

    // The user selected something else. Without the declared generation this
    // node would serve the old answer forever - which is the whole point of
    // declaring it.
    host.Holds ({ "guid-a", "guid-c" });
    host.generationSource.selection = 2;
    outcome = RunWithHost (evaluator, graph, registry, ExecuteRuntimeNode, &host, false, 3);
    EXPECT_EQ (1U, outcome.executedCount);
    EXPECT_EQ (0U, outcome.cacheHitCount);
    EXPECT_EQ (2, std::get<int64_t> (evaluator.Result ("sel")->outputs.at ("count").DataValue ()));
}

TEST (NodeGraphArchicad, AGraphNeedingArchicadIsRefusedAtTheDoorWithoutAHost)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    GraphDocument graph;
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

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
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);
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
    EXPECT_EQ (NodeExecutionState::Skipped, evaluator.Status ("set").state);
    EXPECT_EQ (NodeExecutionState::Complete, evaluator.Status ("sel").state);

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
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);
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
    host.resolver.present.erase ("guid-b");
    host.generationSource.selection = 2;
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
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);
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
    ASSERT_TRUE (
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

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
        ApplyEdit (graph, registry, GraphEdit { AddNodeEdit { Node { "sel", "archicad.getSelection" } } }).accepted);

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
    const EvaluationOutcome outcome = RunWithHost (evaluator, graph, registry, closing, &host);
    EXPECT_FALSE (outcome.succeeded);
    EXPECT_NE (std::string::npos, evaluator.Status ("sel").message.find ("no longer available"));
}

// --- The Panel node --------------------------------------------------------

TEST (NodeGraphPanel, AcceptsAnyValueTypeThroughOneWildcardInput)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();

    // One panel, three source types. Without the wildcard input this would need
    // a panel variant per value type.
    for (const char* source : { "number", "makeList", "archicad.getSelection" }) {
        GraphDocument graph;
        Node sourceNode { "src", source };
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
