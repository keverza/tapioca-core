#ifndef EVP_NODEGRAPH_RUNCONTEXT_HPP
#define EVP_NODEGRAPH_RUNCONTEXT_HPP

// Run identity and cooperative cancellation.
//
// Every evaluation carries a monotonic RunId. The point is not logging: it is
// that a late result from a superseded run must be identifiable and discarded.
// Cancellation is best-effort, so "the run was cancelled" and "a result arrived
// from a run that is no longer current" are DIFFERENT conditions and both have
// to be handled. RunId handles the second.

#include "NodeGraph/RunEvents.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace evp::nodegraph {

// Cooperative only. Nothing here interrupts a running node; it asks. A node that
// never checks and never returns is contained by the node budget in
// EvaluationLimits and, from the worker-pool stage, by running off the host
// thread. Shared by value so a worker keeps the token alive past the requester.
class CancellationToken {
  public:
    CancellationToken () : flag_ (std::make_shared<std::atomic<bool>> (false))
    {
    }

    void Cancel () const
    {
        flag_->store (true, std::memory_order_relaxed);
    }

    bool IsCancelled () const
    {
        return flag_->load (std::memory_order_relaxed);
    }

  private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// Ceilings that turn a pathological graph into a rejected graph instead of a
// dead process. Every one of these has a matching offline test.
struct EvaluationLimits {
    // Reject a plan larger than this rather than attempting it.
    size_t maxPlanNodes = 100000;

    // A single node result may not exceed this many aggregate list items.
    size_t maxOutputItems = 5000000;

    // Nesting depth permitted inside one Value. Guards the recursive walks that
    // hash and measure list values.
    size_t maxValueDepth = 64;

    // Wall-clock budget for one node. Exceeding it fails that node and ends the
    // run. Enforced on return, so it detects a slow node; interrupting a node
    // that never returns needs the node to run off the host thread.
    double nodeBudgetMs = 30000.0;
};

struct RunContext {
    RunId runId = kNoRun;
    GraphId graphId;
    uint64_t graphRevision = 0;
    CancellationToken cancellation;
    EvaluationLimits limits;

    // Optional. An evaluation with no sink still runs; it is simply unobserved,
    // which is what keeps the offline tests free of the recorder.
    RunEventSink events;
};

} // namespace evp::nodegraph

#endif
