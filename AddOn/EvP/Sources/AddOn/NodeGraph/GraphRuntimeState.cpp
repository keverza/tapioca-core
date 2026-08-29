#include "NodeGraph/GraphRuntimeState.hpp"

#include "NodeGraph/BuiltinNodes.hpp"

namespace evp::nodegraph {

const char* const kDefaultGraphId = "default";

GraphRuntimeState& GraphRuntimeState::Get ()
{
    static GraphRuntimeState state;
    return state;
}

GraphRuntimeState::GraphRuntimeState () : registry_ (MakeBuiltinNodeRegistry ())
{
}

GraphRuntimeState::Slot& GraphRuntimeState::SlotFor (const GraphId& graphId) const
{
    std::lock_guard lock (mapMutex_);
    std::unique_ptr<Slot>& slot = graphs_[graphId];
    if (!slot)
        slot = std::make_unique<Slot> ();
    return *slot;
}

NodeRegistry GraphRuntimeState::Catalog () const
{
    return registry_;
}

std::vector<GraphId> GraphRuntimeState::GraphIds () const
{
    std::lock_guard lock (mapMutex_);
    std::vector<GraphId> ids;
    for (const auto& [graphId, slot] : graphs_) {
        (void) slot;
        ids.push_back (graphId);
    }
    return ids;
}

GraphDocument GraphRuntimeState::Document (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    return slot.document;
}

EditResult GraphRuntimeState::Apply (const GraphId& graphId, const GraphEdit& edit)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);
    EditResult result = ApplyEdit (slot.document, registry_, edit);
    if (result.accepted)
        slot.evaluator.Invalidate (slot.document, result.dirtyNodes);
    return result;
}

RunId GraphRuntimeState::Cancel (const GraphId& graphId)
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.runMutex);
    if (!slot.currentCancellation.has_value ())
        return kNoRun;
    slot.currentCancellation->Cancel ();
    return slot.currentRunId;
}

EvaluationSummary GraphRuntimeState::Evaluate (const GraphId& graphId, const EvaluationRequest& request)
{
    Slot& slot = SlotFor (graphId);

    // A newer run supersedes an older one: ask the one in flight to stop before
    // queueing behind it on the document lock.
    Cancel (graphId);

    std::lock_guard lock (slot.documentMutex);

    RunContext context;
    context.graphId = graphId;
    {
        std::lock_guard runLock (slot.runMutex);
        context.runId = slot.nextRunId++;
        context.cancellation = CancellationToken {};
        slot.currentRunId = context.runId;
        slot.currentCancellation = context.cancellation;
    }
    context.graphRevision = slot.document.Revision ();
    context.events = slot.recorder.SinkFor (graphId);

    const EvaluationOutcome outcome =
        slot.evaluator.Evaluate (slot.document, registry_, ExecuteBuiltinNode, request, context);

    {
        std::lock_guard runLock (slot.runMutex);
        // Only retire the token if it is still ours; a newer run may already own it.
        if (slot.currentRunId == context.runId) {
            slot.currentCancellation.reset ();
            slot.currentRunId = kNoRun;
        }
        slot.lastRunId = context.runId;
    }

    EvaluationSummary summary;
    summary.graphId = graphId;
    summary.succeeded = outcome.succeeded;
    summary.cancelled = outcome.cancelled;
    summary.error = outcome.error;
    summary.failedNode = outcome.failedNode;
    summary.cyclicNodes = outcome.cyclicNodes;
    summary.runId = outcome.runId;
    summary.revision = outcome.graphRevision;
    summary.lastEventSeq = slot.recorder.Events ().LastSeq ();
    summary.plannedCount = outcome.plannedNodes.size ();
    summary.executedCount = outcome.executedCount;
    summary.cacheHitCount = outcome.cacheHitCount;
    summary.failedCount = outcome.failedCount;
    summary.blockedCount = outcome.blockedCount;
    return summary;
}

ResultsSnapshot GraphRuntimeState::Results (const GraphId& graphId) const
{
    Slot& slot = SlotFor (graphId);
    std::lock_guard lock (slot.documentMutex);

    ResultsSnapshot snapshot;
    snapshot.graphId = graphId;
    snapshot.revision = slot.document.Revision ();
    // Read INSIDE the document lock and before the node walk, so the sequence a
    // client stores can only be older than the state it describes. A client that
    // re-asks from a slightly early sequence replays an event; one that asks from
    // a late sequence would silently miss one.
    snapshot.lastEventSeq = slot.recorder.Events ().LastSeq ();
    {
        std::lock_guard runLock (slot.runMutex);
        snapshot.lastRunId = slot.lastRunId;
    }
    for (const auto& [nodeId, node] : slot.document.Nodes ()) {
        (void) node;
        snapshot.nodes.push_back ({ nodeId, slot.evaluator.Status (nodeId), slot.evaluator.Result (nodeId) });
    }
    return snapshot;
}

RunEventLog::Tail GraphRuntimeState::Events (const GraphId& graphId, EventSeq sinceSeq, size_t maxEvents) const
{
    // Deliberately not under documentMutex: the event tail has to remain
    // readable while a run holds that lock, or a client could not watch a run in
    // progress - which is the entire point of the stream.
    return SlotFor (graphId).recorder.Events ().Since (sinceSeq, maxEvents);
}

std::vector<RunRecord> GraphRuntimeState::RecentRuns (const GraphId& graphId, size_t maxRuns) const
{
    return SlotFor (graphId).recorder.History ().Recent (maxRuns);
}

} // namespace evp::nodegraph
