#ifndef EVP_GATEWAIT_HPP
#define EVP_GATEWAIT_HPP

// How a caller of MainThreadGate waits for its job, when it can be told to stop.
//
// ⚠️ THE MAIN THREAD CANNOT SERVE A JOB WHILE IT IS JOINING THE THREAD THAT
// POSTED IT. A worker blocked in Invoke waits for the main thread; a main thread
// blocked in that worker's join waits for the worker. Neither moves until the
// Invoke times out, and for the extraction's model acquire that is 20 s.
// Measured 2026-11-01 (#114): two viewer opens from the menu each sat exactly 20 s
// with Archicad frozen, then the next pass acquired the same model in 45 ms.
// Archicad was not generating anything; it was waiting for itself.
//
// So a stoppable waiter watches its stop flag as well as the job. Once the flag
// is set and the job is STILL QUEUED, it revokes the job and returns at once:
// the job never ran, nothing was half-built, and the joiner gets its thread
// back. A job that has already been DISPATCHED cannot be revoked, and is waited
// for exactly as before - it is running on the main thread, so the main thread
// is not the one joining.
//
// Pure and header-only so the shape of that deadlock is testable offline
// (tests/cpp/test_gatewait.cpp); MainThreadGate.cpp supplies the three hooks.

#include <algorithm>
#include <chrono>

namespace evp {
namespace gatewait {

enum class Outcome {
    Completed, // the job ran and finished
    TimedOut,  // the deadline passed first; the job may still be queued or running
    Abandoned, // the stop flag was set and the job was revoked before it ran
};

// How often a stoppable waiter looks at its flag. Short against any join a user
// sits through, long against the cost of a queue scan.
constexpr int kStopPollMs = 10;

// `waitStep (ms)` blocks up to `ms` and returns true once the job is done;
// `stopped ()` reads the caller's stop flag; `revoke ()` removes the job from the
// queue and returns true only if it was still there (i.e. it never ran).
template <class WaitStep, class Stopped, class Revoke>
Outcome Wait (int timeoutMs, WaitStep&& waitStep, Stopped&& stopped, Revoke&& revoke)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now () + std::chrono::milliseconds (std::max (timeoutMs, 0));
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - Clock::now ()).count ();
        const int step = int (std::max<long long> (0, std::min<long long> (left, kStopPollMs)));
        if (waitStep (step))
            return Outcome::Completed;
        if (stopped () && revoke ())
            return Outcome::Abandoned;
        if (Clock::now () >= deadline)
            return Outcome::TimedOut;
    }
}

} // namespace gatewait
} // namespace evp

#endif
