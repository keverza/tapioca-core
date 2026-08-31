#include "NodeGraph/Evaluator.hpp"

#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/FaultBarrier.hpp"
#include "NodeGraph/GraphAlgorithms.hpp"
#include "NodeGraph/NodeRegistry.hpp"
#include "NodeGraph/WorkerPool.hpp"

#include <algorithm>
#include <atomic>
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

// One node per level: the deferred phase run through the same level machinery
// as the primary one. Effectful nodes are serialised by contract, so this is not
// a fallback path but the sequential shape of the one code path - the
// alternative, a second loop, is exactly the drift the plan warns about.
std::vector<std::vector<NodeId>> SingletonLevels (const std::vector<NodeId>& nodes)
{
    std::vector<std::vector<NodeId>> levels;
    levels.reserve (nodes.size ());
    for (const NodeId& nodeId : nodes)
        levels.push_back ({ nodeId });
    return levels;
}

const UnavailableReferenceResolver& OfflineResolver ()
{
    static const UnavailableReferenceResolver resolver;
    return resolver;
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

// One node, gathered on the coordinator and ready to run. Everything the body
// can reach is in here by value, or behind a const pointer to something the run
// does not mutate, which is what makes it safe to execute several of these at
// once while the cache stays single-threaded.
struct Evaluator::PreparedNode {
    NodeId nodeId;
    const NodeType* nodeType = nullptr;
    Node effectiveNode;
    ValueMap inputs;
    size_t inputHash = 0;
    ExecutionDomain domain = ExecutionDomain::Worker;
    NodeExecutionContext execution;

    // Filled by the execute phase and read by the publish phase.
    ValueMap outputs;
    std::string nodeError;
    GuardOutcome guarded;
    double elapsedMs = 0.0;
};

void Evaluator::FailNode (const NodeId& nodeId, const std::string& message, double elapsedMs, PhaseState& state)
{
    const GraphDocument& document = *state.document;
    const RunContext& context = *state.context;
    EvaluationOutcome& outcome = *state.outcome;

    CacheEntry& failing = cache_[nodeId];
    failing.dirty = true;
    failing.status.state = NodeExecutionState::Failed;
    failing.status.message = message;
    failing.status.cacheHit = false;
    failing.status.durationMilliseconds = elapsedMs;
    failing.status.runId = context.runId;
    ++outcome.failedCount;
    if (outcome.error.empty ()) {
        outcome.error = message;
        outcome.failedNode = nodeId;
    }
    outcome.succeeded = false;
    if (context.events) {
        RunEvent failed;
        failed.kind = RunEventKind::NodeFailed;
        failed.runId = context.runId;
        failed.graphRevision = context.graphRevision;
        failed.nodeId = nodeId;
        failed.message = message;
        failed.durationMilliseconds = elapsedMs;
        context.events (std::move (failed));
    }
    BlockDownstream (document, nodeId, "upstream node failed: " + nodeId, context, outcome.blockedCount);
    for (const NodeId& blocked : DownstreamClosure (document, { nodeId }))
        state.unusable->insert (blocked);
}

bool Evaluator::PrepareNode (const NodeId& nodeId, PhaseState& state, PreparedNode& prepared)
{
    const GraphDocument& document = *state.document;
    const NodeRegistry& registry = *state.registry;
    const RunContext& context = *state.context;
    const EvaluationPlan& plan = *state.plan;
    EvaluationOutcome& outcome = *state.outcome;

    if (state.unusable->contains (nodeId))
        return false;

    const Node& node = *document.FindNode (nodeId);
    const NodeType& nodeType = *registry.Find (node.nodeType);

    // Cache key: node identity and parameters by value, upstream results by
    // OUTPUT REVISION rather than by content, and the host state this node
    // declared it reads. That last part is what stops a model-reading node from
    // serving a stale answer after the user changes the model.
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
    for (const GenerationDomain domain : nodeType.generations.Domains ()) {
        CombineText (inputHash, GenerationDomainName (domain));
        CombineHash (inputHash, static_cast<size_t> (plan.generations.Value (domain)));
    }

    ValueMap inputs;
    for (const PortSchema& input : nodeType.inputs) {
        Value::List values;
        for (const Edge& edge : document.Edges ()) {
            if (edge.targetNode != nodeId || edge.targetPort != input.id)
                continue;
            const auto source = cache_.find (edge.sourceNode);
            const std::shared_ptr<const NodeResult> sourceResult =
                source == cache_.end () ? nullptr : source->second.result;
            if (!sourceResult || !sourceResult->outputs.contains (edge.sourcePort)) {
                FailNode (nodeId, "upstream output is absent: " + edge.sourceNode + "." + edge.sourcePort, 0.0,
                          state);
                return false;
            }
            values.push_back (sourceResult->outputs.at (edge.sourcePort));
            CombineText (inputHash, edge.sourceNode);
            CombineText (inputHash, edge.sourcePort);
            CombineHash (inputHash, static_cast<size_t> (sourceResult->outputRevision));
        }

        if (values.empty ()) {
            if (input.required) {
                FailNode (nodeId, "required input is unconnected: " + input.id, 0.0, state);
                return false;
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
        return false;
    }

    prepared.nodeId = nodeId;
    prepared.nodeType = &nodeType;
    prepared.inputHash = inputHash;
    prepared.inputs = std::move (inputs);
    prepared.domain = nodeType.executionDomain;

    prepared.effectiveNode = node;
    for (const ParameterSchema& parameter : nodeType.parameters) {
        if (!prepared.effectiveNode.parameters.contains (parameter.id) && parameter.defaultValue)
            prepared.effectiveNode.parameters.emplace (parameter.id, *parameter.defaultValue);
    }

    // THE HOST IS HANDED ONLY TO NODES WHOSE DOMAIN SAYS THEY RUN ON IT, and
    // that is the mechanism rather than a tidiness. Every call on IArchicadHost,
    // the reference resolver included, crosses MainThreadGate, and Invoke from a
    // pool thread POSTS AND WAITS for Archicad to dispatch it. While the
    // coordinator is Archicad's own thread, sitting inside the level batch,
    // nothing can dispatch until that batch ends, so a worker-domain node
    // reaching the host would stall for the whole gate timeout and then fail.
    // Withholding the pointer turns a latent deadlock into a node that cannot
    // express the mistake.
    prepared.execution = *state.execution;
    if (nodeType.executionDomain != ExecutionDomain::ArchicadMainThread) {
        prepared.execution.archicad = nullptr;
        prepared.execution.references = &OfflineResolver ();
    }
    return true;
}

void Evaluator::PublishNode (PreparedNode& prepared, PhaseState& state)
{
    const RunContext& context = *state.context;
    EvaluationOutcome& outcome = *state.outcome;
    const NodeType& nodeType = *prepared.nodeType;
    const NodeId& nodeId = prepared.nodeId;

    std::string failure;
    if (!prepared.guarded.completed)
        failure = prepared.guarded.fault;
    else if (!prepared.guarded.result)
        failure = prepared.nodeError.empty () ? "the node reported failure without a message" : prepared.nodeError;
    else if (prepared.elapsedMs > context.limits.nodeBudgetMs)
        failure = "the node exceeded its time budget";

    size_t itemCount = 0;
    if (failure.empty ()) {
        for (const auto& [portId, value] : prepared.outputs) {
            const PortSchema* output = FindOutput (nodeType, portId);
            if (output == nullptr || output->valueType != value.Type ()) {
                failure = "invalid output: " + portId;
                break;
            }
            // Rule 5: an oversized or pathologically nested result fails its node
            // rather than entering the cache.
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
            if (output.required && !prepared.outputs.contains (output.id)) {
                failure = "omitted output: " + output.id;
                break;
            }
        }
    }

    if (!failure.empty ()) {
        ++cache_[nodeId].status.evaluationCount;
        FailNode (nodeId, "node " + nodeId + " failed: " + failure, prepared.elapsedMs, state);
        return;
    }

    CacheEntry& entry = cache_[nodeId];
    entry.inputHash = prepared.inputHash;
    entry.result = std::make_shared<const NodeResult> (
        NodeResult { std::move (prepared.outputs), prepared.elapsedMs, nextOutputRevision_++, itemCount });
    entry.dirty = false;
    entry.status.state = NodeExecutionState::Complete;
    entry.status.message.clear ();
    entry.status.cacheHit = false;
    entry.status.durationMilliseconds = prepared.elapsedMs;
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
        completed.durationMilliseconds = prepared.elapsedMs;
        completed.itemCount = itemCount;
        context.events (std::move (completed));
    }
}

bool Evaluator::RunLevel (size_t levelIndex, const std::vector<NodeId>& level, PhaseState& state)
{
    const RunContext& context = *state.context;
    const NodeExecutor& executor = *state.executor;

    if (context.cancellation.IsCancelled ())
        return false;

    // PREPARE, on this thread. Members of one level are independent by
    // construction, so gathering every input before running any body cannot read
    // a result this level is about to produce.
    std::vector<PreparedNode> prepared (level.size ());
    std::vector<size_t> runnable;
    runnable.reserve (level.size ());
    for (size_t i = 0; i < level.size (); ++i) {
        if (PrepareNode (level[i], state, prepared[i]))
            runnable.push_back (i);
    }
    if (runnable.empty ())
        return !context.cancellation.IsCancelled ();

    std::vector<size_t> workerNodes;
    std::vector<size_t> hostNodes;
    for (const size_t index : runnable) {
        if (prepared[index].domain == ExecutionDomain::ArchicadMainThread)
            hostNodes.push_back (index);
        else
            workerNodes.push_back (index);
    }

    for (const size_t index : runnable)
        Emit (context, RunEventKind::NodeStarted, prepared[index].nodeId);

    // EXECUTE. Nothing below touches the cache, the outcome or the event sink -
    // that separation is what lets the bodies run on several threads while the
    // runtime state stays single-threaded.
    std::atomic<size_t> inFlight { 0 };
    std::atomic<size_t> peak { 0 };
    const auto runBody = [&executor, &prepared, &inFlight, &peak] (size_t index) {
        const size_t now = inFlight.fetch_add (1, std::memory_order_relaxed) + 1;
        size_t seen = peak.load (std::memory_order_relaxed);
        while (now > seen && !peak.compare_exchange_weak (seen, now, std::memory_order_relaxed)) {
        }

        PreparedNode& node = prepared[index];
        const auto started = std::chrono::steady_clock::now ();
        // Rule 2: the node body runs behind the fault barrier, so a structured
        // exception in node code fails the node instead of the process - and now
        // instead of the POOL thread, where an escaped fault would take the
        // process down just as surely.
        node.guarded = RunGuarded ([&node, &executor] () {
            return executor (node.effectiveNode, node.inputs, node.execution, node.outputs, node.nodeError);
        });
        node.elapsedMs =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();

        inFlight.fetch_sub (1, std::memory_order_relaxed);
    };

    LevelMetrics metrics;
    metrics.levelIndex = levelIndex;
    metrics.executedCount = runnable.size ();
    metrics.workerNodeCount = workerNodes.size ();
    metrics.hostNodeCount = hostNodes.size ();

    const auto levelStarted = std::chrono::steady_clock::now ();
    if (!workerNodes.empty ()) {
        SharedWorkerPool ().RunBatch (workerNodes.size (), state.outcome->parallelism.maxParallel,
                                      [&workerNodes, &runBody] (size_t slot) { runBody (workerNodes[slot]); });
    }
    // Archicad-domain nodes run HERE, on the coordinator, one at a time. Not a
    // limitation being accepted: the host serialises them at the gate anyway,
    // and running them on the coordinator is what keeps MainThreadGate::Invoke
    // on its inline path when the coordinator is Archicad's own thread.
    for (const size_t index : hostNodes)
        runBody (index);
    metrics.wallClockMs =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - levelStarted).count ();
    metrics.peakConcurrency = peak.load (std::memory_order_relaxed);

    // PUBLISH, on this thread and in LEVEL ORDER rather than completion order,
    // so the event stream and the cache read the same however the scheduler
    // interleaved the bodies.
    for (const size_t index : runnable) {
        metrics.workMs += prepared[index].elapsedMs;
        if (state.unusable->contains (prepared[index].nodeId))
            continue;
        PublishNode (prepared[index], state);
    }

    ParallelismMetrics& parallelism = state.outcome->parallelism;
    parallelism.wallClockMs += metrics.wallClockMs;
    parallelism.workMs += metrics.workMs;
    parallelism.peakConcurrency = std::max (parallelism.peakConcurrency, metrics.peakConcurrency);
    parallelism.levels.push_back (metrics);

    return !context.cancellation.IsCancelled ();
}

bool Evaluator::RunLevels (const std::vector<std::vector<NodeId>>& levels, PhaseState& state)
{
    for (size_t i = 0; i < levels.size (); ++i) {
        if (!RunLevel (i, levels[i], state))
            return false;
    }
    return true;
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
    outcome.skippedEffectNodes = plan.skippedEffectNodes;

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

    NodeExecutionContext execution;
    execution.archicad = context.archicad;
    execution.references = context.archicad != nullptr ? &context.archicad->References () : &OfflineResolver ();
    execution.generations = plan.generations;
    execution.cancellation = context.cancellation;

    // The cap this run will honour, resolved once so the outcome reports the
    // number that was actually used rather than the zero that asked for a
    // default.
    outcome.parallelism.workerThreads = SharedWorkerPool ().ThreadCount ();
    outcome.parallelism.maxParallel = request.maxParallel == 0
                                          ? SharedWorkerPool ().ThreadCount () + 1
                                          : request.maxParallel;

    std::set<NodeId> unusable;
    outcome.succeeded = true;

    PhaseState state;
    state.document = &document;
    state.registry = &registry;
    state.executor = &executor;
    state.plan = &plan;
    state.context = &context;
    state.execution = &execution;
    state.outcome = &outcome;
    state.unusable = &unusable;

    const auto cancel = [&] () {
        // Rule 7: cancelling degrades nothing. Nodes that already finished keep
        // their results; only what did not run is marked cancelled.
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
    };

    if (!RunLevels (plan.levels, state)) {
        cancel ();
        return outcome;
    }

    // An effectful node that was not permitted is reported, not hidden. Nothing
    // is wrong with it; this run simply did not ask for it.
    for (const NodeId& nodeId : plan.skippedEffectNodes) {
        CacheEntry& entry = cache_[nodeId];
        entry.status.state = NodeExecutionState::Skipped;
        entry.status.message = "this node changes Archicad and runs only on an explicit run";
        entry.status.cacheHit = false;
        entry.status.runId = context.runId;
    }

    // The deferred phase has to EARN its turn. A graph that failed anywhere must
    // not go on to change the user's selection - that is the whole reason
    // effects are a second phase rather than nodes in topological position.
    if (!plan.deferredNodes.empty ()) {
        if (!outcome.succeeded) {
            for (const NodeId& nodeId : plan.deferredNodes) {
                CacheEntry& entry = cache_[nodeId];
                entry.status.state = NodeExecutionState::Blocked;
                entry.status.message = "not applied: the evaluation failed before it could be";
                entry.status.runId = context.runId;
                ++outcome.blockedCount;
                Emit (context, RunEventKind::NodeBlocked, nodeId, entry.status.message);
            }
        }
        else if (!RunLevels (SingletonLevels (plan.deferredNodes), state)) {
            cancel ();
            return outcome;
        }
        else {
            outcome.effectsCommitted = outcome.succeeded;
        }
    }

    EmitRunFinished (context, outcome, RunEventKind::RunCompleted);
    return outcome;
}

void Evaluator::Reset ()
{
    cache_.clear ();
    // Output revisions deliberately keep counting. They are an identity for a
    // published result, not a position in this document, and restarting them
    // would let a stale consumer's remembered revision match a new result.
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
