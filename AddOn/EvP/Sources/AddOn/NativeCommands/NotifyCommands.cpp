#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NotifyCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "Notify/ChangeTracker.hpp"
#include "Notify/BackgroundArm.hpp"
#include "Notify/ModelDiff.hpp"

#include "APIdefs_ElementDifferenceGenerator.h"
// scope="visible" asks the MODELER what the current 3D view actually shows —
// see VisibleElementGuids below for why that is the right list to watch.
#include "NativeCommands/ModelAccessUtils.hpp"
#include "Geometry/GeometryExtractor.hpp"     // AcquireCurrentModel
#include <Model.hpp>
#include <ModelElement.hpp>

#include <chrono>
#include <vector>

namespace geomsrv {

namespace {

// ===========================================================================
// E25 — the model change token.
//
// TWO COMMANDS, ON OPPOSITE SIDES OF THE GATE, and that is the whole design:
//
//   EvP.WatchModel      arms it. Main-thread ACAPI, called ONCE (or after a
//                       project change). Attaching an observer to a few
//                       thousand elements is not something to do in a poll.
//   EvP.GetChangeToken  reads it. Touches NO ACAPI — NeedsMainThread() is
//                       false, so the dispatcher runs it inline on the caller's
//                       thread with no ~3ms gate hop. That is what makes it
//                       honest to call at 300 ms inside an await loop, which is
//                       the only way a viewer can refresh itself.
//
// Neither is IsWrite(): observing a model changes nothing in it, and an undo
// step for arming a watch would be a bug the user has to press Ctrl+Z through.
// ===========================================================================

// The raw API_ElementDBEventID as a name a script can branch on. Spelled out
// rather than passed through as a number because a consumer's most common
// question — "was this a delete?" — should not require it to carry a copy of the
// DevKit's enum. An unknown id reports its number, so a future Archicad adding
// an event kind degrades to something diagnosable instead of a silent "".
GS::UniString EventName (Int32 eventId)
{
    switch (eventId) {
        case APINotifyElement_New:                  return "new";
        case APINotifyElement_Copy:                 return "copy";
        case APINotifyElement_Change:               return "change";
        case APINotifyElement_Edit:                 return "edit";
        case APINotifyElement_Delete:               return "delete";
        case APINotifyElement_Undo_Created:         return "undoCreated";
        case APINotifyElement_Undo_Modified:        return "undoModified";
        case APINotifyElement_Undo_Deleted:         return "undoDeleted";
        case APINotifyElement_Redo_Created:         return "redoCreated";
        case APINotifyElement_Redo_Modified:        return "redoModified";
        case APINotifyElement_Redo_Deleted:         return "redoDeleted";
        case APINotifyElement_PropertyValueChange:  return "propertyValue";
        case APINotifyElement_ClassificationChange: return "classification";
        default: break;
    }
    // "something changed, identity unknown" — a project event or a write EvP
    // made itself. The guid is null; the only correct reaction is a full
    // refresh. See ChangeTracker::RecordSelfWrite for why this exists.
    if (eventId == ChangeTracker::OpaqueEventId)
        return "opaque";
    return GS::UniString::Printf ("event%d", (int) eventId);
}

GS::ObjectState ElementRecord (const GS::UniString& guid)
{
    GS::ObjectState elementId, element;
    elementId.Add ("guid", guid);
    element.Add ("elementId", elementId);
    return element;
}

bool GetElementGuid (const GS::ObjectState& element, GS::UniString& guid)
{
    GS::ObjectState elementId;
    return element.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ();
}

// The elements the CURRENT 3D VIEW actually shows.
//
// ⚠️ THIS IS THE RIGHT LIST TO WATCH, and it is much shorter than the project.
// A watcher exists to keep a viewer in step with what the user can SEE, so
// hidden layers, other storeys and filtered-out elements are not merely
// affordable to skip — tracking them is work that can never change a pixel. The
// measured project had 12,238 elements in the database; the 3D sight is built
// from the current view's layer/storey/filter settings, so this is whatever
// subset of those is actually on screen.
//
// The modeler is the authority here, not a filter flag: ACAPI_Element_GetElemList
// with APIFilt_In3D answers "could have 3D geometry", which is a different (and
// larger) question than "is in the current sight".
//
// MAIN THREAD ONLY.
static bool VisibleElementGuids (GS::Array<API_Guid>& guids, GS::UniString& err)
{
    ModelerAPI::Model model;
    if (!AcquireCurrentModel (model)) {
        err = EVP_FAIL ("could not read the current 3D model",
                        "watching what is visible needs a generated 3D view - "
                        "open the 3D window once, then try again");
        return false;
    }

    const Int32 total = model.GetElementCount ();
    for (Int32 i = 1; i <= total; ++i) {
        ModelerAPI::Element elem;
        model.GetElement (i, &elem);
        const GS::UniString guid = ElementGuidString (elem);
        if (!guid.IsEmpty ())
            guids.Push (APIGuidFromString (guid.ToCStr ().Get ()));
    }
    return true;
}

// ---------------------------------------------------------------------------
// EvP.WatchModel { enable?, guids?, attachAll?, background?, sliceMs?, gapMs?, ... }
//
// ⚠️ READ THIS BEFORE CHANGING THE DEFAULT. Measured on a real 12,238-element
// project (2026-08-03): attaching an observer to every element managed 174
// attaches in 19.5 s — ~9/second, because each gate round trip spent ~190 ms
// waiting to be dispatched to carry ~6 ms of work. A full pass would have taken
// ~22 MINUTES of poking Archicad twice a second, and it kept running after the
// command ended. Slicing cannot fix that: the total is ~12k x ~3 ms of
// main-thread work however it is chopped up. Per-element observers do not scale,
// full stop.
//
// So the modes, cheapest first:
//
//   0. `scope: "visible"` — attach to what the CURRENT 3D VIEW shows, and
//      nothing else. Hidden layers, other storeys and filtered-out elements
//      cannot change a pixel, so watching them is pure cost. THE RIGHT DEFAULT
//      FOR A VIEWER.
//   1. DEFAULT (no `guids`, no `attachAll`): install the handlers, attach to
//      NOTHING. ACAPI_Element_CatchNewElement is global and free, so element
//      CREATION is reported at zero cost. Changes/deletes are not — use one of:
//   2. `guids` — attach to exactly the elements you display. Bounded by a list
//      you chose. This is the right call for a viewer.
//   3. EvP.GetModelDiff — Archicad's own difference generator answers "what
//      changed" in ONE call with no observers at all. The scalable answer, and
//      the one to reach for on a big project.
//   4. `attachAll: true` — the whole-model pass, opt-in by name. Background by
//      default (worker thread, ~6 ms slices, 25 ms gaps, HARD 60 s wall), or
//      `background: false` for the synchronous capped version.
//
// ⚠️ THIS COMMAND FROZE ARCHICAD ONCE, in an uncapped synchronous whole-model
// pass, and hammered it for minutes in a second attempt. Both are why mode 4 is
// no longer reachable by accident.
//
// ⚠️ RE-ARMING IS EXPECTED, NOT AN ERROR. A project open/close drops every
// observer (ChangeTracker::OnProjectEvent). Attaching twice is harmless: ACAPI
// answers APIERR_LINKEXIST and we count it as attached.
// ---------------------------------------------------------------------------
class WatchModelCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "WatchModel"; }

