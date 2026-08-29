#ifndef EVP_NODEGRAPH_RUNEVENTS_HPP
#define EVP_NODEGRAPH_RUNEVENTS_HPP

// The runtime's observation channel, and the reason this backend is not tied to
// one frontend.
//
// A client holds an authoritative snapshot plus a sequence number, and asks for
// everything after it. That needs no push channel, no socket and no callback
// into the client, so a Svelte page, a pytest script, a CLI and an agent all
// consume the runtime through the same two calls. When a client's sequence has
// fallen off the back of the ring it is TOLD so, and re-snapshots - a silent gap
// is the one failure this design has to make impossible.
//
// The stream is also the single writer for run history: RunRecord is a fold over
// these events, never a parallel bookkeeping path, because two writers over one
// truth drift and the drift surfaces as a UI bug nobody can reproduce.

#include "NodeGraph/Graph.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace evp::nodegraph {

using EventSeq = uint64_t;

constexpr EventSeq kNoEvent = 0;

enum class RunEventKind {
    RunStarted,
    NodeQueued,
    NodeStarted,
    NodeCacheHit,
    NodeCompleted,
    NodeWarning,
    NodeFailed,
    NodeBlocked,
    NodeCancelled,
    RunCompleted,
    RunCancelled,
};

const char* RunEventKindName (RunEventKind kind);

// One flat record rather than a variant payload: every field a client renders is
// present or zero, which keeps the wire schema fixed and the fold trivial.
struct RunEvent {
    EventSeq seq = kNoEvent;
    RunEventKind kind = RunEventKind::RunStarted;

    // Correlation identity. Every diagnostic the runtime emits carries these, so
    // a failure is traceable across the runtime, the log and the client.
    GraphId graphId;
    uint64_t graphRevision = 0;
    RunId runId = kNoRun;
    NodeId nodeId; // empty for run-scoped events

    // Milliseconds since the Unix epoch, so a client can line an event up with
    // an ordinary log without sharing the runtime's clock.
    int64_t timestampMs = 0;

    std::string message;
    double durationMilliseconds = 0.0;
    size_t itemCount = 0;

    // Run-scoped counters, set on RunCompleted/RunCancelled.
    size_t plannedCount = 0;
    size_t executedCount = 0;
    size_t cacheHitCount = 0;
    size_t failedCount = 0;
    size_t blockedCount = 0;
};

// The evaluator emits through this. `seq`, `graphId` and `timestampMs` are
// stamped by the recorder, so the evaluator never has to know either.
using RunEventSink = std::function<void (RunEvent&&)>;

int64_t NowMillisecondsSinceEpoch ();

// A bounded ring. Old events are dropped, never grown into: an evaluation that
// touches ten thousand nodes must not be able to exhaust memory through its own
// observability.
class RunEventLog {
  public:
    explicit RunEventLog (size_t capacity = 4096);

    // Stamps `event.seq` and stores it. Returns the assigned sequence.
    EventSeq Append (RunEvent event);

    struct Tail {
        std::vector<RunEvent> events;
        EventSeq lastSeq = kNoEvent;

        // True when `sinceSeq` was already older than the oldest retained event,
        // so the caller has missed something and must re-snapshot rather than
        // stitch an incomplete tail onto stale state.
        bool gap = false;
    };

    Tail Since (EventSeq sinceSeq, size_t maxEvents) const;

    EventSeq LastSeq () const;

  private:
    mutable std::mutex mutex_;
    std::deque<RunEvent> events_;
    size_t capacity_;
    EventSeq nextSeq_ = 1;
};

} // namespace evp::nodegraph

#endif
