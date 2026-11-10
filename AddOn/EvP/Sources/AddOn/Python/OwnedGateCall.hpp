#ifndef TAPIOCA_OWNED_GATE_CALL_HPP
#define TAPIOCA_OWNED_GATE_CALL_HPP

#include <atomic>
#include <memory>
#include <utility>

namespace tapioca {

// Invoke must synchronize completion before returning true. A false return may
// leave work in flight: its inputs must be value captures and its result remains
// owned by the job. Abandonment cannot interrupt an SDK call already running.
template <class Result, class Invoke, class Work> std::shared_ptr<Result> InvokeOwnedGateCall (Invoke invoke, Work work)
{
    auto result = std::make_shared<Result> ();
    auto abandoned = std::make_shared<std::atomic<bool>> (false);
    if (!invoke ([result, abandoned, work = std::move (work)] () mutable {
            if (!abandoned->load ())
                work (*result);
        })) {
        abandoned->store (true);
        return {};
    }
    return result;
}

} // namespace tapioca
#endif
