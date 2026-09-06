#ifndef EVP_MAINTHREADGATE_HPP
#define EVP_MAINTHREADGATE_HPP

// The Zone B/C -> Zone A funnel: the ONE place worker-thread code crosses onto
// Archicad's main thread. Every ACAPI call a script makes ends up here.
//
// Built on what P1 spike A measured, not on assumption:
//   * ACAPI_AddOnAddOnCommunication_CallFromEventLoop is ASYNCHRONOUS — it queues
//     and returns in ~0.02ms. So Invoke() must rendezvous on its own condition
//     variable; the return of CallFromEventLoop means nothing.
//   * The rendezvous MUST have a timeout: "posted but never dispatched" is the
//     documented historical failure mode (Archicad's own Python palette blocking
//     the loop) and is otherwise indistinguishable from success.
//   * Round trips cost ~0.6-8ms (mean ~3ms), dispatch-when-idle. Per-element
//     marshaling does not scale — prefer batching over chatty calls.
//   * A live ACAPI_UserInput_* pick BLOCKS the gate until the user acts
//     (measured 4/4). Nothing else marshals meanwhile.
//
// Spike evidence: archive/docs/evp-command-system-plan.md, "P1 spike A/B".

#include "ACAPinc.h"
#include "UniString.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace evp {

class MainThreadGate {
  public:
    static MainThreadGate& Get ();

    // RegisterInterface / Initialize plumbing.
    static GSErrCode RegisterServices ();
    static GSErrCode InstallHandlers ();

    // Call from Initialize, on the main thread.
    void RecordMainThread ();
    bool IsMainThread () const;

    // Rejects new work and wakes queued Invoke callers before APX teardown joins
    // worker threads. Must run on the main thread while the module is still live.
    void BeginShutdown ();

    // ⚠️ CONTRACT for `fn` in both calls below: it MUST be self-contained —
    // capture by VALUE, never by reference to a caller local. On timeout Invoke
    // returns while the job may still be queued, and Post never waits at all, so
    // the gate can run `fn` after the caller's frame is gone. A by-reference
    // capture is a use-after-free that only fires when the gate is slow.
    // Invoke makes a best-effort revoke on timeout, but it is a race, not a
    // guarantee.

    // Runs `fn` on the main thread and BLOCKS until it has completed.
    // Called ON the main thread, it runs `fn` inline — posting and waiting there
    // would deadlock against the very loop that has to dispatch it.
    // Returns false on timeout (gate not dispatching) or a post failure.
    //
    // ⚠️ NEVER Invoke a job that opens a MODAL dialog or a user-input pick. Such
    // a job does not return until the USER acts, so it holds the gate for human
    // time and this call reports a bogus timeout while everything is in fact
    // fine. (Observed: a result alert logged "posted but never dispatched — job
    // already in flight" purely because the user had not clicked OK yet.)
    // Use Post for those.
    bool Invoke (const std::function<void ()>& fn, int timeoutMs, GS::UniString& error);

    // Fire-and-forget. For output/telemetry, where a ~3ms round trip per line
    // would be absurd. No completion guarantee, no ordering guarantee against
    // Invoke.
    bool Post (const std::function<void ()>& fn, GS::UniString& error);

    static constexpr int DefaultTimeoutMs = 30000;

    // ---- WHY THE GATE IS OR IS NOT DISPATCHING ----------------------------
    //
    // ⚠️ THIS EXISTS BECAUSE "the main-thread gate stopped dispatching" IS
    // TRUE AND USELESS ON ITS OWN. It is the symptom of something else holding
    // Archicad's UI thread, and the message names neither what nor for how long.
    // Three live failures were spent guessing at it; these numbers are what turn
    // the next one into a reading rather than a theory.
    //
    // ⚠️ AND IT IS READABLE FROM A WORKER THREAD, WHICH IS THE WHOLE POINT.
    // A diagnostic that needed the main thread could not observe a stalled main
    // thread - it would queue behind exactly the thing it was sent to measure.
    // Every field here is an atomic and Snapshot() takes no lock the UI thread
    // could be holding, so Tapioca.MainThreadGateState answers over loopback
    // while Archicad is frozen.
    //
    // The discriminator it provides: if `posted` keeps climbing while
    // `dispatched` does not, the event loop is not running our handler and the
    // UI thread is busy or blocked. If both climb, the gate is healthy and the
    // fault is elsewhere.
    struct Stats {
        uint64_t posted = 0;
        uint64_t dispatched = 0;
        // Invoke called ON the main thread runs inline and never queues. A high
        // count here with a low `posted` means the work is not crossing at all.
        uint64_t inlineRuns = 0;
        uint64_t timeouts = 0;
        uint64_t postFailures = 0;
        // What is waiting right now.
        uint64_t queueDepth = 0;
        // Milliseconds since the last successful dispatch. The number that says
        // "the loop has been dead this long".
        int64_t msSinceLastDispatch = -1;
        int64_t msSinceLastPost = -1;
        // The longest a job has ever waited between being posted and running.
        int64_t longestWaitMs = 0;
        bool shuttingDown = false;
        bool mainThreadKnown = false;

        // ---- who is holding the main thread -------------------------------
        // The native command currently executing ON the main thread, if any, and
        // for how long. This is the field that NAMES the culprit instead of
        // leaving it to be inferred: a gate that stopped dispatching while
        // "GraphEvaluate" has been running for 40 seconds is not a mystery.
        std::string mainThreadCommand;
        int64_t mainThreadCommandMs = -1;
    };

    Stats Snapshot () const;

    // Called by the command dispatcher around anything it runs ON the main
    // thread. Empty name clears it.
    static void NoteMainThreadCommand (const char* name);

  private:
    MainThreadGate () = default;
};

} // namespace evp

#endif
