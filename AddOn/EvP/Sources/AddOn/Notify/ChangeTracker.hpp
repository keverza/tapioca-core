#ifndef EVP_NOTIFY_CHANGETRACKER_HPP
#define EVP_NOTIFY_CHANGETRACKER_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// E25 — the model CHANGE TOKEN.
//
// THE PROBLEM. Nothing in EvP could answer "has the model changed since I last
// looked", so every viewer and every report had to be re-run by hand. The first
// consumer is Commands/ModelViewer: a geometry edit should refresh the viewer
// without the user re-running the command.
//
// THE SHAPE. Archicad's element observer bumps a MONOTONIC COUNTER and marks the
// element DIRTY. A consumer polls EvP.GetChangeToken — which touches no ACAPI and
// therefore takes no gate hop — then drains the queue with EvP.TakeChanges when
// it is ready to do the work.
//
// ⚠️ THE CALLBACK RUNS ON ARCHICAD'S OWN THREAD, DURING EDITING — while the user
// is dragging a wall. It must do almost nothing: no gate call, no snapshot
// rebuild, no ACAPI read beyond the elem head it is handed. Record() is therefore
// a lock and one map write, and NOTHING else. Never make it read the element.
//
// ⚠️ A DIRTY SET, NOT AN EVENT LOG — and the difference is not cosmetic. The
// pipeline the consumer wants is:
//
//     element changed -> mark it DIRTY -> queue -> update the WebUI, batched
//
// so what matters is WHICH ELEMENTS are stale, not how many times each was
// touched. A drag fires a notification per frame: an event log records the same
// wall a thousand times and evicts every OTHER changed element to make room,
// then reports "I lost track" — forcing a full refresh when in truth ONE element
// moved. A set collapses those thousand events into one dirty entry, so the
// storm costs one slot and the other elements survive.
//
// Coalescing therefore happens for free, at the source. The consumer drains the
// set when it likes (TakeDirty) and gets each element exactly once, which is
// precisely "batch them together if many changes are done quickly".
//
// ⚠️ THE TOKEN IS PER-SESSION, NOT PER-PROJECT. It never resets on a project
// change; OnProjectEvent() BUMPS it instead, so a consumer that opened a
// different document sees the same "something changed" signal it already handles
// rather than a token that mysteriously went backwards.
namespace geomsrv {

class ChangeTracker {
public:
    // One dirty element. `eventId` is the LAST thing that happened to it — a
    // delete after a change is a delete, which is what a consumer must act on.
    struct Entry {
        API_Guid  guid;
        Int32     eventId;      // API_ElementDBEventID, raw — see APIdefs_Callback.h
        uint64_t  token;        // the token when it was last touched
    };

    // Distinct dirty ELEMENTS, not events. Reached only by a project genuinely
    // changing this many separate elements between drains, at which point a
    // wholesale refresh is cheaper than the list anyway.
    static constexpr size_t MaxDirty = 20000;

    static ChangeTracker& Get ();

    // ---- writer side: ARCHICAD'S THREAD, INSIDE THE OBSERVER CALLBACK --------
    // Bounded work only. See the ⚠️ above before adding anything here.
    void Record (const API_Guid& guid, Int32 eventId);

    // A project-level event (new / open / close). Bumps the token with a null
    // guid so a consumer refreshes, and drops the watch: the observers were
    // attached to elements of a database that is gone.
    void OnProjectEvent ();

    // A write EvP ITSELF made. Bumps the token with a null guid — "something
    // changed, identity unknown".
    //
    // ⚠️ THIS IS NOT BELT-AND-BRACES, IT IS THE ONLY SIGNAL. Proved live
    // 2026-08-03: the probe wrote an element's ID box through the bus while that
    // exact element was confirmed observed, and NO notification arrived — while a
    // human edit to the SAME element in the SAME session did notify. Archicad
    // does not tell an add-on about the add-on's own changes. Without this call a
    // viewer would silently miss every change made by another EvP command, which
    // is precisely the case a scripted workflow hits most.
    void RecordSelfWrite ();

    // The event id used for "something changed but we cannot name it" — a self
    // write or a project event. Reported as "opaque"; the guid is null.
    static constexpr Int32 OpaqueEventId = 1000;

    // ---- consumers ----------------------------------------------------------
    //
    // ⚠️ THERE IS MORE THAN ONE CONSUMER NOW, AND THE OLD DRAIN COULD NOT
    // SURVIVE THAT. `TakeDirty` used to ERASE what it returned, so whichever
    // consumer drained first HID the change from every other one — the Python
    // side (EvP.TakeChanges) and the bgfx viewer's live sync would each see
    // roughly half the edits, non-deterministically, and each would look like an
    // intermittent bug in itself. Tracked as PLAT-CHANGETRACKER-CURSORS; the
    // bgfx plan makes it a prerequisite of the second consumer, not a follow-up.
    //
    // THE SHAPE. Every entry already carried the token it was last touched at.
    // A consumer is now just a CURSOR over that token: "dirty for me" is
    // `entry.token > myCursor`, and draining advances the cursor instead of
    // erasing. An entry is erased only once EVERY registered consumer has passed
    // it. Re-touching an entry gives it a new, higher token, so it becomes dirty
    // again for everyone — which is exactly right.
    using ConsumerId = uint32_t;

