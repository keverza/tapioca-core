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

#include <cstdint>
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

Stats Get ();

} // namespace modelwatch
} // namespace archviz
} // namespace geomsrv

#endif
