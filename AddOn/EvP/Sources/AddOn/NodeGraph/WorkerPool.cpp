#include "NodeGraph/WorkerPool.hpp"

#include <algorithm>

namespace evp::nodegraph {
namespace {

// More threads than this buys nothing on a graph level and costs Archicad the
// cores it needs for its own work while the user waits.
constexpr size_t kMaxWorkerThreads = 8;

} // namespace

size_t DefaultWorkerThreadCount ()
{
    const unsigned int reported = std::thread::hardware_concurrency ();
    // hardware_concurrency is permitted to answer 0 ("not computable"), and a
    // pool sized from it without a floor would silently run every graph
    // sequentially on such a machine while still claiming to be parallel.
    const size_t cores = reported == 0 ? 2U : static_cast<size_t> (reported);
    const size_t withoutSubmitter = cores > 1 ? cores - 1 : 1;
    return std::min (withoutSubmitter, kMaxWorkerThreads);
}

WorkerPool::WorkerPool (size_t threadCount)
{
    threads_.reserve (threadCount);
    for (size_t i = 0; i < threadCount; ++i)
        threads_.emplace_back ([this] () { ThreadMain (); });
}

WorkerPool::~WorkerPool ()
{
    Stop ();
}

void WorkerPool::Stop ()
{
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        if (threads_.empty ())
            return;
        stopping_ = true;
    }
    workAvailable_.notify_all ();
    for (std::thread& thread : threads_) {
        if (thread.joinable ())
            thread.join ();
    }
    // Cleared rather than left joined-but-present, so ThreadCount answers zero
    // and RunBatch's cap collapses to the submitter alone.
    threads_.clear ();
}

bool WorkerPool::ClaimLocked (size_t& index)
{
    if (body_ == nullptr || claimed_ >= count_)
        return false;
    index = claimed_++;
    return true;
}

void WorkerPool::RunOne (const std::function<void (size_t)>& body, size_t index)
{
    // A task that throws must not strand the batch: the finished count is what
    // the submitter waits on, so an escaped exception here would hang it. The
    // evaluator already runs node bodies behind the fault barrier, which makes
    // this a second line rather than the only one.
    try {
        body (index);
    }
    catch (...) {
    }
}

void WorkerPool::ThreadMain ()
{
    for (;;) {
        size_t index = 0;
        const std::function<void (size_t)>* body = nullptr;
        {
            std::unique_lock<std::mutex> lock (mutex_);
            workAvailable_.wait (lock, [this] () {
                return stopping_ || (body_ != nullptr && claimed_ < count_ && active_ < maxParallel_);
            });
            if (stopping_)
                return;
            if (!ClaimLocked (index))
                continue;
            ++active_;
            // Read under the lock. The batch is cleared only once every task has
            // finished, and this one has not, but taking a copy of the pointer
            // here means that reasoning is not load-bearing.
            body = body_;
        }

        RunOne (*body, index);

        {
            const std::lock_guard<std::mutex> lock (mutex_);
            --active_;
            ++finished_;
            if (finished_ == count_)
                batchFinished_.notify_all ();
        }
        // A slot came free, so a thread parked on the cap can take it.
        workAvailable_.notify_one ();
    }
}

void WorkerPool::RunBatch (size_t count, size_t maxParallel, const std::function<void (size_t)>& body)
{
    if (count == 0)
        return;

    const std::lock_guard<std::mutex> submit (submitMutex_);

    const size_t cap = maxParallel == 0 ? threads_.size () + 1 : maxParallel;
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        body_ = &body;
        count_ = count;
        claimed_ = 0;
        finished_ = 0;
        // The submitter's slot, held for the whole batch. Releasing it as soon
        // as the caller runs dry would let the pool exceed the cap on the tail
        // of every batch, which is exactly where a parallelism measurement is
        // read.
        active_ = 1;
        maxParallel_ = cap;
    }
    if (cap > 1)
        workAvailable_.notify_all ();

    for (;;) {
        size_t index = 0;
        {
            const std::lock_guard<std::mutex> lock (mutex_);
            if (!ClaimLocked (index))
                break;
        }
        RunOne (body, index);
        {
            const std::lock_guard<std::mutex> lock (mutex_);
            ++finished_;
            if (finished_ == count_)
                batchFinished_.notify_all ();
        }
    }

    {
        std::unique_lock<std::mutex> lock (mutex_);
        batchFinished_.wait (lock, [this] () { return finished_ == count_; });
        body_ = nullptr;
        count_ = 0;
        claimed_ = 0;
        finished_ = 0;
        active_ = 0;
        maxParallel_ = 0;
    }
}

WorkerPool& SharedWorkerPool ()
{
    static WorkerPool pool (DefaultWorkerThreadCount ());
    return pool;
}

void ShutDownWorkerPool ()
{
    // The pool OBJECT outlives this call deliberately. Destroying it would leave
    // a dangling reference in any caller that already holds one, and there is
    // nothing to gain: a stopped pool holds no thread, and a batch submitted
    // afterwards runs inline on its submitter, which is what an evaluation still
    // finishing during teardown needs.
    SharedWorkerPool ().Stop ();
}

} // namespace evp::nodegraph
