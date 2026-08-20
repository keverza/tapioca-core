#include "RunCancel.hpp"

#include <chrono>

namespace {

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

}   // namespace

namespace evp {

RunCancel& RunCancel::Get ()
{
    static RunCancel instance;
    return instance;
}

uint64_t RunCancel::BeginRun (double timeoutSeconds)
{
    // Order matters: clear the previous run's verdict BEFORE publishing the new
    // generation, or a reader that catches the new generation with the old
    // `cancelled` still set would report a cancel that never happened.
    cancelled.store (false);
    reason.store (CancelReason::None);
    startMs.store (NowMs ());
    timeoutMs.store (timeoutSeconds > 0.0 ? (int64_t) (timeoutSeconds * 1000.0) : 0);
    running.store (true);
    return generation.fetch_add (1) + 1;
}

void RunCancel::EndRun (uint64_t forGeneration)
{
    // A completion from an older run must not clear the flag for the live one.
    if (generation.load () != forGeneration)
        return;
    running.store (false);
    timeoutMs.store (0);
    // `cancelled`/`reason` are deliberately LEFT SET: RunSelected reads them
    // after the worker returns to decide whether to report "cancelled" instead
    // of "FAILED". BeginRun clears them for the next run.
}

void RunCancel::Request (CancelReason why)
{
    if (!running.load ())
        return;         // nothing to stop; do not arm a cancel for the next run
    // First reason wins — if a timeout and a Stop press race, the status should
    // name whichever actually stopped the command first.
    CancelReason expected = CancelReason::None;
    reason.compare_exchange_strong (expected, why);
    cancelled.store (true);
}

bool RunCancel::IsCancelled ()
{
    if (!running.load ())
        return false;
    if (cancelled.load ())
        return true;

    const int64_t limit = timeoutMs.load ();
    if (limit > 0 && (NowMs () - startMs.load ()) > limit) {
        Request (CancelReason::Timeout);
        return true;
    }
    return false;
}

bool RunCancel::IsCancelled (uint64_t forGeneration)
{
    if (generation.load () != forGeneration)
        return false;
    return IsCancelled ();
}

const char* RunCancel::ReasonText (CancelReason why)
{
    switch (why) {
        case CancelReason::StopButton:  return "stopped by the user";
        case CancelReason::PanelClosed: return "the EvP panel was closed";
        case CancelReason::Timeout:     return "the command's timeout elapsed";
        default:                        return "cancelled";
    }
}

}   // namespace evp