    // Register (or find) a consumer by name. Idempotent: the same name always
    // gets the same id, so a viewer that reopens resumes its own cursor.
    //
    // ⚠️ A NEW CONSUMER STARTS AT THE CURRENT TOKEN, not at zero. It has just
    // built its whole world from scratch; handing it the entire backlog would
    // make it re-extract everything it already has, once, for nothing.
    ConsumerId RegisterConsumer (const char* name);

    // ⚠️ UNREGISTER WHEN THE CONSUMER STOPS, and this is not optional. A
    // registered consumer that never drains pins the dirty set: nothing can be
    // erased past its cursor, the set climbs to MaxDirty, and every OTHER
    // consumer starts getting `overflowed` — a viewer left closed would slowly
    // break the Python watcher. Closing the viewer unregisters it.
    void UnregisterConsumer (ConsumerId id);

    // The id EvP.TakeChanges drains under. Named so the Python side and the
    // viewer are visibly two consumers rather than one accidental one.
    static const char* const BusConsumerName;

    // ---- reader side: ANY THREAD, NO ACAPI ----------------------------------
    uint64_t Token ()        const { return token.load (); }
    bool     IsWatching ()   const { return watching.load (); }
    uint32_t WatchedCount () const { return watchedCount.load (); }

    // Milliseconds since the last recorded event, or -1 if nothing has ever been
    // recorded. This is what lets a consumer wait for the model to SETTLE — a
    // drag fires continuously, and re-extracting geometry on every notification
    // would fight the user's own edit.
    int64_t IdleMs () const;

    // THE QUEUE, for one consumer. Takes up to `max` of the elements THAT
    // CONSUMER has not seen and advances its cursor past them, so the next drain
    // returns different work — while every other consumer still sees them.
    // `remaining` says how many are still queued for this consumer (drain again,
    // or refresh wholesale).
    //
    // ⚠️ RETURNED IN TOKEN ORDER, OLDEST FIRST, and that is what makes `max`
    // safe. The cursor is a single number, so it can only advance past a
    // CONTIGUOUS run of tokens; handing back an arbitrary subset (which an
    // unordered map's iteration order is) and then advancing to the highest of
    // them would silently skip everything in between.
    //
    // `peek` leaves the cursor where it is — for a consumer that wants to look
    // before committing, since an entry it has passed will not come back.
    // `overflowed` means the set hit MaxDirty and elements were dropped for THIS
    // consumer: refresh everything, the list is not the whole story.
    void TakeDirtyFor (ConsumerId id, size_t max, bool peek, std::vector<Entry>& out,
                       size_t& remaining, bool& overflowed);

    // The bus consumer's drain — what EvP.TakeChanges has always called. Kept as
    // its own name so the Python side needs no change and the two consumers stay
    // visibly separate.
    void TakeDirty (size_t max, bool peek, std::vector<Entry>& out,
                    size_t& remaining, bool& overflowed);

    // How many elements are waiting for a consumer. Cheap; for deciding whether
    // it is worth waking up.
    size_t DirtyCountFor (ConsumerId id) const;
    size_t DirtyCount () const;   // the bus consumer's

    // ---- watch bookkeeping: MAIN THREAD -------------------------------------
    void SetWatching (bool on, uint32_t attached);

private:
    ChangeTracker () = default;

    mutable std::mutex    mtx;
    // guid -> the last thing that happened to it. A map rather than a log, so a
    // thousand drag frames on one wall stay ONE entry. See the ⚠️ above.
    // GS::Guid (not API_Guid — that one is a POD with no hash) keyed by its own
    // GenerateHashValue. API_Guid exists only because a union cannot hold a class
    // with a constructor; the two are the same bits, so APIGuid2GSGuid is free.
    struct GuidHash {
        std::size_t operator() (const GS::Guid& g) const { return (std::size_t) g.GenerateHashValue (); }
    };
    std::unordered_map<GS::Guid, Entry, GuidHash> dirty;

    // One cursor per consumer. `overflowed` is PER CONSUMER because the loss it
    // reports is per consumer: an entry evicted before consumer A read it is a
    // hole for A and nothing at all for B, who had already passed it.
    struct Consumer {
        std::string name;
        uint64_t    cursor     = 0;
        bool        overflowed = false;
    };
    std::vector<Consumer> consumers;