    // The listing and the attach loop are both long on a real model. Archicad's
    // own progress window is the difference between "working" and "hung", and it
    // is what carries the Cancel button.
    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& pc) const override
    {
        GS::ObjectState os;

        bool enable = true;
        params.Get ("enable", enable);

        if (!enable) {
            // ⚠️ STOP THE BACKGROUND PASS FIRST. Round 5: the arming worker
            // outlived the command that started it and kept hitting the gate
            // roughly twice a second with no way to call it off — "it doesn't
            // stop even after the command stops". Turning the watch off has to
            // mean turning ALL of it off.
            ArmWorker::Get ().Stop ();
            const GSErrCode err = UninstallChangeObserver ();
            if (err != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                    "ACAPI_Element_InstallElementObserver", err,
                    "removing the model change observer"));
            }
            os.Add ("watching", false);
            os.Add ("attached", (GS::Int32) 0);
            os.Add ("token", (GS::Int64) ChangeTracker::Get ().Token ());
            return os;
        }

        GS::UniString scope ("3d");
        params.Get ("scope", scope);
        if (scope != "3d" && scope != "all" && scope != "visible") {
            return NativeCommandResult::Failure (EVP_FAIL (
                GS::UniString ("scope must be \"visible\", \"3d\" or \"all\", got \"") +
                scope + "\"",
                "arming the model change observer"));
        }

        GS::Array<GS::ObjectState> wantedElements;
        const bool haveGuids = params.Get ("elements", wantedElements) && !wantedElements.IsEmpty ();
        for (const GS::ObjectState& element : wantedElements) {
            GS::UniString guid;
            if (!GetElementGuid (element, guid)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every element needs elementId.guid", "Tapioca.WatchModel"));
            }
        }

        const auto started = std::chrono::steady_clock::now ();

        const GSErrCode installErr = InstallChangeObserver ();
        if (installErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_Element_InstallElementObserver", installErr,
                "installing the model change observer"));
        }

        // ---- THE DEFAULT: install the handlers, attach to NOTHING -----------
        //
        // ⚠️ MEASURED ON A REAL PROJECT, 2026-08-03: 12,238 elements. The
        // background pass managed 174 attaches in 19.5 s — about 9 per second,
        // because each gate round trip cost ~190 ms of DISPATCH LATENCY to carry
        // ~6 ms of work. Finishing would have taken ~22 minutes of Archicad
        // being poked twice a second, and it kept going after the command ended
        // because nothing stopped it. Slicing cannot rescue that: the total is
        // ~12k * ~3 ms of main-thread work no matter how it is chopped up.
        //
        // So attaching to the whole model is no longer something that happens by
        // default, or by accident. It must be asked for by name (`attachAll`),
        // and the honest paths are:
        //   * `guids` — attach to what you actually display; or
        //   * EvP.GetModelDiff — Archicad's own difference generator, which
        //     answers "what changed" with NO observers at all and one call.
        //
        // Creation events still work with zero attaches: ACAPI_Element_CatchNewElement
        // is global and free. That is what this default leaves you with.
        bool attachAll = false;
        params.Get ("attachAll", attachAll);

        // ---- scope="visible": watch what the 3D view SHOWS ------------------
        // The requirement, once the whole-project pass was abandoned: "only the
        // initial selection of elements, or what's visible in the current 3D
        // view — no need to track hidden elements." This is that list, and it is
        // bounded by the view rather than by the database.
        if (!haveGuids && !attachAll && scope == "visible") {
            GS::Array<API_Guid> visible;
            GS::UniString       visErr;
            if (!VisibleElementGuids (visible, visErr)) {
                return NativeCommandResult::Failure (visErr);
            }

            AttachReport  report;
            GS::UniString attachErrText;
            AttachObserversTo (visible, report, attachErrText);
            ChangeTracker::Get ().SetWatching (true, report.attached);

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
                std::chrono::steady_clock::now () - started).count ();

            os.Add ("watching", true);
            os.Add ("mode", "visible");
            os.Add ("listed",   (GS::Int32) report.listed);     // what the sight showed
            os.Add ("attached", (GS::Int32) report.attached);
            os.Add ("failed",   (GS::Int32) report.failed);
            os.Add ("observed", (GS::Int32) report.observed);
            if (!report.firstError.IsEmpty ())
                os.Add ("firstError", report.firstError);
            os.Add ("token", (GS::Int64) ChangeTracker::Get ().Token ());
            os.Add ("attachMs",  (GS::Int64) report.attachMs);
            os.Add ("elapsedMs", (GS::Int64) elapsed);
            return os;
        }

        if (!haveGuids && !attachAll) {
            ChangeTracker::Get ().SetWatching (true, 0);
            os.Add ("watching", true);
            os.Add ("mode", "createOnly");
            os.Add ("attached", (GS::Int32) 0);
            os.Add ("token", (GS::Int64) ChangeTracker::Get ().Token ());
            os.Add ("note", "handlers installed; NEW elements are reported. For changes and "
                            "deletes pass guids=[...] (cheap) or poll EvP.GetModelDiff "
                            "(no observers). attachAll=true walks every element and is slow "
                            "on a real project — 12k elements measured ~22 minutes.");
            return os;
        }

        bool background = true;
        params.Get ("background", background);

        if (background && !haveGuids) {
            GS::Int64 sliceMs = 6, gapMs = 25;
            params.Get ("sliceMs", sliceMs);
            params.Get ("gapMs", gapMs);
            ArmWorker::Get ().Start (sliceMs > 0 ? sliceMs : 6, gapMs >= 0 ? gapMs : 25);

            os.Add ("watching", true);      // creation events are live already
            os.Add ("mode", "background");
            os.Add ("arming", true);
            os.Add ("token", (GS::Int64) ChangeTracker::Get ().Token ());
            os.Add ("note", "arming in the background; poll EvP.GetChangeToken for armProgress");
            return os;
        }

        AttachLimits limits;
        GS::Int32 maxElements = (GS::Int32) limits.maxElements;
        if (params.Get ("maxElements", maxElements) && maxElements > 0)
            limits.maxElements = (uint32_t) maxElements;
        GS::Int64 budgetMs = limits.budgetMs;
        if (params.Get ("budgetMs", budgetMs) && budgetMs > 0)
            limits.budgetMs = budgetMs;

        AttachReport  report;
        GS::UniString err;
        GSErrCode     attachErr = NoError;

        // THE BOUNDED PATH FIRST. When the caller names the elements there is no
        // listing pass at all — which removes the 3.5-second half of the cost
        // outright — and no cap is needed, because the caller already chose the
        // size of the job.
        if (haveGuids) {
            GS::Array<API_Guid> guids;
            for (const GS::ObjectState& element : wantedElements) {
                GS::UniString guid;
                if (GetElementGuid (element, guid))
                    guids.Push (APIGuidFromString (guid.ToCStr ().Get ()));
            }
            pc.SetProcessName ("EvP: watching " +
                               GS::UniString::Printf ("%u element(s)", (unsigned) guids.GetSize ()));
            attachErr = AttachObserversTo (guids, report, err);
        } else {
            pc.SetProcessName ("EvP: arming the model change observer");
            attachErr = AttachObserversToAll (
                (scope == "all") ? APIFilt_None : APIFilt_In3D, limits,
                [&pc] { return pc.TestBreak (); },      // Cancel button
                report, err);
        }
        if (attachErr != NoError) {
            // The handlers ARE installed at this point, so leave them: creation
            // events still arrive and are better than nothing. But do not claim
            // to be watching — that would tell the consumer a change to an
            // existing element will be reported, and it will not be.
            ChangeTracker::Get ().SetWatching (false, 0);
            return NativeCommandResult::Failure (err);
        }

        ChangeTracker::Get ().SetWatching (true, report.attached);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - started).count ();

        os.Add ("watching", true);
        os.Add ("scope", scope);
        os.Add ("listed",   (GS::Int32) report.listed);
        os.Add ("attached", (GS::Int32) report.attached);
        os.Add ("failed",   (GS::Int32) report.failed);
        os.Add ("observed", (GS::Int32) report.observed);   // Archicad's own count
        // Both mean "changes to the elements we did NOT reach are invisible", so
        // a caller that ignores them will silently miss edits. Always present,
        // never inferred from a count comparison.
        os.Add ("truncated", report.truncated);
        os.Add ("cancelled", report.cancelled);
        if (!report.firstError.IsEmpty ())
            os.Add ("firstError", report.firstError);
        os.Add ("token", (GS::Int64) ChangeTracker::Get ().Token ());
        // Split, because "5 seconds" with one number in it says nothing about
        // whether listing or attaching is the expensive half.
        os.Add ("listMs",    (GS::Int64) report.listMs);
        os.Add ("attachMs",  (GS::Int64) report.attachMs);
        os.Add ("elapsedMs", (GS::Int64) elapsed);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.GetChangeToken {} -> { ok, token, watching, watchedCount, idleMs,
//                            dirtyCount, arming, armProgress }
//
// `token` is a monotonic counter of element notifications this session. Poll it;
// when it differs from the one you last saw, the model changed. `dirtyCount` is
// how many DISTINCT elements are waiting — the queue depth. Claim them with
// EvP.TakeChanges.
//
// `idleMs` is milliseconds since the last notification, or -1 if none ever. A
// drag fires continuously, so a consumer that re-extracts geometry should wait
// for the model to SETTLE — token changed AND idleMs > ~300 — rather than
// fighting the edit in progress.
//
// GATE-FREE: NeedsMainThread() is false. Everything below is atomics and one
// mutex over the dirty set; there is no ACAPI call in it, and there must never be.
//
// This is the CHEAP heartbeat — "is there anything to do". The work itself is
// claimed with EvP.TakeChanges, which is the queue drain.
// ---------------------------------------------------------------------------
class GetChangeTokenCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetChangeToken"; }

    bool NeedsMainThread () const override { return false; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        const ChangeTracker& tracker = ChangeTracker::Get ();

        os.Add ("token",        (GS::Int64) tracker.Token ());
        os.Add ("watching",     tracker.IsWatching ());
        os.Add ("watchedCount", (GS::Int32) tracker.WatchedCount ());
        os.Add ("idleMs",       (GS::Int64) tracker.IdleMs ());

        // Background arming progress rides along on the poll a consumer already
        // makes, so "is the watch ready yet" costs no extra call and — like
        // everything else here — no gate hop. `longestSliceMs` is the number
        // that matters: it is the worst the main thread was ever held, i.e. the
        // measurement that says whether Archicad could have stuttered.
        const ArmWorker::Progress arm = ArmWorker::Get ().Snapshot ();
        os.Add ("arming", arm.running);
        GS::ObjectState armState;
        armState.Add ("running",   arm.running);
        armState.Add ("done",      arm.done);
        armState.Add ("listed",    (GS::Int32) arm.listed);
        armState.Add ("attached",  (GS::Int32) arm.attached);
        armState.Add ("failed",    (GS::Int32) arm.failed);
        armState.Add ("slices",    (GS::Int32) arm.slices);
        armState.Add ("listMs",    (GS::Int64) arm.listMs);
        armState.Add ("longestHoldMs",      (GS::Int64) arm.longestHoldMs);
        armState.Add ("longestRoundTripMs", (GS::Int64) arm.longestRoundTripMs);
        armState.Add ("gaveUp",             arm.gaveUp);
        armState.Add ("elapsedMs", (GS::Int64) arm.elapsedMs);
        armState.Add ("phase",     arm.phase);
        os.Add ("armProgress", armState);

        os.Add ("dirtyCount", (GS::Int32) tracker.DirtyCount ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.TakeChanges { max?:int=500, peek?:bool=false }
//   -> { ok, guids, events, count, remaining, overflowed, token, idleMs }
//
// THE QUEUE DRAIN — the middle of the pipeline the consumer asked for:
//
//     element changed -> mark it dirty -> [TakeChanges] -> update the WebUI
//
// Returns up to `max` DISTINCT dirty elements and REMOVES them, so the caller
// owns that batch and the next drain returns different work. Elements touched a
// thousand times during a drag appear ONCE, with the last event that happened to
// them — coalescing is free because the store is a set, not a log.
//
// ⚠️ `peek: true` leaves the queue intact. Use it when the update might fail: a
// drained entry is gone, and re-marking it is not possible from outside. Peek,
// update, then drain.
//
// ⚠️ `overflowed` means more than MaxDirty distinct elements changed and some
// were dropped. The list is real but incomplete — refresh wholesale.
// `remaining` > 0 just means "drain again", which is normal and not a problem.
//
// GATE-FREE, like the token read: this only moves entries out of a std::map.
// Doing the actual geometry work is the caller's business, on its own thread.
// ---------------------------------------------------------------------------
class TakeChangesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "TakeChanges"; }

    bool NeedsMainThread () const override { return false; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 max = 500;
        params.Get ("max", max);
        if (max < 0)
            max = 0;

        bool peek = false;
        params.Get ("peek", peek);

        std::vector<ChangeTracker::Entry> taken;
        size_t remaining   = 0;
        bool   overflowed  = false;
        ChangeTracker::Get ().TakeDirty ((size_t) max, peek, taken, remaining, overflowed);

        GS::Array<GS::ObjectState> changes;
        for (const ChangeTracker::Entry& e : taken) {
            GS::ObjectState change = ElementRecord (GS::UniString (APIGuidToString (e.guid).ToCStr ().Get ()));
            change.Add ("event", EventName (e.eventId));
            changes.Push (change);
        }

        GS::ObjectState os;
        os.Add ("changes", changes);
        os.Add ("count",      (GS::Int32) changes.GetSize ());
        os.Add ("remaining",  (GS::Int32) remaining);
        os.Add ("overflowed", overflowed);
        os.Add ("peeked",     peek);
        os.Add ("token",  (GS::Int64) ChangeTracker::Get ().Token ());
        os.Add ("idleMs", (GS::Int64) ChangeTracker::Get ().IdleMs ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.GetObservedElements { guids?:[...] } -> { count, observed?:[bool], guids? }
//
// WHY THIS EXISTS. The first live run of ChangeTokenProbe found that a write
// made THROUGH THE BUS did not move the token, and there was no way to tell
// which of two opposite things had happened:
//
//   * the element was never watched (a coverage gap — fix the arming), or
//   * the element WAS watched and Archicad still sent nothing (fix the
//     expectation: an add-on may not be notified of its own changes).
//
// From Python those two are indistinguishable, and they have opposite fixes.
// ACAPI_Notification_GetObservedElements answers it directly, from Archicad's
// side rather than from our own tally.
//
// With `guids`, returns a parallel bool array — the per-element question. Without
// them, just the total, which is the cheap health check.
// ---------------------------------------------------------------------------
class GetObservedElementsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetObservedElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> asked;
        params.Get ("elements", asked);
        for (const GS::ObjectState& element : asked) {
            GS::UniString guid;
            if (!GetElementGuid (element, guid)) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    "every element needs elementId.guid", "Tapioca.GetObservedElements"));
            }
        }

        GS::Array<API_Guid> observed;
        const GSErrCode err = ObservedElements (observed);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_Notification_GetObservedElements", err,
                "asking Archicad which elements this add-on observes"));
        }

        os.Add ("count", (GS::Int32) observed.GetSize ());

        if (!asked.IsEmpty ()) {
            GS::Array<GS::ObjectState> observations;
            for (const GS::ObjectState& element : asked) {
                GS::UniString g;
                GetElementGuid (element, g);
                const API_Guid guid = APIGuidFromString (g.ToCStr ().Get ());
                bool found = false;
                for (const API_Guid& o : observed) {
                    if (o == guid) { found = true; break; }
                }
                GS::ObjectState observation = ElementRecord (g);
                observation.Add ("observed", found);
                observations.Push (observation);
            }
            os.Add ("observations", observations);
        }
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.GetModelDiff { reset?:bool, scope?:"3d"|"file" }
//   -> { ok, baseline, new:[guid], modified:[guid], deleted:[guid],
//        environmentChanged, counts…, elapsedMs }
//
// WHAT CHANGED SINCE LAST TIME — WITH NO OBSERVERS AT ALL.
//
// This is the answer to the problem that sank the attach-everything approach.
// Watching a 12,238-element project by attaching an observer to each element
// measured ~22 minutes of sliced main-thread work; this asks Archicad the same
// question in ONE call, because Archicad already tracks it
// (ACAPI_DifferenceGenerator_*, AC26+, real symbols in ACAPinc.h).
//
// The first call establishes a baseline and reports nothing changed. Each later
// call diffs the current project against that baseline, returns the difference,
// and adopts the current state as the new baseline — so a poll loop naturally
// reports "what changed since you last asked".
//
// `scope`: "3d" (APIDiff_3DModelBased — what a viewer cares about: geometry as
// the modeler sees it) or "file" (APIDiff_ModificationStampBased — every element
// edit, including ones with no 3D consequence).
//
// ⚠️ IT IS A POLL, NOT A NOTIFICATION, and that is the right shape here: the
// requirement explicitly allows latency ("slower pace and bigger latency is
// fine") and forbids interference. Poll it from a background loop at whatever
// cadence the measured cost justifies — `elapsedMs` is returned so a caller can
// back off on its own.
//
// ⚠️ COST IS UNMEASURED until the probe runs it. It is ONE main-thread call, but
// on a big project it may not be cheap. Do NOT wire it into a 300 ms loop before
// looking at `elapsedMs`.
// ---------------------------------------------------------------------------
class GetModelDiffCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelDiff"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString scope ("3d");
        params.Get ("scope", scope);
        const modeldiff::Scope which =
            (scope == "file") ? modeldiff::Scope::File : modeldiff::Scope::Model;

        bool reset = false;
        params.Get ("reset", reset);

        // ⚠️ THE BUS GETS ITS OWN BASELINE, SEPARATE FROM THE VIEWER'S. Polling
        // adopts the current state, so a shared baseline would mean whichever
        // consumer asked first swallowed the change — the same fault
        // ChangeTracker's per-consumer cursors exist to prevent. The viewer's
        // watch timer holds a different Baseline of its own.
        static modeldiff::Baseline busBaseline {modeldiff::Scope::Model};
        busBaseline.SetScope (which);   // a scope change drops the baseline for us

        const modeldiff::Result diff = busBaseline.Poll (reset);
        if (!diff.ok)
            return NativeCommandResult::Failure (GS::UniString (diff.error.c_str (), CC_UTF8));

        GS::Array<GS::ObjectState> created, modified, deleted;
        for (const std::string& g : diff.created)
            created.Push (ElementRecord (GS::UniString (g.c_str (), CC_UTF8)));
        for (const std::string& g : diff.modified)
            modified.Push (ElementRecord (GS::UniString (g.c_str (), CC_UTF8)));
        for (const std::string& g : diff.deleted)
            deleted.Push (ElementRecord (GS::UniString (g.c_str (), CC_UTF8)));

        // Anything real bumps the token too, so a consumer already watching it
        // does not need a second mechanism to notice.
        if (diff.AnythingChanged ())
            ChangeTracker::Get ().RecordSelfWrite ();

        os.Add ("scope", scope);
        // `baseline` true = "this call only established a baseline". The empty
        // lists below mean 'nothing to compare against', NOT 'nothing changed' —
        // a distinction a caller must not have to guess at.
        os.Add ("baseline", diff.firstCall);
        os.Add ("new", created);
        os.Add ("modified", modified);
        os.Add ("deleted", deleted);
        os.Add ("newCount",      (GS::Int32) created.GetSize ());
        os.Add ("modifiedCount", (GS::Int32) modified.GetSize ());
        os.Add ("deletedCount",  (GS::Int32) deleted.GetSize ());
        os.Add ("environmentChanged", diff.environmentChanged);
        os.Add ("elapsedMs", (GS::Int64) diff.elapsedMs);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.SyncModel {} -> { ok, token }
