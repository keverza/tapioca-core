#ifndef EVP_SUNSTUDY_CPUTRAVERSALWORK_HPP
#define EVP_SUNSTUDY_CPUTRAVERSALWORK_HPP

#include "SunStudy/CpuTraversal.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace evp::sunstudy {

// Surface-ordered rays have very unequal cost (clear rays can walk the entire
// BVH). Claim small contiguous batches instead of leaving one static shard to
// finish alone. Only the submitting thread invokes the cancellation callback.
template <typename Body>
bool RunCpuTraversal (size_t count, size_t maxParallel, const std::function<bool ()>& isCancelled, const Body& body)
{
    constexpr size_t kBatchRays = 256;
    std::atomic<size_t> next { 0 };
    std::atomic<bool> stopped { false };
    const auto run = [&] (bool submitting) {
        while (!stopped.load (std::memory_order_relaxed)) {
            if (submitting && isCancelled && isCancelled ()) {
                stopped.store (true, std::memory_order_relaxed);
                break;
            }
            const size_t first = next.fetch_add (kBatchRays, std::memory_order_relaxed);
            if (first >= count)
                break;
            body (first, std::min (count, first + kBatchRays));
        }
    };
    if (isCancelled && isCancelled ())
        return false;
    std::vector<std::thread> workers;
    const size_t threads = ChooseThreadCount (count, maxParallel);
    workers.reserve (threads - 1);
    try {
        for (size_t i = 1; i < threads; ++i)
            workers.emplace_back (run, false);
        run (true);
    }
    catch (...) {
        stopped.store (true, std::memory_order_relaxed);
        for (auto& worker : workers)
            worker.join ();
        throw;
    }
    for (auto& worker : workers)
        worker.join ();
    return !stopped.load () && (!isCancelled || !isCancelled ());
}

} // namespace evp::sunstudy
#endif