    // Erase everything every registered consumer has passed. Called at the end
    // of a drain, under the lock. ⚠️ WITH NO CONSUMERS REGISTERED THIS ERASES
    // NOTHING — deliberately: a set with no readers is not garbage, it is a
    // backlog for whoever registers next... except that a new consumer starts at
    // the current token, so it would never be read. The MaxDirty cap is what
    // bounds that case, and it is the honest bound: nothing here can know that
    // no consumer will ever register.
    void CollectPassedEntries ();

    std::atomic<uint64_t> token        { 0 };
    std::atomic<bool>     watching     { false };
    std::atomic<uint32_t> watchedCount { 0 };
    std::atomic<int64_t>  lastEventMs  { -1 };   // steady_clock ms, -1 = never
};

// ---------------------------------------------------------------------------
// Installing the observer — MAIN THREAD ONLY, all three.
//
// Two registrations are needed and they cover different halves, which is the
// one thing about this API that is easy to get wrong:
//
//   * ACAPI_Element_CatchNewElement (nullptr, proc) fires for elements that are
//     CREATED, project-wide, with no per-element setup;
//   * ACAPI_Element_InstallElementObserver (proc) fires for change/delete/undo
//     ONLY on elements that have had ACAPI_Element_AttachObserver called on them.
//
// So catching "the model changed" needs the handlers installed AND an observer
// attached to every element that already exists (AttachObserversToAll). Newly
// created ones get theirs attached by the callback itself, as the DevKit's own
// Notification_Manager example does.
// ---------------------------------------------------------------------------
GSErrCode InstallChangeObserver ();
GSErrCode UninstallChangeObserver ();

// What the arming pass actually did.
//
// ⚠️ IT REPORTS FAILURES AND TIMINGS BECAUSE THE FIRST LIVE RUN NEEDED THEM AND
// DID NOT HAVE THEM: it said "attached: 20" in 5.2 SECONDS and there was no way
// to tell whether the project had 20 elements or 3000 of which 2980 were
// refused, nor whether the 5 seconds went into listing or attaching. A count
// with no denominator answers nothing.
struct AttachReport {
    uint32_t      listed    = 0;    // what GetElemList returned
    uint32_t      attached  = 0;    // observers now on (including already-on)
    uint32_t      failed    = 0;    // refused by ACAPI, itemised by firstError
    uint32_t      observed  = 0;    // ARCHICAD'S OWN count — see below
    bool          truncated = false;// hit the budget/cap before the end of the list
    bool          cancelled = false;// the user pressed Cancel in the progress window
    GS::UniString firstError;       // the first refusal, decoded
    int64_t       listMs    = 0;
    int64_t       attachMs  = 0;
};

// How much of the MAIN THREAD the arming pass is allowed to take.
//
// ⚠️ THIS EXISTS BECAUSE THE UNBOUNDED VERSION HUNG ARCHICAD. Measured on a
// 20-element project: the listing alone took 3545 ms and each attach ~3.9 ms.
// Both scale with the project, and all of it runs on the main thread inside one
// gate call — so on a real model "arm the watch" became a frozen application
// with no progress and no way out. A watcher is a convenience; it may never cost
// the user their session. The pass now stops at whichever limit comes first and
// says so (`truncated`), which is a far better failure than a hang.
struct AttachLimits {
    uint32_t maxElements = 2000;    // never attach to more than this
    int64_t  budgetMs    = 2000;    // never spend longer than this attaching
};

// Attaches an observer to each element the filter selects, up to `limits`.
// `filterFlags` is the APIFilt_* mask handed to ACAPI_Element_GetElemList —
// APIFilt_In3D for "the model", APIFilt_None for everything in the current
// database (which is BIGGER and slower, not cheaper: it adds every 2D element).
//
// `shouldStop` is polled between elements so Archicad's own Cancel button works;
// pass nullptr for no cancellation.
//
// `observed` comes from ACAPI_Notification_GetObservedElements, i.e. from
// Archicad rather than from our own tally. That distinction is the point: our
// count says what we THINK we attached, and only Archicad's says what is
// actually being watched. When they disagree, believe Archicad.
GSErrCode AttachObserversToAll (GSFlags filterFlags, const AttachLimits& limits,
                                const std::function<bool ()>& shouldStop,
                                AttachReport& report, GS::UniString& err);

// Attaches to exactly these elements — the BOUNDED path, and the one a real
// consumer should use. A viewer knows which elements it drew; watching those
// costs what they cost and nothing more, with no listing pass at all.
GSErrCode AttachObserversTo (const GS::Array<API_Guid>& guids,
                             AttachReport& report, GS::UniString& err);

// Is each of these elements actually observed, per Archicad? The diagnostic that
// separates "changes are not reported" from "this element was never watched" —
// two failures that look identical from Python and have opposite fixes.
GSErrCode ObservedElements (GS::Array<API_Guid>& guids);

} // namespace geomsrv

#endif