//
// THE MANUAL FALLBACK the requirement asked for: "if that's not possible then
// there should be a button to synchronize model with Archicad". It bumps the
// token exactly as a real change would, so a viewer's Sync button goes through
// the SAME refresh path as an observed edit. One path, not two that drift.
//
// It is also the honest escape hatch for what the observer cannot cover: an
// element the background arming pass has not reached yet, and any change
// Archicad reports to nobody.
//
// GATE-FREE — it only bumps a counter, so the button can never be the thing that
// makes Archicad wait.
// ---------------------------------------------------------------------------
class SyncModelCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "SyncModel"; }

    bool NeedsMainThread () const override { return false; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        ChangeTracker::Get ().RecordSelfWrite ();       // one opaque "refresh everything"

        GS::ObjectState os;
        os.Add ("token", (GS::Int64) ChangeTracker::Get ().Token ());
        return os;
    }
};

const NativeCommandRegistration kNotifyCommandRegistrations[] = {
    { "WatchModel", &MakeRegisteredNativeCommand<WatchModelCommand>, true,
      R"json({"type":"object","properties":{"enable":{"type":"boolean"},"scope":{"type":"string","enum":["visible","3d","all"]},"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"attachAll":{"type":"boolean"},"background":{"type":"boolean"},"sliceMs":{"type":"integer"},"gapMs":{"type":"integer"},"maxElements":{"type":"integer"},"budgetMs":{"type":"integer"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"watching":{"type":"boolean"},"mode":{"type":"string","enum":["visible","createOnly","background"]},"scope":{"type":"string","enum":["visible","3d","all"]},"arming":{"type":"boolean"},"listed":{"type":"integer","minimum":0},"attached":{"type":"integer","minimum":0},"failed":{"type":"integer","minimum":0},"observed":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"cancelled":{"type":"boolean"},"firstError":{"type":"string"},"token":{"type":"integer","minimum":0},"listMs":{"type":"integer","minimum":0},"attachMs":{"type":"integer","minimum":0},"elapsedMs":{"type":"integer","minimum":0},"note":{"type":"string"}},"additionalProperties":false,"required":["watching","token"]})json" },
    { "GetChangeToken", &MakeRegisteredNativeCommand<GetChangeTokenCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"token":{"type":"integer","minimum":0},"watching":{"type":"boolean"},"watchedCount":{"type":"integer","minimum":0},"idleMs":{"type":"integer","minimum":-1},"arming":{"type":"boolean"},"armProgress":{"type":"object","properties":{"running":{"type":"boolean"},"done":{"type":"boolean"},"listed":{"type":"integer","minimum":0},"attached":{"type":"integer","minimum":0},"failed":{"type":"integer","minimum":0},"slices":{"type":"integer","minimum":0},"listMs":{"type":"integer","minimum":0},"longestHoldMs":{"type":"integer","minimum":0},"longestRoundTripMs":{"type":"integer","minimum":0},"gaveUp":{"type":"boolean"},"elapsedMs":{"type":"integer","minimum":0},"phase":{"type":"string"}},"additionalProperties":false,"required":["running","done","listed","attached","failed","slices","listMs","longestHoldMs","longestRoundTripMs","gaveUp","elapsedMs","phase"]},"dirtyCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["token","watching","watchedCount","idleMs","arming","armProgress","dirtyCount"]})json" },
    { "GetObservedElements", &MakeRegisteredNativeCommand<GetObservedElementsCommand>, false,
      R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0},"observations":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"observed":{"type":"boolean"}},"additionalProperties":false,"required":["elementId","observed"]}}},"additionalProperties":false,"required":["count"]})json" },
    { "SyncModel", &MakeRegisteredNativeCommand<SyncModelCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"token":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["token"]})json" },
    { "GetModelDiff", &MakeRegisteredNativeCommand<GetModelDiffCommand>, false,
      R"json({"type":"object","properties":{"reset":{"type":"boolean"},"scope":{"type":"string","enum":["3d","file"]}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"scope":{"type":"string","enum":["3d","file"]},"baseline":{"type":"boolean"},"new":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"modified":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"deleted":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}},"newCount":{"type":"integer","minimum":0},"modifiedCount":{"type":"integer","minimum":0},"deletedCount":{"type":"integer","minimum":0},"environmentChanged":{"type":"boolean"},"elapsedMs":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["scope","baseline","new","modified","deleted","newCount","modifiedCount","deletedCount","environmentChanged","elapsedMs"]})json" },
    { "TakeChanges", &MakeRegisteredNativeCommand<TakeChangesCommand>, false,
      R"json({"type":"object","properties":{"max":{"type":"integer"},"peek":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"changes":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"event":{"type":"string"}},"additionalProperties":false,"required":["elementId","event"]}},"count":{"type":"integer","minimum":0},"remaining":{"type":"integer","minimum":0},"overflowed":{"type":"boolean"},"peeked":{"type":"boolean"},"token":{"type":"integer","minimum":0},"idleMs":{"type":"integer","minimum":-1}},"additionalProperties":false,"required":["changes","count","remaining","overflowed","peeked","token","idleMs"]})json" },
};

}   // namespace

NativeCommandRegistrations GetNotifyCommandRegistrations ()
{
    return MakeRegistrationView (kNotifyCommandRegistrations);
}

} // namespace geomsrv
