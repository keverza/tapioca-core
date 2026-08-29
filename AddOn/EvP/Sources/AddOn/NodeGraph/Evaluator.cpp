#include "NodeGraph/Evaluator.hpp"

#include "NodeGraph/FaultBarrier.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <chrono>
#include <set>
#include <utility>

namespace evp::nodegraph {
namespace {

void CombineHash (size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

void CombineText (size_t& seed, const std::string& text)
{
    CombineHash (seed, std::hash<std::string> {}(text));
}

// Sets running_ for the duration of one evaluation and clears it on every exit
// path, including a thrown one. Reentrancy is rejected by the caller testing the
// flag before constructing this.
class RunGuard {
  public:
    explicit RunGuard (std::atomic<bool>& flag) : flag_ (flag)
    {
    }
    ~RunGuard ()
    {
        flag_.store (false, std::memory_order_relaxed);
    }
    RunGuard (const RunGuard&) = delete;
    RunGuard& operator= (const RunGuard&) = delete;

  private:
    std::atomic<bool>& flag_;
};

// Emitting through the context's sink rather than through a member keeps the
// evaluator ignorant of the recorder: with no sink, a run is simply unobserved.
void Emit (const RunContext& context, RunEventKind kind, const NodeId& nodeId, const std::string& message = {})
{
    if (!context.events)
        return;
    RunEvent event;
    event.kind = kind;
    event.runId = context.runId;
    event.graphRevision = context.graphRevision;
    event.nodeId = nodeId;
    event.message = message;
    context.events (std::move (event));
}

void EmitRunFinished (const RunContext& context, const EvaluationOutcome& outcome, RunEventKind kind)
{
    if (!context.events)
        return;
    RunEvent event;
    event.kind = kind;
    event.runId = context.runId;
    event.graphRevision = context.graphRevision;
    event.message = outcome.error;
    event.plannedCount = outcome.plannedNodes.size ();
    event.executedCount = outcome.executedCount;
    event.cacheHitCount = outcome.cacheHitCount;
    event.failedCount = outcome.failedCount;
    event.blockedCount = outcome.blockedCount;
    context.events (std::move (event));
}

} // namespace

void Evaluator::Invalidate (const GraphDocument& document, const std::vector<NodeId>& roots)
{
    for (const NodeId& nodeId : DownstreamClosure (document, roots)) {
        CacheEntry& entry = cache_[nodeId];
        entry.dirty = true;
        entry.status.state = NodeExecutionState::Dirty;
        entry.status.message.clear ();
        entry.status.cacheHit = false;
    }
}

void Evaluator::BlockDownstream (const GraphDocument& document, const NodeId& origin, const std::string& reason,
                                 const RunContext& context, size_t& blockedCount)
{
    for (const NodeId& nodeId : DownstreamClosure (document, { origin })) {
        if (nodeId == origin)
            continue;
        CacheEntry& entry = cache_[nodeId];
        entry.status.state = NodeExecutionState::Blocked;
        entry.status.message = reason;
        entry.status.cacheHit = false;
        entry.status.runId = context.runId;
        entry.dirty = true;
        ++blockedCount;
        Emit (context, RunEventKind::NodeBlocked, nodeId, reason);
    }
}

EvaluationOutcome Evaluator::Evaluate (const GraphDocument& document, const NodeRegistry& registry,
                                       const NodeExecutor& executor, const EvaluationRequest& request,
                                       const RunContext& context)
{
    EvaluationOutcome outcome;
    outcome.runId = context.runId;
    outcome.graphRevision = document.Revision ();

    // Rule 6: an evaluation started from inside an evaluation is refused. Two
    // runs mutating one cache is the failure that produces results belonging to
    // neither of them.
    bool expected = false;
    if (!running_.compare_exchange_strong (expected, true, std::memory_order_acq_rel)) {
        outcome.error = "an evaluation is already running";
        return outcome;
    }
    const RunGuard guard (running_);

    std::erase_if (cache_, [&document] (const auto& item) { return document.FindNode (item.first) == nullptr; });
    for (const auto& [nodeId, node] : document.Nodes ()) {
        (void) node;
        cache_.try_emplace (nodeId);
    }

    const PlanOutcome planned = BuildEvaluationPlan (document, registry, request, context);
    if (!planned.accepted) {
        outcome.error = planned.error;
        outcome.cyclicNodes = planned.cyclicNodes;
        if (context.events) {
            RunEvent started;
            started.kind = RunEventKind::RunStarted;
            started.runId = context.runId;
            started.graphRevision = context.graphRevision;
            context.events (RunEvent { started });
            RunEvent completed = started;
            completed.kind = RunEventKind::RunCompleted;
            completed.failedCount = 1;
            completed.message = planned.error;
            context.events (std::move (completed));
        }
        return outcome;
    }
    const EvaluationPlan& plan = planned.plan;
    outcome.plannedNodes = plan.requiredNodes;

    if (context.events) {
        RunEvent started;
        started.kind = RunEventKind::RunStarted;
        started.runId = context.runId;
        started.graphRevision = context.graphRevision;
        started.plannedCount = plan.requiredNodes.size ();
        context.events (std::move (started));
        for (const NodeId& nodeId : plan.requiredNodes)
            Emit (context, RunEventKind::NodeQueued, nodeId);
    }

    std::set<NodeId> unusable;
    outcome.succeeded = true;

    for (const NodeId& nodeId : plan.requiredNodes) {
        if (context.cancellation.IsCancelled ()) {
            // Rule 7: cancelling degrades nothing. Nodes that already finished
            // keep their results; only what did not run is marked cancelled.
            outcome.cancelled = true;
            outcome.succeeded = false;
            outcome.error = "the evaluation was cancelled";
            for (const NodeId& remaining : plan.requiredNodes) {
                CacheEntry& entry = cache_[remaining];
                if (entry.status.runId != context.runId) {
                    entry.status.state = NodeExecutionState::Cancelled;
                    entry.status.message = "the evaluation was cancelled";
                    entry.status.runId = context.runId;
                    Emit (context, RunEventKind::NodeCancelled, remaining);
                }
            }
            EmitRunFinished (context, outcome, RunEventKind::RunCancelled);
            return outcome;
        }

        if (unusable.contains (nodeId))
            continue;

        const Node& node = *document.FindNode (nodeId);
        const NodeType& nodeType = *registry.Find (node.nodeType);

        // Cache key: node identity and parameters by value, upstream results by
        // OUTPUT REVISION rather than by content. A mesh is never rehashed to
        // decide whether its consumer is still clean.
        size_t inputHash = std::hash<std::string> {}(node.nodeType);
        for (const auto& [parameterId, value] : node.parameters) {
            CombineText (inputHash, parameterId);
            CombineHash (inputHash, value.Hash ());
        }
        for (const ParameterSchema& parameter : nodeType.parameters) {
            if (!node.parameters.contains (parameter.id) && parameter.defaultValue) {
                CombineText (inputHash, parameter.id);
                CombineHash (inputHash, parameter.defaultValue->Hash ());
            }
        }

        ValueMap inputs;
        bool inputsResolved = true;
        for (const PortSchema& input : nodeType.inputs) {
            Value::List values;
            for (const Edge& edge : document.Edges ()) {
                if (edge.targetNode != nodeId || edge.targetPort != input.id)
                    continue;
                const auto source = cache_.find (edge.sourceNode);
                const std::shared_ptr<const NodeResult> sourceResult =
                    source == cache_.end () ? nullptr : source->second.result;
                if (!sourceResult || !sourceResult->outputs.contains (edge.sourcePort)) {
                    BlockDownstream (document, nodeId,
                                     "upstream output is absent: " + edge.sourceNode + "." + edge.sourcePort, context,
                                     outcome.blockedCount);
                    CacheEntry& failing = cache_[nodeId];
                    failing.status.state = NodeExecutionState::Failed;
                    failing.status.message = "upstream output is absent: " + edge.sourceNode + "." + edge.sourcePort;
                    failing.status.runId = context.runId;
                    ++outcome.failedCount;
                    if (outcome.error.empty ()) {
                        outcome.error = failing.status.message;
                        outcome.failedNode = nodeId;
                    }
                    outcome.succeeded = false;
                    inputsResolved = false;
                    Emit (context, RunEventKind::NodeFailed, nodeId, failing.status.message);
                    break;
                }
                values.push_back (sourceResult->outputs.at (edge.sourcePort));
                CombineText (inputHash, edge.sourceNode);
                CombineText (inputHash, edge.sourcePort);
                CombineHash (inputHash, static_cast<size_t> (sourceResult->outputRevision));
            }
            if (!inputsResolved)
                break;

            if (values.empty ()) {
                if (input.required) {
                    BlockDownstream (document, nodeId, "required input is unconnected: " + input.id, context,
                                     outcome.blockedCount);
                    CacheEntry& failing = cache_[nodeId];
                    failing.status.state = NodeExecutionState::Failed;
                    failing.status.message = "required input is unconnected: " + input.id;
                    failing.status.runId = context.runId;
                    ++outcome.failedCount;
                    if (outcome.error.empty ()) {
                        outcome.error = failing.status.message;
                        outcome.failedNode = nodeId;
                    }
                    outcome.succeeded = false;
                    inputsResolved = false;
                    Emit (context, RunEventKind::NodeFailed, nodeId, failing.status.message);
                    break;
                }
                inputs.emplace (input.id, Value {});
            }
            else if (input.acceptsMultiple) {
                inputs.emplace (input.id, Value (std::move (values)));
            }
            else {
                inputs.emplace (input.id, std::move (values.front ()));
            }
        }

        if (!inputsResolved) {
            for (const NodeId& blocked : DownstreamClosure (document, { nodeId }))
                unusable.insert (blocked);
            continue;
        }

        CacheEntry& entry = cache_[nodeId];
        if (plan.mode == EvaluationMode::Incremental && entry.result && !entry.dirty && entry.inputHash == inputHash) {
            entry.status.state = NodeExecutionState::Complete;
            entry.status.message.clear ();
            entry.status.cacheHit = true;
            entry.status.runId = context.runId;
            ++outcome.cacheHitCount;
            if (context.events) {
                RunEvent hit;
                hit.kind = RunEventKind::NodeCacheHit;
                hit.runId = context.runId;
                hit.graphRevision = context.graphRevision;
                hit.nodeId = nodeId;
                hit.itemCount = entry.status.itemCount;
                context.events (std::move (hit));
            }
            continue;
        }

        Node effectiveNode = node;
        for (const ParameterSchema& parameter : nodeType.parameters) {
            if (!effectiveNode.parameters.contains (parameter.id) && parameter.defaultValue)
                effectiveNode.parameters.emplace (parameter.id, *parameter.defaultValue);
        }

        ValueMap outputs;
        std::string nodeError;
        Emit (context, RunEventKind::NodeStarted, nodeId);
        const auto started = std::chrono::steady_clock::now ();
        // Rule 2: the node body runs behind the fault barrier, so a structured
        // exception in node code fails the node instead of the process.
        const GuardOutcome guarded = RunGuarded ([&executor, &effectiveNode, &inputs, &outputs, &nodeError] () {
            return executor (effectiveNode, inputs, outputs, nodeError);
        });
        const double elapsedMs =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();

        std::string failure;
        if (!guarded.completed)
            failure = guarded.fault;
        else if (!guarded.result)
            failure = nodeError.empty () ? "the node reported failure without a message" : nodeError;
        else if (elapsedMs > context.limits.nodeBudgetMs)
            failure = "the node exceeded its time budget";

        size_t itemCount = 0;
        if (failure.empty ()) {
            for (const auto& [portId, value] : outputs) {
                const PortSchema* output = FindOutput (nodeType, portId);
                if (output == nullptr || output->valueType != value.Type ()) {
                    failure = "invalid output: " + portId;
                    break;
                }
                // Rule 5: an oversized or pathologically nested result fails its
                // node rather than entering the cache.
                const ValueMeasure measure =
                    MeasureValue (value, context.limits.maxOutputItems - itemCount, context.limits.maxValueDepth);
                if (!measure.WithinLimits ()) {
                    failure = measure.exceededDepth ? "output '" + portId + "' nests too deeply"
                                                    : "output '" + portId + "' exceeds the output ceiling";
                    break;
                }
                itemCount += measure.items;
            }
        }
        if (failure.empty ()) {
            for (const PortSchema& output : nodeType.outputs) {
                if (output.required && !outputs.contains (output.id)) {
                    failure = "omitted output: " + output.id;
                    break;
                }
            }
        }

        if (!failure.empty ()) {
            entry.dirty = true;
            entry.status.state = NodeExecutionState::Failed;
            entry.status.message = failure;
            entry.status.cacheHit = false;
            entry.status.durationMilliseconds = elapsedMs;
            entry.status.runId = context.runId;
            ++entry.status.evaluationCount;
            ++outcome.failedCount;
            if (outcome.error.empty ()) {
                outcome.error = "node " + nodeId + " failed: " + failure;
                outcome.failedNode = nodeId;
            }
            outcome.succeeded = false;
            if (context.events) {
                RunEvent failed;
                failed.kind = RunEventKind::NodeFailed;
                failed.runId = context.runId;
                failed.graphRevision = context.graphRevision;
                failed.nodeId = nodeId;
                failed.message = failure;
                failed.durationMilliseconds = elapsedMs;
                context.events (std::move (failed));
            }
            BlockDownstream (document, nodeId, "upstream node failed: " + nodeId, context, outcome.blockedCount);
            for (const NodeId& blocked : DownstreamClosure (document, { nodeId }))
                unusable.insert (blocked);
            continue;
        }

        entry.inputHash = inputHash;
        entry.result = std::make_shared<const NodeResult> (
            NodeResult { std::move (outputs), elapsedMs, nextOutputRevision_++, itemCount });
        entry.dirty = false;
        entry.status.state = NodeExecutionState::Complete;
        entry.status.message.clear ();
        entry.status.cacheHit = false;
        entry.status.durationMilliseconds = elapsedMs;
        entry.status.itemCount = itemCount;
        entry.status.runId = context.runId;
        ++entry.status.evaluationCount;
        ++outcome.executedCount;
        if (context.events) {
            RunEvent completed;
            completed.kind = RunEventKind::NodeCompleted;
            completed.runId = context.runId;
            completed.graphRevision = context.graphRevision;
            completed.nodeId = nodeId;
            completed.durationMilliseconds = elapsedMs;
            completed.itemCount = itemCount;
            context.events (std::move (completed));
        }
    }

    EmitRunFinished (context, outcome, RunEventKind::RunCompleted);
    return outcome;
}

NodeStatus Evaluator::Status (const NodeId& nodeId) const
{
    const auto iterator = cache_.find (nodeId);
    return iterator == cache_.end () ? NodeStatus {} : iterator->second.status;
}

std::shared_ptr<const NodeResult> Evaluator::Result (const NodeId& nodeId) const
{
    const auto iterator = cache_.find (nodeId);
    return iterator == cache_.end () ? nullptr : iterator->second.result;
}

bool Evaluator::IsDirty (const NodeId& nodeId) const
{
    const auto iterator = cache_.find (nodeId);
    return iterator == cache_.end () || iterator->second.dirty;
}

} // namespace evp::nodegraph
