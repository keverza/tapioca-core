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
    bool     running       = false;
    uint32_t polls         = 0;   // ticks that actually asked Archicad
    uint32_t skippedBusy   = 0;   // ticks that found an extraction already running
    uint32_t refreshes     = 0;   // ticks that started a re-extraction
    int64_t  lastDiffMs    = 0;   // what the last poll cost
    int64_t  worstDiffMs   = 0;   // the worst one, which is what set the interval
    uint32_t intervalMs    = 0;   // the cadence it has settled on
    std::string lastError;        // last generator failure, empty when healthy
};

// Arm the watch (idempotent). `floorMs` is the FASTEST it will ever poll; the
// timer may choose to go slower on its own but never faster. Returns whether the
// timer is armed.
bool Start (uint32_t floorMs = 750);
void Stop ();

// Re-extract the model NOW, whatever the diff says. This is the manual Refresh:
// it exists because a poll can only report what Archicad's generator considers a
// change, and "the picture looks wrong, rebuild it" is a request no change
// detector can be asked to infer. Returns false when a pass is already running.
bool RefreshNow ();

Stats Get ();

}   // namespace modelwatch
}   // namespace archviz
}   // namespace geomsrv

#endif
