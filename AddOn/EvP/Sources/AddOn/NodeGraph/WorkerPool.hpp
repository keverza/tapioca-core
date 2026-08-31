#ifndef EVP_NODEGRAPH_WORKERPOOL_HPP
#define EVP_NODEGRAPH_WORKERPOOL_HPP

// The unit of parallelism: one topological level, run across a fixed pool.
//
// ADR-007 adds no dependency for this. Taskflow buys work stealing, subflows and
// heterogeneous scheduling, and the architecture document's §40.2 promotion test
// asks for MEASURED need of several of those. A graph level is a flat list of
// independent nodes with no nesting and no dynamic spawning, which is the one
// shape a fixed pool serves exactly.
//
// ⚠️ THE SUBMITTING THREAD PARTICIPATES, and that is load-bearing rather than an
// optimisation. It means a pool of zero threads runs the batch inline on the
// caller, so the offline suite and a single-core machine exercise THE SAME code
// path as a four-way run instead of a sequential fallback that is never tested.
//
// ⚠️ `maxParallel` IS A MEASUREMENT KNOB, NOT A TUNING ONE. ADR-007's gate is
// "pure nodes demonstrably execute concurrently", and demonstrating it needs the
// same graph run at 1 and at N. Capping concurrency inside the pool - rather
// than by building a differently sized pool - keeps the two arms identical in
// every other respect, including thread creation cost.
//
// ⚠️ NOTHING HERE MAY TOUCH ACAPI. A pooled task runs on a thread that is not
// Archicad's, and MainThreadGate::Invoke from such a thread posts and WAITS for
// the main thread to dispatch it - which, while the submitting thread is the
// main thread sitting in RunBatch, cannot happen until the batch ends. The
// evaluator enforces this by withholding the host from worker-domain nodes; see
// Evaluator.cpp. Do not weaken it here.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace evp::nodegraph {

// Hardware concurrency less the submitting thread, clamped to something a
// desktop add-on should not exceed. Never zero: one thread plus the caller.
size_t DefaultWorkerThreadCount ();

class WorkerPool final {
  public:
    explicit WorkerPool (size_t threadCount);
    ~WorkerPool ();

    WorkerPool (const WorkerPool&) = delete;
    WorkerPool& operator= (const WorkerPool&) = delete;

    // Pool threads only; the submitting thread is additional.
    size_t ThreadCount () const
    {
        return threads_.size ();
    }

    // Runs body(0), .. body(count-1), each exactly once, and returns only when
    // every one has finished - including after a task throws, which is contained
    // per task so one bad task cannot strand the batch.
    //
    // `maxParallel` bounds how many tasks may be in flight at once, counting the
    // submitting thread. 1 runs the whole batch on the caller; 0 means the pool
    // decides. Calls from two threads at once are serialised: the pool runs one
    // batch at a time.
    void RunBatch (size_t count, size_t maxParallel, const std::function<void (size_t)>& body);

    // Joins every pool thread and leaves the pool permanently empty, so a later
    // batch runs inline on its submitter rather than failing. Idempotent.
    //
    // ⚠️ THE POOL MUST BE STOPPED ON ARCHICAD'S OWN SHUTDOWN PATH, not left to
    // static destruction. A function-local static is destroyed when the APX
    // unloads, and joining threads at that point runs inside the loader lock -
    // the same hazard this add-on already handles for its windows, timers, hooks
    // and the Grasshopper worker. See ShutDownWorkerPool below.
    void Stop ();

  private:
    void ThreadMain ();

    // Claims the next index, or returns false when there is nothing left to
    // claim. Caller must hold `mutex_`.
    bool ClaimLocked (size_t& index);

    static void RunOne (const std::function<void (size_t)>& body, size_t index);

    std::vector<std::thread> threads_;

    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable batchFinished_;

    // Serialises submitters, so `batch_` below needs no per-batch identity.
    std::mutex submitMutex_;

    const std::function<void (size_t)>* body_ = nullptr;
    size_t count_ = 0;
    size_t claimed_ = 0;
    size_t finished_ = 0;

    // Tasks in flight, INCLUDING the submitting thread's reserved slot, which is
    // held for the whole batch so the cap is exact at the tail rather than
    // drifting upward as the caller runs dry.
    size_t active_ = 0;
    size_t maxParallel_ = 0;

    bool stopping_ = false;
};

// The pool every evaluation shares. Created on first use, so a process that
// never evaluates a graph never spawns a thread, and the offline suite pays for
// one pool rather than one per test.
WorkerPool& SharedWorkerPool ();

// Joins the shared pool's threads. Called from the add-on's quit and unload
// paths, alongside the gate shutdown and the other thread owners, so no pool
// thread is alive when this module goes away. The pool object itself is not
// destroyed - see the definition.
void ShutDownWorkerPool ();

} // namespace evp::nodegraph

#endif
