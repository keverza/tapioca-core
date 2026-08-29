#ifndef EVP_NODEGRAPH_RUNHISTORY_HPP
#define EVP_NODEGRAPH_RUNHISTORY_HPP

// What happened, per run and per node.
//
// These records are a FOLD over the RunEvent stream and are written nowhere
// else. That is the whole design rule: the handoff specified an event stream and
// per-node records with no stated relationship between them, and two independent
// writers over one truth drift. Folding gets the consistency of event sourcing
// without the machinery.
//
// In memory, bounded, and deliberately small. Large outputs are not recorded -
// a run record carries counts, timings and messages, never geometry. ADR-007
// adds no dependency, so SQLite is not here and IRunHistoryStore is not shaped
// around it; it exists so a durable store can be substituted later without the
// evaluator learning about persistence.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/RunEvents.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace evp::nodegraph {

struct NodeRunRecord {
    NodeId nodeId;
    NodeExecutionState finalState = NodeExecutionState::Dirty;
    std::string message;
    double durationMilliseconds = 0.0;
    size_t itemCount = 0;
    bool cacheHit = false;
};

struct RunRecord {
    RunId runId = kNoRun;
    GraphId graphId;
    uint64_t graphRevision = 0;

    int64_t startedAtMs = 0;
    int64_t finishedAtMs = 0;

    bool finished = false;
    bool succeeded = false;
    bool cancelled = false;

    std::string error;
    NodeId failedNode;

    size_t plannedCount = 0;
    size_t executedCount = 0;
    size_t cacheHitCount = 0;
    size_t failedCount = 0;
    size_t blockedCount = 0;

    std::vector<NodeRunRecord> nodes;
};

class IRunHistoryStore {
  public:
    virtual ~IRunHistoryStore () = default;

    // The only mutating entry point, and it takes an event. There is no way to
    // write a record that the stream does not also describe.
    virtual void Apply (const RunEvent& event) = 0;

    // Newest first.
    virtual std::vector<RunRecord> Recent (size_t maxRuns) const = 0;

    virtual std::optional<RunRecord> Find (RunId runId) const = 0;
};

class InMemoryRunHistoryStore final : public IRunHistoryStore {
  public:
    explicit InMemoryRunHistoryStore (size_t capacity = 64);

    void Apply (const RunEvent& event) override;
    std::vector<RunRecord> Recent (size_t maxRuns) const override;
    std::optional<RunRecord> Find (RunId runId) const override;

  private:
    RunRecord* FindMutable (RunId runId);
    NodeRunRecord& NodeFor (RunRecord& record, const NodeId& nodeId);

    mutable std::mutex mutex_;
    std::deque<RunRecord> runs_;
    size_t capacity_;
};

// Stamps, appends and folds - in that order, under one lock - so the log and the
// history can never disagree about a run.
class RunRecorder {
  public:
    RunRecorder (size_t eventCapacity = 4096, size_t historyCapacity = 64);

    // Fills seq, graphId and timestamp, then records. Safe to call from any
    // thread; safe to call with no listener.
    void Emit (RunEvent event, const GraphId& graphId);

    RunEventLog& Events ()
    {
        return events_;
    }
    const RunEventLog& Events () const
    {
        return events_;
    }
    const IRunHistoryStore& History () const
    {
        return history_;
    }

    // A sink bound to one graph, for handing to an evaluation.
    RunEventSink SinkFor (GraphId graphId);

  private:
    std::mutex mutex_;
    RunEventLog events_;
    InMemoryRunHistoryStore history_;
};

} // namespace evp::nodegraph

#endif
