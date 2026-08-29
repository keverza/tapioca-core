#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/BuiltinNodes.hpp"
#include "NodeGraph/EvaluationPlan.hpp"
#include "NodeGraph/FaultBarrier.hpp"
#include "NodeGraph/GraphEdit.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"

#include <gtest/gtest.h>

#include <map>
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
    const NodeExecutor executor = [&executions] (const Node& node, const ValueMap& inputs, ValueMap& outputs,
                                                 std::string&) {
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

TEST (NodeGraphBuiltins, CatalogHasSixSchemaDrivenPureNodes)
{
    const NodeRegistry registry = MakeBuiltinNodeRegistry ();
    EXPECT_EQ (6U, registry.Types ().size ());
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
    const NodeExecutor succeeds = [] (const Node& node, const ValueMap&, ValueMap& outputs, std::string&) {
        outputs.emplace ("value", node.parameters.at ("value"));
        return true;
    };
    ASSERT_TRUE (RunGraph (evaluator, graph, registry, succeeds).succeeded);
    const auto first = evaluator.Result ("one");
    evaluator.Invalidate (graph, { "one" });
    const NodeExecutor fails = [] (const Node&, const ValueMap&, ValueMap&, std::string& nodeError) {
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
    const NodeExecutor executor = [&executions] (const Node& node, const ValueMap& inputs, ValueMap& outputs,
                                                 std::string&) {
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
    const NodeExecutor executor = [&context] (const Node& node, const ValueMap& inputs, ValueMap& outputs,
                                              std::string&) {
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
    const NodeExecutor executor = [] (const Node& node, const ValueMap&, ValueMap& outputs, std::string&) -> bool {
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
    const NodeExecutor executor = [&] (const Node& node, const ValueMap&, ValueMap& outputs, std::string&) {
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
    const NodeExecutor wide = [] (const Node&, const ValueMap&, ValueMap& outputs, std::string&) {
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

    const NodeExecutor deep = [] (const Node&, const ValueMap&, ValueMap& outputs, std::string&) {
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
    const NodeExecutor executor = [] (const Node& node, const ValueMap&, ValueMap& outputs, std::string& nodeError) {
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
    const NodeExecutor executor = [] (const Node& node, const ValueMap&, ValueMap& outputs, std::string&) {
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
