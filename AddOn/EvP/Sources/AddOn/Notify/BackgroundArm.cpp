#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Notify/BackgroundArm.hpp"
#include "Notify/ChangeTracker.hpp"
#include "Python/MainThreadGate.hpp"
#include "Diagnostics/ApiError.hpp"

#include <algorithm>
#include <chrono>

namespace geomsrv {

namespace {

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// ⚠️ NOT MainThreadGate::DefaultTimeoutMs (30 s). THE DEADLOCK THIS AVOIDS:
// Stop() joins the worker, and Stop() is called FROM THE MAIN THREAD (project
// close, unload, a restart). If the worker is inside Invoke at that moment it is
// waiting for the main thread to dispatch its job — and the main thread is
// sitting in join(). Nothing moves until the Invoke times out, so that timeout
// IS the worst-case freeze. A slice is ~6 ms of work, so 1.5 s is already
// enormously generous, and it bounds the join at 1.5 s instead of 30.
constexpr int SliceTimeoutMs = 1500;

// What one slice reports back. ATOMICS, and shared by value, because a timed-out
// Invoke may still run the job LATER — after this loop iteration has moved on.
// The gate's contract says exactly that, so plain ints here would be a data race
// that only fires when Archicad is busy, i.e. in the only situation that matters.
struct SliceState {
    std::atomic<UIndex>   next      { 0 };
    std::atomic<uint32_t> attached  { 0 };
    std::atomic<uint32_t> failed    { 0 };
    std::atomic<int64_t>  holdMs    { 0 };   // time actually spent on the main thread
    std::atomic<bool>     completed { false };
};

}   // namespace

ArmWorker& ArmWorker::Get ()
{
    static ArmWorker instance;
    return instance;
}

ArmWorker::~ArmWorker ()
{
    Stop ();
}

void ArmWorker::Start (int64_t sliceMs, int64_t gapMs, int64_t maxSeconds)
{
    Stop ();                        // a restart supersedes the pass in flight

    stopFlag.store (false);
    running.store (true);
    {
        std::lock_guard<std::mutex> lock (mtx);
        progress = Progress ();
        progress.running = true;
        progress.phase   = "starting";
    }

    worker = std::thread ([this, sliceMs, gapMs, maxSeconds] { Run (sliceMs, gapMs, maxSeconds); });
}

void ArmWorker::RequestStop ()
{
    // Flag only, NO join — for main-thread callers that must not block, i.e.
    // the project-event handler. The worker notices between slices (it is
    // sleeping most of the time) and unwinds on its own.
    stopFlag.store (true);
}

void ArmWorker::Stop ()
{
    stopFlag.store (true);
    if (worker.joinable ())
        worker.join ();             // bounded by SliceTimeoutMs — see the note there
    running.store (false);
    {
        std::lock_guard<std::mutex> lock (mtx);
        progress.running = false;
    }
}

ArmWorker::Progress ArmWorker::Snapshot () const
{
    std::lock_guard<std::mutex> lock (mtx);
    return progress;
}

void ArmWorker::Run (int64_t sliceMs, int64_t gapMs, int64_t maxSeconds)
{
    const int64_t started = NowMs ();

    // ---- phase 1: list ------------------------------------------------------
    // One uninterruptible call, so it gets a slice to itself, and APIFilt_None
    // rather than APIFilt_In3D — In3D has to consult the 3D model and can force
    // it to GENERATE, which is where the measured 3545 ms went. See the header.
    auto guids   = std::make_shared<GS::Array<API_Guid>> ();
    auto listErr = std::make_shared<std::atomic<GSErrCode>> (NoError);
    auto listed  = std::make_shared<std::atomic<bool>> (false);
    {
        std::lock_guard<std::mutex> lock (mtx);
        progress.phase = "listing";
    }

    const int64_t listStart = NowMs ();
    GS::UniString gateErr;
    const bool gated = evp::MainThreadGate::Get ().Invoke (
        [guids, listErr, listed] {
            listErr->store (ACAPI_Element_GetElemList (API_ZombieElemID, guids.get (), APIFilt_None));
            listed->store (true);
        },
        SliceTimeoutMs * 4, gateErr);     // the listing is the one long job; give it room
    const int64_t listMs = NowMs () - listStart;

    if (!gated || !listed->load () || listErr->load () != NoError) {
        std::lock_guard<std::mutex> lock (mtx);
        progress.listMs    = listMs;
        progress.phase     = (!gated || !listed->load ())
            ? GS::UniString ("the main-thread gate did not dispatch the element listing")
            : evp::DescribeErr (listErr->load ());
        progress.running   = false;
        progress.elapsedMs = NowMs () - started;
        running.store (false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock (mtx);
        progress.listMs         = listMs;
        progress.longestHoldMs      = std::max (progress.longestHoldMs, listMs);
        progress.longestRoundTripMs = std::max (progress.longestRoundTripMs, listMs);
        progress.listed         = (uint32_t) guids->GetSize ();
        progress.phase          = "attaching";
    }

    // ---- phase 2: attach, a few milliseconds at a time ----------------------
    UIndex cursor = 0;
    int    consecutiveTimeouts = 0;

    bool gaveUp = false;
    while (cursor < guids->GetSize () && !stopFlag.load ()) {
        // The hard wall. Better to stop with a partial watch and SAY so than to
        // keep tapping the main thread for twenty more minutes.
        if (NowMs () - started >= maxSeconds * 1000) {
            gaveUp = true;
            std::lock_guard<std::mutex> lock (mtx);
            progress.phase = GS::UniString::Printf (
                "gave up after %ds with %u of %u attached - too big to watch this way; "
                "use guids=[...] or EvP.GetModelDiff",
                (int) maxSeconds, (unsigned) progress.attached, (unsigned) progress.listed);
            break;
        }
        auto st = std::make_shared<SliceState> ();
        st->next.store (cursor);

        const int64_t sliceStart = NowMs ();
        GS::UniString sliceErr;
        const bool ok = evp::MainThreadGate::Get ().Invoke (
            [guids, st, sliceMs] {
                // The budget is checked INSIDE the slice, on the main thread, so
                // the bound is on how long Archicad is actually held — not on how
                // many elements we guessed would fit in that time.
                const int64_t begin = NowMs ();
                UIndex i = st->next.load ();
                for (; i < guids->GetSize (); ++i) {
                    if (NowMs () - begin >= sliceMs)
                        break;
                    const GSErrCode e = ACAPI_Element_AttachObserver ((*guids)[i]);
                    if (e == NoError || e == APIERR_LINKEXIST)
                        st->attached.fetch_add (1);
                    else
                        st->failed.fetch_add (1);
                }
                st->next.store (i);
                st->holdMs.store (NowMs () - begin);
                st->completed.store (true);
            },
            SliceTimeoutMs, sliceErr);
        const int64_t sliceElapsed = NowMs () - sliceStart;

        if (!ok || !st->completed.load ()) {
            // A timed-out slice's counters are NOT trustworthy — the job may run
            // later. Do not advance; retry the same cursor. Re-attaching is
            // idempotent (APIERR_LINKEXIST), so a late duplicate costs nothing.
            ++consecutiveTimeouts;
            if (consecutiveTimeouts >= 5) {
                std::lock_guard<std::mutex> lock (mtx);
                progress.phase = "the main-thread gate stopped dispatching; arming paused";
                break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (gapMs * 4));
            continue;
        }
        consecutiveTimeouts = 0;

        const UIndex advanced = st->next.load ();
        {
            std::lock_guard<std::mutex> lock (mtx);
            progress.attached += st->attached.load ();
            progress.failed   += st->failed.load ();
            ++progress.slices;
            progress.longestHoldMs      = std::max (progress.longestHoldMs, st->holdMs.load ());
            progress.longestRoundTripMs = std::max (progress.longestRoundTripMs, sliceElapsed);
            progress.elapsedMs          = NowMs () - started;
        }

        if (advanced == cursor)      // no forward progress: refuse to spin forever
            break;
        cursor = advanced;

        // THE YIELD, and the reason this is a background process rather than a
        // slow foreground one: between slices the main thread is entirely the
        // user's. A longer gap means less interference and more latency — and
        // the requirement explicitly prefers latency over interference.
        std::this_thread::sleep_for (std::chrono::milliseconds (gapMs));
    }

    // Archicad's own tally, once, at the end. Skipped when stopping — the point
    // of a stop is to get off the main thread, not to take one more trip.
    uint32_t observedCount = 0;
    if (!stopFlag.load ()) {
        auto observed = std::make_shared<std::atomic<uint32_t>> (0);
        GS::UniString err;
        evp::MainThreadGate::Get ().Invoke ([observed] {
            GS::Array<API_Guid> heads;
            if (ObservedElements (heads) == NoError)
                observed->store ((uint32_t) heads.GetSize ());
        }, SliceTimeoutMs, err);
        observedCount = observed->load ();
    }

    {
        std::lock_guard<std::mutex> lock (mtx);
        progress.done      = (cursor >= guids->GetSize ());
        progress.gaveUp    = gaveUp;
        progress.running   = false;
        progress.elapsedMs = NowMs () - started;
        if (progress.phase == "attaching")
            progress.phase = progress.done ? "idle" : "stopped";
        if (observedCount == 0)
            observedCount = progress.attached;      // gate refused the tally; ours will do
    }
    ChangeTracker::Get ().SetWatching (true, observedCount);
    running.store (false);
}

} // namespace geomsrv
