#ifndef EVP_RUNCANCEL_HPP
#define EVP_RUNCANCEL_HPP

// E9 — the one cancel token for the command currently running.
//
// A command runs on a DETACHED worker (ControlPalette::RunSelected). Before E9
// nothing could signal it: closing the palette did not stop it, there was no
// timeout, and a runaway loop wedged the session with no way out. This is the
// single piece of shared state that fixes that.
//
// Deliberately dependency-free — no ACAPI, no DG, no GS types. The palette
// (main thread, DG events) SETS it; the dispatcher and the external runner
// (worker threads) READ it. Making it a leaf header is what lets all three
// include it without a cycle, and what keeps ACAPI out of the readers.
//
// GENERATION, not just a bool. Runs serialize but overlap at the edges: a
// cancel from a panel close, or a timeout, can arrive after the run it was
// meant for already finished. Every trip and every query is gated on the
// generation the run captured at start, so a late cancel can never kill the
// NEXT run. This is the one bug that would be a heisenbug; everything else here
// is recoverable.
//
// Cooperative, and honest about it: nothing here interrupts code that neither
// polls nor talks to Archicad. What it does guarantee is that after a cancel
//   * every subsequent bus call fails with a "Cancelled" envelope (ApiDispatcher),
//     which surfaces in Python as evp.Cancelled — so any command that touches
//     Archicad at all unwinds without opting in;
//   * evp.runtime.check_cancel() raises at the next checkpoint (EvP.PollCancel);
//   * an external subprocess is TerminateProcess'd (ExternalRunner).
// A pure-compute embedded loop that does neither still has to call
// check_cancel() itself. That is the whole contract.

#include <atomic>
#include <cstdint>

namespace evp {

// Why the current run is stopping. Kept as a plain enum so the whole token can
// be lock-free atomics — a reason STRING would need a mutex, and this is read
// on every bus call.
enum class CancelReason {
    None = 0,
    StopButton,     // the user pressed Stop in the palette
    PanelClosed,    // the user closed the EvP palette
    Timeout,        // the command's declared timeout_s elapsed
};

class RunCancel {
public:
    static RunCancel& Get ();

    // ---- owned by the palette (main thread) -------------------------------

    // Starts a new run and returns its generation. Clears any stale cancel.
    // `timeoutSeconds` <= 0 means no timeout.
    uint64_t BeginRun (double timeoutSeconds);

    // Ends the run with the given generation. A stale call (the run already
    // moved on) is a no-op, so a late completion cannot clear a live run.
    void EndRun (uint64_t generation);

    // Trip the current run, whatever it is. No-op when nothing is running, so
    // closing the palette with no command running costs nothing.
    void Request (CancelReason reason);

    // ---- read from anywhere, including worker threads ---------------------

    bool     IsRunning () const { return running.load (); }
    uint64_t Generation () const { return generation.load (); }

    // Has the CURRENT run been cancelled? Also enforces the deadline: the
    // timeout is evaluated lazily here rather than by a watchdog thread, so the
    // pollers already in place (the bus, EvP.PollCancel, the external drain
    // loop) are what make it fire. No extra thread to own or shut down.
    bool IsCancelled ();

    // Same, but only if `generation` is still the live run — for a worker that
    // captured its generation at start and must not observe a later run's state.
    bool IsCancelled (uint64_t forGeneration);

    CancelReason Reason () const { return reason.load (); }

    // A short phrase for the status line / log, e.g. "stopped by the user".
    static const char* ReasonText (CancelReason reason);

private:
    RunCancel () = default;

    std::atomic<uint64_t>     generation { 0 };
    std::atomic<bool>         running    { false };
    std::atomic<bool>         cancelled  { false };
    std::atomic<CancelReason> reason     { CancelReason::None };

    // steady_clock ticks (a monotonic count of milliseconds since the run
    // started is all the deadline needs). 0 == no deadline.
    std::atomic<int64_t> startMs   { 0 };
    std::atomic<int64_t> timeoutMs { 0 };
};

}   // namespace evp

#endif
