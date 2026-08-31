#include "NodeGraph/RunEvents.hpp"

#include <algorithm>
#include <chrono>

namespace evp::nodegraph {

const char* RunEventKindName (RunEventKind kind)
{
    switch (kind) {
        case RunEventKind::RunStarted:
            return "runStarted";
        case RunEventKind::NodeQueued:
            return "nodeQueued";
        case RunEventKind::NodeStarted:
            return "nodeStarted";
        case RunEventKind::NodeCacheHit:
            return "nodeCacheHit";
        case RunEventKind::NodeCompleted:
            return "nodeCompleted";
        case RunEventKind::NodeWarning:
            return "nodeWarning";
        case RunEventKind::NodeFailed:
            return "nodeFailed";
        case RunEventKind::NodeBlocked:
            return "nodeBlocked";
        case RunEventKind::NodeCancelled:
            return "nodeCancelled";
        case RunEventKind::NodeBypassed:
            return "nodeBypassed";
        case RunEventKind::NodeHeld:
            return "nodeHeld";
        case RunEventKind::NodeReleased:
            return "nodeReleased";
        case RunEventKind::RunCompleted:
            return "runCompleted";
        case RunEventKind::RunCancelled:
            return "runCancelled";
    }
    return "runStarted";
}

int64_t NowMillisecondsSinceEpoch ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ())
        .count ();
}

RunEventLog::RunEventLog (size_t capacity) : capacity_ (capacity == 0 ? 1 : capacity)
{
}

EventSeq RunEventLog::Append (RunEvent event)
{
    std::lock_guard lock (mutex_);
    event.seq = nextSeq_++;
    const EventSeq assigned = event.seq;
    events_.push_back (std::move (event));
    while (events_.size () > capacity_)
        events_.pop_front ();
    return assigned;
}

RunEventLog::Tail RunEventLog::Since (EventSeq sinceSeq, size_t maxEvents) const
{
    std::lock_guard lock (mutex_);
    Tail tail;
    tail.lastSeq = nextSeq_ - 1;

    // A client that has seen nothing (sinceSeq 0) is not in a gap - it is
    // starting, and the snapshot it pairs this with covers what the ring has
    // already dropped.
    if (sinceSeq != kNoEvent && !events_.empty () && sinceSeq < events_.front ().seq - 1)
        tail.gap = true;

    for (const RunEvent& event : events_) {
        if (event.seq <= sinceSeq)
            continue;
        if (maxEvents != 0 && tail.events.size () >= maxEvents) {
            // Truncated, not lost: lastSeq reports what the caller actually has,
            // so the next call resumes exactly here.
            tail.lastSeq = tail.events.empty () ? sinceSeq : tail.events.back ().seq;
            break;
        }
        tail.events.push_back (event);
    }
    return tail;
}

EventSeq RunEventLog::LastSeq () const
{
    std::lock_guard lock (mutex_);
    return nextSeq_ - 1;
}

} // namespace evp::nodegraph
