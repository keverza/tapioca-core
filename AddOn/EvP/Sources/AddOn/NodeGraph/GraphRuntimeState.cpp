#include "NodeGraph/GraphRuntimeState.hpp"

#include "NodeGraph/BuiltinNodes.hpp"

namespace evp::nodegraph {

GraphRuntimeState& GraphRuntimeState::Get ()
{
    static GraphRuntimeState state;
    return state;
}

GraphRuntimeState::GraphRuntimeState () : registry_ (MakeBuiltinNodeRegistry ())
{
}

NodeRegistry GraphRuntimeState::Catalog () const
{
    std::lock_guard lock (mutex_);
    return registry_;
}

GraphDocument GraphRuntimeState::Document () const
{
    std::lock_guard lock (mutex_);
    return document_;
}

EditResult GraphRuntimeState::Apply (const GraphEdit& edit)
{
    std::lock_guard lock (mutex_);
    EditResult result = ApplyEdit (document_, registry_, edit);
    if (result.accepted)
        evaluator_.Invalidate (document_, result.dirtyNodes);
    return result;
}

RunId GraphRuntimeState::Cancel ()
{
    // Takes runMutex_ only. Cancel has to be answerable while a run holds the
    // document lock, or it could never reach the run it exists to stop.
    std::lock_guard lock (runMutex_);
    if (!currentCancellation_.has_value ())
        return kNoRun;
    currentCancellation_->Cancel ();
    return currentRunId_;
}

EvaluationSummary GraphRuntimeState::Evaluate (const EvaluationRequest& request)
{
    // A newer run supersedes an older one: ask the one in flight to stop before
    // queueing behind it on the document lock.
    Cancel ();

    std::lock_guard lock (mutex_);

    RunContext context;
    {
        std::lock_guard runLock (runMutex_);
        context.runId = nextRunId_++;
        context.cancellation = CancellationToken {};
        currentRunId_ = context.runId;
        currentCancellation_ = context.cancellation;
    }
    context.graphRevision = document_.Revision ();

    const EvaluationOutcome outcome = evaluator_.Evaluate (document_, registry_, ExecuteBuiltinNode, request, context);

    {
        std::lock_guard runLock (runMutex_);
        // Only retire the token if it is still ours; a newer run may already own it.
        if (currentRunId_ == context.runId) {
            currentCancellation_.reset ();
            currentRunId_ = kNoRun;
        }
        lastRunId_ = context.runId;
    }

    EvaluationSummary summary;
    summary.succeeded = outcome.succeeded;
    summary.cancelled = outcome.cancelled;
    summary.error = outcome.error;
    summary.failedNode = outcome.failedNode;
    summary.cyclicNodes = outcome.cyclicNodes;
    summary.runId = outcome.runId;
    summary.revision = outcome.graphRevision;
    summary.plannedCount = outcome.plannedNodes.size ();
    summary.executedCount = outcome.executedCount;
    summary.cacheHitCount = outcome.cacheHitCount;
    summary.failedCount = outcome.failedCount;
    summary.blockedCount = outcome.blockedCount;
    return summary;
}

ResultsSnapshot GraphRuntimeState::Results () const
{
    std::lock_guard lock (mutex_);
    ResultsSnapshot snapshot;
    snapshot.revision = document_.Revision ();
    {
        std::lock_guard runLock (runMutex_);
        snapshot.lastRunId = lastRunId_;
    }
    for (const auto& [nodeId, node] : document_.Nodes ()) {
        (void) node;
        snapshot.nodes.push_back ({ nodeId, evaluator_.Status (nodeId), evaluator_.Result (nodeId) });
    }
    return snapshot;
}

} // namespace evp::nodegraph
