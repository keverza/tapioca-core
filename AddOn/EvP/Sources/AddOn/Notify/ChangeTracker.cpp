#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Notify/ChangeTracker.hpp"
#include "Diagnostics/ApiError.hpp"     // EVP_ACAPI_FAIL — never print a bare GSErrCode

#include <algorithm>
#include <chrono>

namespace geomsrv {

namespace {

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// The observer. ARCHICAD'S THREAD, DURING EDITING — see the header's ⚠️.
//
// The only ACAPI call here is AttachObserver on a brand-new element, and it is
// here because it is the ONLY moment that element can be reached: nothing polls
// for new elements, so an element created after the watch started would
// otherwise never report its later changes. This is exactly what the DevKit's
// Notification_Manager example does in the same case.
//
// Everything else — reading the element, rebuilding a snapshot, telling Python —
// stays out. A notification arrives per element per drag frame.
GSErrCode ElementEventHandler (const API_NotifyElementType* elemType)
{
    if (elemType == nullptr)
        return NoError;

    // Database-level brackets around a batch of events. They carry no element,
    // so there is nothing to record and no guid to report.
    if (elemType->notifID == APINotifyElement_BeginEvents ||
        elemType->notifID == APINotifyElement_EndEvents)
        return NoError;

    ChangeTracker::Get ().Record (elemType->elemHead.guid, (Int32) elemType->notifID);

    switch (elemType->notifID) {
        case APINotifyElement_New:
        case APINotifyElement_Copy:
        case APINotifyElement_Undo_Created:
        case APINotifyElement_Redo_Created:
            // APIERR_LINKEXIST just means we already watch it — not a failure.
            // Any other error is swallowed on purpose: this callback runs inside
            // the user's edit and has nowhere to report to. The consequence is
            // bounded (that one element's later changes go unreported) and the
            // creation itself was already recorded above.
            ACAPI_Element_AttachObserver (elemType->elemHead.guid);
            break;
        default:
            break;
    }

    return NoError;
}

}   // namespace

// ---------------------------------------------------------------------------

ChangeTracker& ChangeTracker::Get ()
{
    static ChangeTracker instance;
    return instance;
}

void ChangeTracker::Record (const API_Guid& guid, Int32 eventId)
{
    const uint64_t issued = token.fetch_add (1) + 1;   // first event is token 1

    {
        std::lock_guard<std::mutex> lock (mtx);
        const GS::Guid key = APIGuid2GSGuid (guid);
        auto it = dirty.find (key);
        if (it != dirty.end ()) {
            // ALREADY DIRTY — the storm case, and the whole reason this is a set.
            // Overwrite rather than append: the LAST event is the one that
            // matters (a delete after a change is a delete), and a drag's
            // thousand frames stay one entry.
            it->second.eventId = eventId;
            it->second.token   = issued;
        } else if (dirty.size () < MaxDirty) {
            dirty.emplace (key, Entry { guid, eventId, issued });
        } else {
            // Refusing to grow is the only safe option inside this callback.
            // The flag is what stops that being a silent loss: the consumer is
            // told the list is incomplete and refreshes wholesale.
            //
            // ⚠️ EVERY consumer is marked, because the entry was never recorded
            // at all — there is no cursor position at which it exists, so no
            // consumer can distinguish "not mine" from "lost". Marking only the
            // furthest-behind one would let a caught-up consumer miss an edit
            // silently, which is the whole failure this flag exists to prevent.
            for (Consumer& c : consumers) {
                if (!c.name.empty ())
                    c.overflowed = true;
            }
        }
    }

    lastEventMs.store (NowMs ());
}

const char* const ChangeTracker::BusConsumerName = "bus";

ChangeTracker::ConsumerId ChangeTracker::RegisterConsumer (const char* name)
{
    std::lock_guard<std::mutex> lock (mtx);

    const std::string wanted = (name != nullptr) ? name : "";
    for (size_t i = 0; i < consumers.size (); ++i) {
        if (consumers[i].name == wanted)
            return (ConsumerId) i;
    }

    Consumer c;
    c.name = wanted;
    // Starts at the CURRENT token: a consumer registering now has just built its
    // world and must not be handed the backlog it already reflects.
    c.cursor = token.load ();
    consumers.push_back (c);
    return (ConsumerId) (consumers.size () - 1);
}

void ChangeTracker::UnregisterConsumer (ConsumerId id)
{
    std::lock_guard<std::mutex> lock (mtx);
    if (id >= consumers.size ())
        return;

    // ⚠️ THE SLOT IS EMPTIED, NOT ERASED. Ids are indices, so removing one would
    // renumber every consumer after it and silently hand one consumer another
    // one's cursor. An empty slot costs a string and two words; the alternative
    // costs a bug that only appears with three consumers.
    consumers[id] = Consumer ();
    consumers[id].cursor = UINT64_MAX;   // pins nothing: it has passed everything
    CollectPassedEntries ();
}

void ChangeTracker::CollectPassedEntries ()
{
    // Called under the lock. An entry survives while ANY registered consumer
    // still has to see it.
    uint64_t low = UINT64_MAX;
    bool     any = false;
    for (const Consumer& c : consumers) {
        if (c.name.empty ())
            continue;              // an emptied slot pins nothing
        any = true;
        low = std::min (low, c.cursor);
    }
    if (!any)
        return;                    // no readers: see the header's ⚠️

    for (auto it = dirty.begin (); it != dirty.end (); ) {
        if (it->second.token <= low)
            it = dirty.erase (it);
        else
            ++it;
    }
}

void ChangeTracker::TakeDirtyFor (ConsumerId id, size_t max, bool peek,
                                  std::vector<Entry>& out, size_t& remaining,
                                  bool& overflowedOut)
{
    out.clear ();
    remaining      = 0;
    overflowedOut  = false;

    std::lock_guard<std::mutex> lock (mtx);
    if (id >= consumers.size ())
        return;                    // not registered: nothing is dirty for nobody

    Consumer& me  = consumers[id];
    overflowedOut = me.overflowed;

    // Everything this consumer has not passed, OLDEST FIRST. The sort is what
    // makes a bounded drain safe — see the header's ⚠️: the cursor is one
    // number, so it may only advance over a contiguous run of tokens, and an
    // unordered map hands them back in whatever order it likes.
    std::vector<const Entry*> pending;
    pending.reserve (dirty.size ());
    for (const auto& kv : dirty) {
        if (kv.second.token > me.cursor)
            pending.push_back (&kv.second);
    }
    std::sort (pending.begin (), pending.end (),
               [] (const Entry* a, const Entry* b) { return a->token < b->token; });

    const size_t take = std::min (max, pending.size ());
    out.reserve (take);
    for (size_t i = 0; i < take; ++i)
        out.push_back (*pending[i]);

    remaining = pending.size () - take;

    if (!peek && take > 0) {
        me.cursor = out.back ().token;
        // The overflow flag clears with the drain it was reported on: it
        // describes THAT batch, and leaving it set makes every later batch look
        // lossy. Only once this consumer is caught up, though — clearing it
        // mid-backlog would hide the loss it is there to announce.
        if (remaining == 0)
            me.overflowed = false;
        CollectPassedEntries ();
    } else if (!peek && pending.empty ()) {
        me.overflowed = false;
    }
}

void ChangeTracker::TakeDirty (size_t max, bool peek, std::vector<Entry>& out,
                               size_t& remaining, bool& overflowedOut)
{
    // The bus consumer registers itself on first use, so EvP.TakeChanges needs
    // no setup call and no change on the Python side.
    const ConsumerId bus = RegisterConsumer (BusConsumerName);
    TakeDirtyFor (bus, max, peek, out, remaining, overflowedOut);
}

size_t ChangeTracker::DirtyCountFor (ConsumerId id) const
{
    std::lock_guard<std::mutex> lock (mtx);
    if (id >= consumers.size ())
        return 0;

    const uint64_t cursor = consumers[id].cursor;
    size_t n = 0;
    for (const auto& kv : dirty) {
        if (kv.second.token > cursor)
            ++n;
    }
    return n;
}

size_t ChangeTracker::DirtyCount () const
{
    // ⚠️ CONST, so it cannot register the bus consumer the way TakeDirty does.
    // If nothing has drained yet the bus has no cursor, and the honest answer to
    // "how much is waiting for it" is then the whole set — which is what it
    // would receive.
    std::lock_guard<std::mutex> lock (mtx);
    for (const Consumer& c : consumers) {
        if (c.name != BusConsumerName)
            continue;
        size_t n = 0;
        for (const auto& kv : dirty) {
            if (kv.second.token > c.cursor)
                ++n;
        }
        return n;
    }
    return dirty.size ();
}

void ChangeTracker::OnProjectEvent ()
{
    // A null guid says "something changed and it is not one element" — the
    // consumer's cue to refresh wholesale. The watch is dropped because the
    // elements it was attached to belong to a database that is going away.
    Record (APINULLGuid, OpaqueEventId);
    SetWatching (false, 0);
}

void ChangeTracker::RecordSelfWrite ()
{
    // Deliberately NOT per element: the dispatcher knows a write succeeded, not
    // which guids it touched (a transaction is an opaque batch, and half the
    // write commands take a selection rather than a list). One opaque entry with
    // a null guid is honest — "refresh, I cannot tell you what" — where a guessed
    // guid list would be a lie a consumer acts on.
    Record (APINULLGuid, OpaqueEventId);
}

int64_t ChangeTracker::IdleMs () const
{
    const int64_t last = lastEventMs.load ();
    if (last < 0)
        return -1;
    const int64_t idle = NowMs () - last;
    return idle < 0 ? 0 : idle;
}

void ChangeTracker::SetWatching (bool on, uint32_t attached)
{
    watching.store (on);
    watchedCount.store (on ? attached : 0);
}

// ---------------------------------------------------------------------------

GSErrCode InstallChangeObserver ()
{
    // Creation, project-wide (nullptr = every element type).
    const GSErrCode newErr = ACAPI_Element_CatchNewElement (nullptr, ElementEventHandler);
    if (newErr != NoError)
        return newErr;

    // Change/delete/undo on elements that have an observer attached.
    return ACAPI_Element_InstallElementObserver (ElementEventHandler);
}

GSErrCode UninstallChangeObserver ()
{
    // Both handlers come off, and the first failure is not allowed to leave the
    // second one installed — a dangling callback into an unloading add-on is the
    // worst outcome available here.
    const GSErrCode newErr = ACAPI_Element_CatchNewElement (nullptr, nullptr);
    const GSErrCode obsErr = ACAPI_Element_InstallElementObserver (nullptr);
    ChangeTracker::Get ().SetWatching (false, 0);
    return (newErr != NoError) ? newErr : obsErr;
}

namespace {

// The attach loop, shared by both entry points. Honours the limits and the
// Cancel button, and counts everything it did and did not do.
void AttachEach (const GS::Array<API_Guid>& guids, const AttachLimits& limits,
                 const std::function<bool ()>& shouldStop, AttachReport& report)
{
    const int64_t attachStart = NowMs ();

    for (const API_Guid& guid : guids) {
        // Both limits are checked BEFORE the call, so neither can be overshot by
        // a slow element. The budget is what actually protects the UI.
        if (report.attached + report.failed >= limits.maxElements ||
            NowMs () - attachStart >= limits.budgetMs) {
            report.truncated = true;
            break;
        }
        if (shouldStop != nullptr && shouldStop ()) {
            report.cancelled = true;
            break;
        }

        const GSErrCode attachErr = ACAPI_Element_AttachObserver (guid);
        // APIERR_LINKEXIST = already observed, which is the normal case on a
        // re-arm and not a failure. A genuinely bad guid is counted and skipped
        // rather than failing the whole watch: one unobservable element must not
        // cost the caller the other few thousand. But it IS counted — a silent
        // skip is how "attached: 20" can mean either "20 elements" or "20 of
        // 3000", and those need different fixes.
        if (attachErr == NoError || attachErr == APIERR_LINKEXIST) {
            ++report.attached;
        } else {
            ++report.failed;
            if (report.firstError.IsEmpty ())
                report.firstError = evp::DescribeErr (attachErr);
        }
    }

    report.attachMs = NowMs () - attachStart;

    // Archicad's own tally, not ours. When the two disagree, ours is the wrong
    // one — this is the number that says what is really being watched.
    GS::Array<API_Guid> observed;
    if (ObservedElements (observed) == NoError)
        report.observed = (uint32_t) observed.GetSize ();
}

}   // namespace

GSErrCode AttachObserversToAll (GSFlags filterFlags, const AttachLimits& limits,
                                const std::function<bool ()>& shouldStop,
                                AttachReport& report, GS::UniString& err)
{
    report = AttachReport ();

    GS::Array<API_Guid> guids;
    const int64_t listStart = NowMs ();
    const GSErrCode listErr = ACAPI_Element_GetElemList (
        API_ZombieElemID, &guids, (API_ElemFilterFlags) filterFlags);
    report.listMs = NowMs () - listStart;
    if (listErr != NoError) {
        err = EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", listErr,
                              "listing the elements to attach change observers to");
        return listErr;
    }
    report.listed = (uint32_t) guids.GetSize ();

    AttachEach (guids, limits, shouldStop, report);
    return NoError;
}

GSErrCode AttachObserversTo (const GS::Array<API_Guid>& guids,
                             AttachReport& report, GS::UniString& err)
{
    (void) err;         // no listing step here, so nothing can fail before the loop
    report = AttachReport ();
    report.listed = (uint32_t) guids.GetSize ();

    // No budget: the caller named these, so the cost is theirs to know and is
    // bounded by the list they passed. This is the path that does not surprise
    // anyone — see AttachLimits for what the unbounded version did.
    AttachLimits unlimited;
    unlimited.maxElements = (uint32_t) guids.GetSize ();
    unlimited.budgetMs    = 60 * 1000;      // a backstop, not a policy

    AttachEach (guids, unlimited, nullptr, report);
    return NoError;
}

GSErrCode ObservedElements (GS::Array<API_Guid>& guids)
{
    guids.Clear ();

    GS::Array<API_Elem_Head> heads;
    const GSErrCode err = ACAPI_Notification_GetObservedElements (&heads);
    if (err != NoError)
        return err;

    for (const API_Elem_Head& head : heads)
        guids.Push (head.guid);
    return NoError;
}

} // namespace geomsrv
