// ArchViz/ExtractionLiveLoop -- the extraction worker's observer arming and its watch loop.
//
// ⚠️ MOVED OUT OF ExtractionThread.cpp VERBATIM (2026-11-22, #14) because that file sat at the
// 1,000-line cap and the scene cache needs room in the pass. The reasoning is in
// ExtractionThread.hpp (PHASE 7 -- LIVE SYNC) and is not repeated here.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ExtractionSlice.hpp" // SliceTimeoutMs
#include "ArchViz/ArchVizLog.hpp"
#include "Geometry/GeometryExtractor.hpp" // ExpandElementAndParts
#include "Notify/ChangeTracker.hpp"       // the viewer is its SECOND consumer - own cursor
#include "Python/MainThreadGate.hpp"
#include "Diagnostics/ApiError.hpp" // DescribeErr - never print a bare GSErrCode

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace geomsrv {
namespace archviz {

namespace {

using extractionslice::SliceTimeoutMs;

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// How long an acquire may take, in LIVE mode, before the loop stops doing them.
//
// ⚠️ THE POINT IS TO FAIL ONCE, NOT EVERY TIME. Editing in a 2D window
// invalidates the 3D model, so every re-extraction regenerates it — each one
// another multi-second stall, on every edit, for as long as the viewer is
// watching. One slow pass is a cost; the same cost repeating forever is the
// freeze the user reported. Past this, live sync pauses itself and says so.
constexpr int64_t LiveAcquireBudgetMs = 3000;

} // namespace

// ---------------------------------------------------------------------------
// ARMING. Attach a change observer to every element the pass actually extracted.
//
// ⚠️ `scope=visible` BY CONSTRUCTION — the list is what the 3D SIGHT yielded, so
// hidden layers, other storeys and filtered-out elements are never armed. They
// cannot change a pixel, and the measured alternative (attach to every element
// in the database) managed ~9 attaches/second, i.e. ~22 minutes on a
// 12k-element project. See EvP.WatchModel's own warning; this is that decision,
// applied by construction rather than by a flag.
//
// ⚠️ SLICED, like everything else here. ACAPI_Element_AttachObserver is main
// thread, and a visible set can still be thousands of elements.
// ---------------------------------------------------------------------------
void ExtractionWorker::ArmObservers (const std::vector<std::string>& guids, const Options& opt)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.phase = "arming";
    }

    // ⚠️⚠️ INSTALL THE HANDLERS FIRST, AND THIS IS THE BUG THE 2026-08-07 RUN
    // ACTUALLY HAD. `ACAPI_Element_AttachObserver` LINKS an element to this
    // add-on; it does NOT make Archicad call anything. Delivery needs
    // `ACAPI_Element_InstallElementObserver` (and `CatchNewElement` for
    // creations), which until now was called from ONE place: EvP.WatchModel,
    // the Python path. Live sync attached observers to a few hundred elements,
    // reported `armed: 388`, went to "watching", and could never hear a single
    // notification — a watch that is armed, healthy-looking and DEAF.
    //
    // Nothing in the reported symptoms could distinguish that from "the model is
    // not changing", which is why the first two diagnoses were wrong. It is
    // idempotent, so calling it on every arm costs one main-thread hop.
    auto installErr = std::make_shared<std::atomic<GSErrCode>> (NoError);
    {
        GS::UniString err;
        if (!evp::MainThreadGate::Get ().Invoke ([installErr] { installErr->store (InstallChangeObserver ()); },
                                                 SliceTimeoutMs, err) ||
            installErr->load () != NoError) {
            ChangeTracker::Get ().SetWatching (false, 0);
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.armed = 0;
            progress_.phase = "the change observer could not be INSTALLED, so no edit can ever be "
                              "reported however many elements are attached (" +
                              std::string (evp::DescribeErr (installErr->load ()).ToCStr ().Get ()) + ")";
            ArchVizLog ("extraction: " + progress_.phase);
            return;
        }
    }

    auto all = std::make_shared<std::vector<std::string>> (guids);
    size_t cursor = 0;
    uint32_t attached = 0;
    uint32_t refused = 0;

    while (cursor < all->size () && !stopFlag_.load ()) {
        auto next = std::make_shared<std::atomic<size_t>> (cursor);
        auto attachedNow = std::make_shared<std::atomic<uint32_t>> (0);
        auto refusedNow = std::make_shared<std::atomic<uint32_t>> (0);
        auto completed = std::make_shared<std::atomic<bool>> (false);
        const int64_t sliceMs = opt.sliceMs;

        GS::UniString err;
        const bool ok = evp::MainThreadGate::Get ().Invoke (
            [all, next, attachedNow, refusedNow, completed, sliceMs] {
                const int64_t begin = NowMs ();
                size_t i = next->load ();
                for (; i < all->size (); ++i) {
                    if (NowMs () - begin >= sliceMs)
                        break;
                    const API_Guid guid = APIGuidFromString ((*all)[i].c_str ());
                    const GSErrCode e = ACAPI_Element_AttachObserver (guid);
                    // APIERR_LINKEXIST = already watched: the normal case on a
                    // re-arm, and not a failure.
                    if (e == NoError || e == APIERR_LINKEXIST)
                        attachedNow->fetch_add (1);
                    else
                        // ⚠️ COUNTED, NOT SWALLOWED. The modeler yields sub-part
                        // GUIDs (stair treads, curtain-wall panels) that the
                        // element database may refuse to observe; "armed 388,
                        // refused 0" and "armed 12, refused 376" describe
                        // completely different watches and used to print the
                        // same way.
                        refusedNow->fetch_add (1);
                }
                next->store (i);
                completed->store (true);
            },
            SliceTimeoutMs, err);

        if (!ok || !completed->load ())
            break; // same discipline as an extraction slice

        const size_t advanced = next->load ();
        attached += attachedNow->load ();
        refused += refusedNow->load ();
        if (advanced <= cursor)
            break;
        cursor = advanced;

        std::this_thread::sleep_for (std::chrono::milliseconds (opt.gapMs));
    }

    // ⚠️ A WATCH OF NOTHING IS THE FAILURE THIS REPORTS. Everything downstream
    // looks identical whether the observer is armed or not — the viewer simply
    // never updates — so the count is the only thing that separates "the model
    // is not changing" from "we are not listening".
    ChangeTracker::Get ().SetWatching (attached > 0, attached);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.armed = attached;
        progress_.armRefused = refused;
        progress_.handlersInstalled = true;
        progress_.phase = (attached > 0) ? "watching" : "watching nothing - no elements armed";
    }
    ArchVizLog ("extraction: handlers installed; armed " + std::to_string (attached) + " of " +
                std::to_string (all->size ()) + " extracted elements (" + std::to_string (refused) + " refused)");
}

