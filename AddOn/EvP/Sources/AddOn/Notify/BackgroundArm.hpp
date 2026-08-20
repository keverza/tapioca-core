#ifndef EVP_NOTIFY_BACKGROUNDARM_HPP
#define EVP_NOTIFY_BACKGROUNDARM_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>


// E25 — arming the change observer WITHOUT EVER STALLING ARCHICAD.
//
// THE REQUIREMENT (from the user, 2026-08-03, after the synchronous version hung
// the application): watching the model must be a background process that never
// stops the main Archicad loop. Latency is fine — a viewer may lag the model by
// seconds. A freeze is not, ever, for any duration.
//
// THE PROBLEM. Attaching an observer is ACAPI, so it MUST run on the main thread,
// and there are as many attaches as there are elements. Done in one gate call
// that is a multi-second stall (measured: 3545 ms listing + ~4 ms per element).
// Capping it only trades a freeze for a half-armed watch.
//
// THE SHAPE. A dedicated worker thread submits the work as MANY SMALL SLICES
// through the ordinary MainThreadGate::Invoke, each with a time budget of a few
// milliseconds, sleeping between them. Total wall-clock goes UP; the longest the
// main thread is ever held goes DOWN to one slice. Archicad stays interactive
// throughout, which is the whole point.
//
//   worker: [Invoke 6ms slice] sleep 25ms [Invoke 6ms slice] sleep 25ms ...
//   main:   ...user edits.....^^^^^^^^^^^..user edits......^^^^^^^^^^^...
//
// ⚠️ WHY A THREAD AND NOT A SELF-RESCHEDULING Post. MainThreadGate::Post runs
// the job INLINE when called from the main thread (see its implementation), so a
// slice that re-posted itself would recurse instead of yielding. The worker
// thread is what makes each slice a genuine separate visit.
//
// ⚠️ THE LISTING IS ONE UNINTERRUPTIBLE CALL and cannot be sliced. It is done
// with APIFilt_None rather than APIFilt_In3D on purpose: In3D has to consult the
// 3D model and can force it to GENERATE, which is where the 3545 ms went.
// APIFilt_None only walks the element database. The cost of that choice is more
// elements (2D included) — which is fine here, because attaching them is now
// sliced and cheap, whereas generating the 3D model never can be.
//
// ⚠️ THE THREAD MUST BE STOPPED BEFORE THE ADD-ON UNLOADS. Stop() joins it, and
// FreeData calls Stop() before anything else is torn down. A thread still
// submitting gate jobs into an unloading add-on is the one fatal outcome here.
namespace geomsrv {

class ArmWorker {
public:
    static ArmWorker& Get ();

    struct Progress {
        bool          running   = false;
        bool          done      = false;   // finished a full pass at least once
        bool          gaveUp    = false;   // hit maxSeconds with elements left
        uint32_t      listed    = 0;
        uint32_t      attached  = 0;
        uint32_t      failed    = 0;
        uint32_t      slices    = 0;       // main-thread visits used so far
        int64_t       listMs    = 0;
        // ⚠️ TWO DIFFERENT NUMBERS, and conflating them made the first live run
        // unreadable. `longestHoldMs` is time spent INSIDE the slice, i.e. how
        // long Archicad was actually held — the one the requirement is about.
        // `longestRoundTripMs` is the whole Invoke seen from this thread, which
        // is mostly WAITING FOR THE GATE TO DISPATCH and says how busy Archicad
        // is, not how much we cost it. Run 5 reported 1076 ms as a "main-thread
        // hold" when it was almost entirely dispatch latency.
        int64_t       longestHoldMs      = 0;
        int64_t       longestRoundTripMs = 0;
        int64_t       elapsedMs = 0;
        GS::UniString phase;               // "listing" / "attaching" / "idle" / an error
    };

    // Begins (or restarts) a background arming pass. Returns immediately —
    // nothing ACAPI happens on the caller's thread.
    // `maxSeconds` is a HARD WALL, not a tuning knob. Round 5's pass would have
    // run ~22 minutes on a 12k-element project, poking Archicad twice a second
    // the whole time, and nothing stopped it when the command ended. A pass that
    // cannot finish in a sensible time is a pass that should give up and say so.
    void Start (int64_t sliceMs = 6, int64_t gapMs = 25, int64_t maxSeconds = 60);

    // Stops the pass and JOINS the thread. Safe to call when not running.
    //
    // ⚠️ ONLY WHERE A BOUNDED BLOCK IS ACCEPTABLE — unload, and a restart. Called
    // from the main thread while the worker is inside a gate Invoke, this waits
    // for that Invoke to TIME OUT (the worker is waiting for the very thread now
    // sitting in join()). The worker's slice timeout is deliberately short so
    // that wait is ~1.5 s at worst rather than the gate's default 30 s.
    void Stop ();

    // Flag only, no join — for main-thread callers that must never block, i.e.
    // the project-event handler. The worker unwinds on its own between slices.
    void RequestStop ();

    Progress Snapshot () const;

private:
    ArmWorker () = default;
    ~ArmWorker ();

    void Run (int64_t sliceMs, int64_t gapMs, int64_t maxSeconds);

    std::thread        worker;
    std::atomic<bool>  stopFlag { false };
    std::atomic<bool>  running  { false };
    mutable std::mutex mtx;
    Progress           progress;
};

} // namespace geomsrv

#endif
