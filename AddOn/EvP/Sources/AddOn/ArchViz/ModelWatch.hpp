#ifndef EVP_ARCHVIZ_MODELWATCH_HPP
#define EVP_ARCHVIZ_MODELWATCH_HPP

// ArchViz/ModelWatch — the viewer follows Archicad's edits, WITHOUT writing to
// the project (PLAT-RE125, and the answer to PLAT-RE69).
//
// WHY IT IS NOT AN OBSERVER. `ACAPI_Element_AttachObserver` is a DATABASE WRITE
// (PLAT-RE68): the project marks dirty and Archicad autosaves, so a viewer that
// merely watches ends up editing the file it is looking at. Arming was turned off
// for that reason, and the price was that the scene stopped following edits at
// all — `ExtractionWorker::StartLive` runs ONE pass and says so in the log. This
// puts the following back on a mechanism that writes nothing: Archicad's own
// difference generator, polled (Notify/ModelDiff).
//
// WHY IT IS A MAIN-THREAD WM_TIMER, like SelectionBridge and the camera sync.
// Both generator calls are ACAPI, so they may not run on the extraction thread
// or the render thread; a WM_TIMER runs on the thread that pumps the queue,
// where ACAPI is legal directly with no gate hop. It is also LOW PRIORITY —
// Windows delivers it only when the message queue is otherwise empty — so during
// a heavy edit or drag it simply does not fire, which for a refresh is exactly
// right.
//
// ⚠️ THE INTERVAL ADAPTS, AND IT MUST. `EvP.GetModelDiff`'s own header says the
// cost is UNMEASURED and warns against wiring it into a fixed fast loop. Rather
// than guess a number, the timer measures each poll and backs off so the diff
// stays a small fraction of the interval (kDutyDivisor below). On a small
// project it settles at the floor; on a project where the diff is expensive it
// finds a slower cadence by itself instead of competing with the user's edit.
//
// ⚠️ IT NEVER RE-EXTRACTS WHILE A PASS IS IN FLIGHT. Restarting the worker joins
// the running thread, so polling faster than a pass completes would stall the
// main thread on every tick. A tick that finds the worker busy skips, and the
// pass already running observes the newer model anyway.
//
// MAIN THREAD ONLY, all of it.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace geomsrv {
namespace archviz {
namespace modelwatch {

struct Stats {
    bool running = false;
    uint32_t polls = 0;       // ticks that actually asked Archicad
    uint32_t skippedBusy = 0; // ticks that found an extraction already running
    uint32_t refreshes = 0;   // ticks that started a re-extraction
    // Ticks where ONLY the environment moved, answered by re-reading the sun
    // instead of rebuilding the model. ⚠️ THE PAIR WITH `refreshes` IS THE
    // DIAGNOSIS: navigating Archicad's 3D window makes its own difference
    // generator report `isEnvironmentChanged` on every poll, so before this
    // split a user who simply orbited caused a FULL re-extraction roughly twice
    // a second -- every element's vertex, index and side buffers destroyed and
    // recreated, for a model that had not changed at all.
    uint32_t environmentOnly = 0;
    // Ticks where Archicad reported a GENUINE element change -- created,
    // modified or deleted. ⚠️ MONOTONIC, AND IT IS A SIGNAL RATHER THAN A
    // STATISTIC: the sun study follower polls it to learn that the model moved,
    // because MeshStore's snapshot is only republished by Tapioca.BuildSnapshot
    // and would otherwise still describe the building as it was. A counter is
    // the right shape because a poller that missed a tick must still see that
    // something happened, which a boolean flag cannot promise.
    uint32_t geometryEdits = 0;
    // Changes to what the 3D window SHOWS -- isolation, a layer, a filter -- that edit no
    // element (ModelContentWatch), and the revision both move: what the overlay stamps on its
    // camera and its occluder, so an edit and a visibility change re-pin the camera alike.
    uint32_t contentChanges = 0;
    uint32_t revision = 0;
    // Walks of the held model that found elements changed (ModelContentWatch's sweep), and the
    // generator's lists dropped because the sweep had already updated every element they named.
    uint32_t sweepChanges = 0;
    uint32_t alreadySwept = 0;
    // APINotify_ChangeProjectDB events seen, and the polls a hint brought forward.
    uint32_t projectChanges = 0;
    uint32_t earlyPolls = 0;
    // The project's modiStamp (JudgeStamp): moves seen, moves taken as a hint, and moves
    // dropped as a selection change or as the watch's own extraction.
    uint32_t stampChanges = 0;
    uint32_t stampHints = 0;
    uint32_t stampSelectionOnly = 0;
    uint32_t stampDuringPass = 0;
    // Regular ticks that asked Archicad nothing because the stamp had not moved (DiffWanted).
    uint32_t quietSkips = 0;
    // The 3D generator run as the backstop (stampwalk::Backstop3DDue) and what it last
    // cost; elements re-extracted because they touch a changed one (stampwalk::Neighbours).
    uint32_t backstopPolls = 0;
    int64_t lastBackstopMs = 0;
    uint32_t neighbourUpdates = 0;
    int64_t lastDiffMs = 0;  // what the last poll cost
    int64_t worstDiffMs = 0; // the worst one, which is what set the interval
    uint32_t intervalMs = 0; // the cadence it has settled on
    std::string lastError;   // last generator failure, empty when healthy
};

// Arm the watch (idempotent). `floorMs` is the FASTEST it will ever poll; the
// timer may choose to go slower on its own but never faster. Returns whether the
// timer is armed.
bool Start (uint32_t floorMs = 750);
void Stop ();

// MAIN THREAD. Keep the watch alive for a consumer that is NOT the portable
// Diligent viewport.
//
// ⚠️ THE TICK USED TO STOP ITSELF THE MOMENT THAT VIEWPORT
// WAS DOWN, AND THE INJECTED OVERLAY HAS NO SUCH VIEWPORT -- it composes into
// Archicad's own swap chain. So the watch armed, ticked once, found no viewport
// and killed itself: a whole log contained `model watch: armed` and not one
// `re-extracting` while the model went from 112 triangles to 96. Only the
// extraction that a VIEW SWITCH happens to trigger ever picked an edit up.
//
// That is the same fault as the overlay needing the portable viewport to arm and
// the depth target being published by a diagnostic: a production path inheriting
// a precondition only another component supplied. See OVERLAY-INVARIANTS.md
// section 9.
void SetKeepAlive (bool keepAlive);

// Re-extract the model NOW, whatever the diff says. This is the manual Refresh:
// it exists because a poll can only report what Archicad's generator considers a
// change, and "the picture looks wrong, rebuild it" is a request no change
// detector can be asked to infer. Returns false when a pass is already running.
bool RefreshNow ();

// MAIN THREAD, from the add-on's project-event handler: Archicad says its project
// database changed (`cause` is the event's parameter, logged). The next poll is
// brought forward instead of waiting out the backed-off cadence.
//
// ⚠️ WHY: on a large project the difference generator is slow (3,169 ms on an
// 806k-triangle model, 2026-10-31), the cadence backs off to its 30 s ceiling,
// and an edit sat unseen for up to 30 s. The event is caught with
// ACAPI_ProjectOperation_CatchProjectEvent, which writes nothing to the project,
// unlike an element observer (PLAT-RE68). It is a HINT, never the only path: the
// regular cadence still runs, so an edit that raises no event is still seen.
//
// ⚠️ AND IT CANNOT BECOME A STALL LOOP. An early poll costs what any poll costs
// on the main thread, so early polls are EarlyPollSpacing apart: at least
// kEarlyPollSpacingMs, at least kEarlyPollDutyFactor diffs, and doubled for each
// early poll in a row that found no element change (up to kEarlyPollMaxBackoff
// doublings). Any poll that finds an edit resets the backoff.
//
// ⚠️ MEASURED 2026-10-31: THIS EVENT DOES NOT FIRE FOR ELEMENT EDITS. Six
// add/delete edits on the 806k-triangle model raised no ChangeProjectDB at all,
// and every one waited for the 30 s cadence. The hint that does move is the
// project's modiStamp -- see the stamp timer and JudgeStamp below.
void NoteProjectChanged (int32_t cause);

constexpr int64_t kEarlyPollSettleMs = 400;    // quiet after the last event of a burst
constexpr int64_t kEarlyPollMaxDeferMs = 2000; // a long burst still polls this soon after it began
constexpr int64_t kEarlyPollSpacingMs = 5000;  // from the end of the previous poll, at least
constexpr int64_t kEarlyPollDutyFactor = 3;    // ... and at least this many diffs' cost
constexpr uint32_t kEarlyPollMaxBackoff = 3;   // doublings of the spacing after misses
// How often the stamp timer reads API_ProjectInfo.modiStamp. One read, no write.
constexpr uint32_t kStampPollMs = 500;
// A stamp move this soon after the watch's own extraction pass ends is taken as
// that pass's (3D generation, the sun follower's capture): seen 1-3 s after
// every update pass on 2026-10-31.
constexpr int64_t kOwnWorkSettleMs = 4000;
// However quiet the stamp is, the regular diff still runs this often.
constexpr int64_t kQuietBackstopMs = 300000;

// Whether a regular tick runs the difference generator -- pure, so tests/cpp
// pins it.
//
// ⚠️ THE DIFF IS WHAT FREEZES ARCHICAD. ~3.1 s of main thread on an
// 806k-triangle model, and it ran every 30 s whether or not anything had
// changed; the user saw Archicad stop responding around every update. A tick
// whose stamp has not moved since the last poll (other than by selection)
// skips it. A move during the watch's own work still counts as moved, so an
// edit made then is seen by the next regular tick, never lost.
inline bool DiffWanted (bool early, bool stampKnown, bool movedSincePoll, int64_t sinceLastPollMs)
{
    return early || !stampKnown || movedSincePoll || sinceLastPollMs >= kQuietBackstopMs;
}

// The spacing between early polls -- pure, so tests/cpp pins it.
inline int64_t EarlyPollSpacing (int64_t worstDiffMs, uint32_t misses)
{
    int64_t spacing = worstDiffMs * kEarlyPollDutyFactor;
    if (spacing < kEarlyPollSpacingMs)
        spacing = kEarlyPollSpacingMs;
    return spacing << (misses < kEarlyPollMaxBackoff ? misses : kEarlyPollMaxBackoff);
}

// What one stamp-timer tick makes of the project's modiStamp -- pure, so
// tests/cpp pins it.
//
// ⚠️ THE STAMP IS A HINT WITH KNOWN FALSE POSITIVES. It moves for an element
// edit, but also for a plain SELECTION change (ProjectCommands' GH2 Ping, which
// fingerprints the selection for exactly this reason) and possibly for model
// generation. A diff costs ~3 s of main thread on a large model, so a click that
// selects must not buy one. A move that came with a selection change is a hint
// only when an element that WAS selected no longer exists, which is what a
// delete of the selection looks like. A creation that also changed the
// selection is left to the regular cadence. While the watch's own extraction
// runs, or within kOwnWorkSettleMs of its end (`ownWork`), the move is adopted,
// never a hint -- but it still makes the next regular tick diff (DiffWanted).
enum class StampVerdict : uint8_t {
    Unchanged,
    DuringPass,    // the watch's own work may have moved it
    SelectionOnly, // the selection changed and everything it held still exists
    Hint           // bring the poll forward
};

inline StampVerdict JudgeStamp (bool stampMoved, bool ownWork, bool selectionChanged, bool previouslySelectedVanished)
{
    if (!stampMoved)
        return StampVerdict::Unchanged;
    if (ownWork)
        return StampVerdict::DuringPass;
    if (selectionChanged && !previouslySelectedVanished)
        return StampVerdict::SelectionOnly;
    return StampVerdict::Hint;
}

// When the brought-forward poll is due -- pure, so tests/cpp pins it. Never
// earlier than `now`, never sooner than `spacingMs` (EarlyPollSpacing) after the last poll
// ended, and never later than the poll already scheduled (`scheduledDue`): a
// result equal to `scheduledDue` means "leave the timer alone".
inline int64_t EarlyPollDue (int64_t now, int64_t burstStart, int64_t lastPollEnd, int64_t scheduledDue,
                             int64_t spacingMs = kEarlyPollSpacingMs)
{
    int64_t due = now + kEarlyPollSettleMs;
    if (burstStart + kEarlyPollMaxDeferMs < due)
        due = burstStart + kEarlyPollMaxDeferMs;
    if (due < now)
        due = now;
    if (due < lastPollEnd + spacingMs)
        due = lastPollEnd + spacingMs;
    return due < scheduledDue ? due : scheduledDue;
}

// What a poll's change list is answered with -- pure, so tests/cpp pins it.
//
// ⚠️ A FEW ELEMENTS ARE UPDATED BY GUID, NOT BY A FULL PASS: three hidden elements cost a whole
// re-extraction (2026-10-02 11:45: 4.9 s of a 3889-element project); updated by GUID they cost
// three. Past `kLargeChange`, or a quarter of the model, the walk reads most of it anyway and a
// full pass also keeps the substances and storey cuts current. And a LARGE list from a poll that
// follows a full pass the watch started since the previous poll is what that pass already read:
// isolating or showing all is seen within a tick by ModelContentWatch, which starts a full pass,
// and the generator, polled seconds later, reported the same change -- a second full pass ran
// for nothing (11:44:12, `3883 new`, 4.8 s).
enum class Route { Update, Full, AlreadyRead };
constexpr size_t kLargeChange = 500;
inline Route RouteChange (size_t changed, size_t elements, uint64_t lastFullStartMs, uint64_t previousPollMs)
{
    if (changed <= kLargeChange && changed * 4 <= elements)
        return Route::Update;
    if (previousPollMs != 0 && lastFullStartMs > previousPollMs)
        return Route::AlreadyRead;
    return Route::Full;
}

// The model size RouteChange compares a change list against. ModelContentWatch
// counts the held model, but it is ticked ONLY by the injected overlay's runtime,
// so with the palette viewer alone its count stays at -1 -- and an unknown size
// read as 0 sent every edit down the full pass: one new element on an
// 806k-triangle model re-extracted all 3,770 elements in 56.6 s (2026-10-31
// 12:23:36, #97), the precondition-from-another-component fault of
// OVERLAY-INVARIANTS.md section 9. Without that count, the published snapshot's
// mesh count stands in: never more than the elements, so the quarter-of-the-model
// rule only gets stricter, and nothing published yet still means a full pass.
inline size_t RouteModelSize (int32_t contentWatchElements, size_t publishedMeshes)
{
    return contentWatchElements > 0 ? size_t (contentWatchElements) : publishedMeshes;
}

// MAIN THREAD. Start what a busy worker had to leave -- a full pass, or the elements to update
// one by one -- once it is free. ModelContentWatch calls it every tick of the overlay's runtime,
// so it does not wait for the next poll of the generator (every 30 s on a large project).
void ServePending ();

// MAIN THREAD. What the 3D window shows changed without an element edit (ModelContentWatch):
// the revision moves -- the camera may re-pin -- and, with `extract`, a full re-extraction
// starts (or waits for the pass in flight).
void NoteContentChanged (bool extract);

// MAIN THREAD. The content sweep found these elements appeared, vanished, or with their change
// stamp or vertices moved: the revision moves, and they are updated (`RouteChange`), or the model
// is re-extracted when they are many -- unless a pass is running and the count changed under it,
// which starts that pass again by itself. Remembered until the next generator poll, which then
// drops a list naming nothing else.
void NoteSweepChange (const std::set<std::string>& changed, size_t elements, bool countChanged);

Stats Get ();
// Any thread: monotonic capture identity, including watch restarts/visibility
// changes. Stats itself remains main-thread-only.
uint64_t CaptureStamp ();

} // namespace modelwatch
} // namespace archviz
} // namespace geomsrv

#endif