// ---------------------------------------------------------------------------
// THE WATCH LOOP. Plan §6.5, and the phase Part I exists for: move a wall in
// Archicad, watch it move in the viewer.
// ---------------------------------------------------------------------------
void ExtractionWorker::RunLiveLoop (const Options& opt)
{
    ChangeTracker& tracker = ChangeTracker::Get ();

    // ⚠️ OUR OWN CURSOR. The bus (EvP.TakeChanges) has its own; sharing one
    // would mean whichever drained first hid the change from the other. See
    // ChangeTracker's consumer block — this is the second consumer that made
    // per-consumer cursors a prerequisite rather than a nicety.
    const ChangeTracker::ConsumerId me = tracker.RegisterConsumer ("archviz");

    // What the LAST pass spent waiting for the model. Read at the top of the
    // next iteration by the freeze guard below; seeded from the initial full
    // pass, so a live sync started from a 2D window pauses BEFORE its first
    // re-extraction rather than after.
    int64_t lastAcquireMs = 0;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        lastAcquireMs = progress_.acquireMs;
    }

    while (!stopFlag_.load ()) {
        std::this_thread::sleep_for (std::chrono::milliseconds (opt.pollMs));
        if (stopFlag_.load ())
            break;

        std::vector<ChangeTracker::Entry> entries;
        size_t remaining = 0;
        bool overflowed = false;
        tracker.TakeDirtyFor (me, 4096, /*peek*/ true, entries, remaining, overflowed);

        {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.dirtyPending = uint32_t (entries.size () + remaining);
        }

        if (entries.empty ()) {
            // ⚠️ A DROPPED WATCH IS NOT AN IDLE MODEL. IsWatching goes false on a
            // project event, and from that moment nothing will ever be reported
            // again — the viewer would sit showing a stale building and looking
            // perfectly healthy. A full pass rebuilds and re-arms.
            if (!tracker.IsWatching ()) {
                std::vector<std::string> extracted;
                if (RunPass (opt, /*full*/ true, std::set<std::string> (), &extracted) && !stopFlag_.load ())
                    ArmObservers (extracted, opt);
            }
            continue;
        }

        // ⚠️ LET THE EDIT FINISH. A drag notifies per frame; re-extracting on
        // every notification competes with the user's own edit for the main
        // thread and produces a viewer that lags precisely while it is being
        // watched. IdleMs is the model going quiet, not a timer of ours.
        const int64_t idle = tracker.IdleMs ();
        if (idle >= 0 && idle < opt.settleMs)
            continue;

        // When the change actually happened, so the reported latency is
        // edit -> pixel rather than merely how long our own pass took.
        const int64_t changedAt = NowMs () - (idle > 0 ? idle : 0);

        // ⚠️ OVERFLOW OR AN OPAQUE ENTRY MEANS FULL, and the branch comes BEFORE
        // the guid list. `overflowed` says the dirty set hit its cap and dropped
        // elements — the list is not the whole story. An opaque entry (null
        // guid) is EvP's own write or a project event: identity unknown, and
        // never mappable to a mesh.
        bool wantFull = overflowed;
        for (const ChangeTracker::Entry& e : entries) {
            if (e.eventId == ChangeTracker::OpaqueEventId) {
                wantFull = true;
                break;
            }
        }

        // ⚠️ THE FREEZE GUARD, and it is the lesson of the 2026-08-07 run. If
        // the previous pass had to wait on Archicad REGENERATING its 3D model,
        // the next one will too — editing in a 2D window invalidates the 3D, so
        // "one slow pass" becomes a multi-second stall on every single edit, for
        // as long as the viewer watches. Live sync pauses itself instead, and
        // says which window to be in to make it cheap again. Nothing here can
        // make the regeneration fast; what it can do is stop paying for it
        // repeatedly without telling anyone.
        if (lastAcquireMs > LiveAcquireBudgetMs) {
            std::lock_guard<std::mutex> lock (mutex_);
            progress_.phase = "live sync PAUSED: the last refresh spent " + std::to_string (lastAcquireMs) +
                              " ms waiting for Archicad to regenerate "
                              "its 3D model, and every edit would cost that again. Work with the 3D "
                              "window open (it stays generated), then start live sync again.";
            ArchVizLog ("extraction: " + progress_.phase);
            break;
        }

        bool ok = false;
        std::vector<std::string> extracted;
        if (wantFull) {
            ok = RunPass (opt, /*full*/ true, std::set<std::string> (), &extracted);
            if (ok && !stopFlag_.load ())
                ArmObservers (extracted, opt);
        }
        else {
            // ⚠️ EXPAND TO SUB-PARTS FIRST. Archicad notifies about the PARENT
            // stair / railing / curtain wall; the modeler only ever yields its
            // sub-parts, under different GUIDs. Without this the pass looks up a
            // GUID the model does not contain, finds nothing, removes nothing,
            // and leaves the old stair on screen while reporting success.
            auto expanded = std::make_shared<std::set<std::string>> ();
            auto guids = std::make_shared<std::vector<API_Guid>> ();
            for (const ChangeTracker::Entry& e : entries)
                guids->push_back (e.guid);

            GS::UniString err;
            const bool gated = evp::MainThreadGate::Get ().Invoke (
                [guids, expanded] {
                    for (const API_Guid& g : *guids)
                        ExpandElementAndParts (g, *expanded);
                },
                SliceTimeoutMs, err);
            if (!gated)
                continue; // try again next tick; nothing was drained

            ok = RunPass (opt, /*full*/ false, *expanded, nullptr);
        }

        // ⚠️ DRAIN ONLY NOW. TakeDirtyFor advances a cursor and nothing can put
        // an entry back; committing before the pass succeeded would lose the
        // change entirely on a failure — silently, because a failed pass leaves
        // the OLD geometry on screen, looking correct.
        if (ok) {
            std::vector<ChangeTracker::Entry> drained;
            size_t rest = 0;
            bool of = false;
            tracker.TakeDirtyFor (me, entries.size (), /*peek*/ false, drained, rest, of);

            std::lock_guard<std::mutex> lock (mutex_);
            progress_.lastSyncMs = NowMs () - changedAt;
            progress_.dirtyPending = uint32_t (rest);
            lastAcquireMs = progress_.acquireMs;
        }
        else {
            // A pass that could not run at all — the acquire timed out, or the
            // model came back empty. Both are already spelled out in `phase`,
            // and both will repeat on the next tick, so the loop stops rather
            // than hammering a project that cannot answer.
            std::lock_guard<std::mutex> lock (mutex_);
            ArchVizLog ("extraction: live sync stopping - " + progress_.phase);
            break;
        }
    }

    // ⚠️ UNREGISTER, UNCONDITIONALLY. A registered consumer that never drains
    // pins the dirty set: nothing can be collected past its cursor, the set
    // climbs to MaxDirty, and the PYTHON watcher starts reporting overflow. A
    // closed viewer must cost the rest of EvP nothing.
    tracker.UnregisterConsumer (me);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        progress_.live = false;
        progress_.phase = "stopped watching";
    }
}

} // namespace archviz
} // namespace geomsrv
