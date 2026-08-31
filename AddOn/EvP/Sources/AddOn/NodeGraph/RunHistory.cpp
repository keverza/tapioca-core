#include "NodeGraph/RunHistory.hpp"

#include <algorithm>

namespace evp::nodegraph {

InMemoryRunHistoryStore::InMemoryRunHistoryStore (size_t capacity) : capacity_ (capacity == 0 ? 1 : capacity)
{
}

RunRecord* InMemoryRunHistoryStore::FindMutable (RunId runId)
{
    const auto found = std::find_if (runs_.begin (), runs_.end (),
                                     [runId] (const RunRecord& record) { return record.runId == runId; });
    return found == runs_.end () ? nullptr : &*found;
}

NodeRunRecord& InMemoryRunHistoryStore::NodeFor (RunRecord& record, const NodeId& nodeId)
{
    const auto found = std::find_if (record.nodes.begin (), record.nodes.end (),
                                     [&nodeId] (const NodeRunRecord& node) { return node.nodeId == nodeId; });
    if (found != record.nodes.end ())
        return *found;
    record.nodes.push_back (NodeRunRecord { nodeId });
    return record.nodes.back ();
}

void InMemoryRunHistoryStore::Apply (const RunEvent& event)
{
    std::lock_guard lock (mutex_);

    if (event.kind == RunEventKind::RunStarted) {
        RunRecord record;
        record.runId = event.runId;
        record.graphId = event.graphId;
        record.graphRevision = event.graphRevision;
        record.startedAtMs = event.timestampMs;
        record.plannedCount = event.plannedCount;
        runs_.push_back (std::move (record));
        while (runs_.size () > capacity_)
            runs_.pop_front ();
        return;
    }

    RunRecord* record = FindMutable (event.runId);
    if (record == nullptr) {
        // The run's start has already aged out of the ring. Dropping the tail is
        // correct: a record without its start would report a run that seems to
        // have planned nothing.
        return;
    }

    switch (event.kind) {
        case RunEventKind::RunStarted:
            break;

        case RunEventKind::NodeQueued:
            NodeFor (*record, event.nodeId);
            break;

        case RunEventKind::NodeStarted: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.cacheHit = false;
            node.finalState = NodeExecutionState::Running;
            break;
        }

        case RunEventKind::NodeCacheHit: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Success;
            node.cacheHit = true;
            node.itemCount = event.itemCount;
            break;
        }

        case RunEventKind::NodeCompleted: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Success;
            node.durationMilliseconds = event.durationMilliseconds;
            node.itemCount = event.itemCount;
            node.cacheHit = false;
            break;
        }

        case RunEventKind::NodeWarning:
            NodeFor (*record, event.nodeId).message = event.message;
            break;

        case RunEventKind::NodeFailed: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Error;
            node.message = event.message;
            node.durationMilliseconds = event.durationMilliseconds;
            if (record->failedNode.empty ()) {
                record->failedNode = event.nodeId;
                record->error = event.message;
            }
            break;
        }

        case RunEventKind::NodeBlocked: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Blocked;
            node.message = event.message;
            break;
        }

        case RunEventKind::NodeCancelled: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Cancelled;
            break;
        }

        // Stage F. Folding these into NodeCompleted would make the history say
        // that a bypassed node ran and that a dam published, which is the
        // opposite of what each of them means.
        case RunEventKind::NodeBypassed: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Bypassed;
            node.message = event.message;
            node.itemCount = event.itemCount;
            node.cacheHit = false;
            break;
        }

        case RunEventKind::NodeHeld:
        case RunEventKind::NodeReleased: {
            NodeRunRecord& node = NodeFor (*record, event.nodeId);
            node.finalState = NodeExecutionState::Holding;
            node.message = event.message;
            node.durationMilliseconds = event.durationMilliseconds;
            node.itemCount = event.itemCount;
            node.cacheHit = false;
            break;
        }

        case RunEventKind::RunCompleted:
        case RunEventKind::RunCancelled:
            record->finished = true;
            record->finishedAtMs = event.timestampMs;
            record->cancelled = event.kind == RunEventKind::RunCancelled;
            record->succeeded = event.kind == RunEventKind::RunCompleted && event.failedCount == 0;
            record->plannedCount = event.plannedCount;
            record->executedCount = event.executedCount;
            record->cacheHitCount = event.cacheHitCount;
            record->failedCount = event.failedCount;
            record->blockedCount = event.blockedCount;
            if (!event.message.empty () && record->error.empty ())
                record->error = event.message;
            break;
    }
}

std::vector<RunRecord> InMemoryRunHistoryStore::Recent (size_t maxRuns) const
{
    std::lock_guard lock (mutex_);
    std::vector<RunRecord> result;
    for (auto iterator = runs_.rbegin (); iterator != runs_.rend (); ++iterator) {
        if (maxRuns != 0 && result.size () >= maxRuns)
            break;
        result.push_back (*iterator);
    }
    return result;
}

std::optional<RunRecord> InMemoryRunHistoryStore::Find (RunId runId) const
{
    std::lock_guard lock (mutex_);
    const auto found = std::find_if (runs_.begin (), runs_.end (),
                                     [runId] (const RunRecord& record) { return record.runId == runId; });
    if (found == runs_.end ())
        return std::nullopt;
    return *found;
}

RunRecorder::RunRecorder (size_t eventCapacity, size_t historyCapacity)
    : events_ (eventCapacity), history_ (historyCapacity)
{
}

void RunRecorder::Emit (RunEvent event, const GraphId& graphId)
{
    std::lock_guard lock (mutex_);
    event.graphId = graphId;
    if (event.timestampMs == 0)
        event.timestampMs = NowMillisecondsSinceEpoch ();
    // Appended first, and the assigned sequence carried into the fold, so the
    // stream and the history can never describe a run differently.
    event.seq = events_.Append (event);
    history_.Apply (event);
}

RunEventSink RunRecorder::SinkFor (GraphId graphId)
{
    return [this, graphId = std::move (graphId)] (RunEvent&& event) { Emit (std::move (event), graphId); };
}

} // namespace evp::nodegraph
