#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ModelWatch.hpp"

#include "ArchViz/ArchVizLog.hpp"       // ArchVizLog -- one log for the whole viewer
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "Notify/ModelDiff.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>

namespace geomsrv {
namespace archviz {
namespace modelwatch {

namespace {

// The diff may occupy at most 1/kDutyDivisor of the interval. At 20, a diff
// measuring 40 ms settles the cadence at 800 ms; one measuring 500 ms settles it
// at 10 s. The point is not the constant, it is that the number comes from a
// MEASUREMENT of this project rather than from an assumption about projects.
constexpr int64_t kDutyDivisor = 20;

// However slow the diff turns out to be, stop backing off here: past this the
// viewer is no longer meaningfully following anything and the user should be
// reaching for Refresh instead. Logged when it is hit, so the ceiling explains
// itself rather than looking like the watch having died.
constexpr uint32_t kCeilingMs = 30000;

UINT_PTR gTimer     = 0;
uint32_t gFloorMs   = 750;
uint32_t gIntervalMs = 0;
bool     gCeilingLogged = false;

Stats gStats;

// ⚠️ THE WATCH'S OWN BASELINE, never shared with the bus. Polling ADOPTS the
// current state as the next comparison point, so if the viewer and a script
// called `EvP.GetModelDiff` against one baseline, whichever asked first would
// consume the change and the other would see an unchanged model. Notify/ModelDiff
// says the same thing at more length; this is the second consumer that makes it
// true rather than theoretical.
std::unique_ptr<modeldiff::Baseline> gBaseline;

void Rearm (uint32_t intervalMs);

// Start a fresh single pass. ⚠️ NOT StartLive: with observers off, the live loop
// runs one pass and exits anyway (ExtractionThread's PLAT-RE68 branch), so the
// honest call is the one that says "one pass" — this timer IS the liveness now.
bool StartPass ()
{
    if (ExtractionWorker::Get ().IsRunning ())
        return false;
    ExtractionWorker::Get ().Start (/*full*/ true);
    return true;
}

void CALLBACK WatchTimerProc (HWND, UINT, UINT_PTR, DWORD)
{
    // ⚠️ A PASS IN FLIGHT MEANS SKIP THE WHOLE TICK, diff included. Restarting
    // the worker joins it first, so asking while it runs would stall the main
    // thread; and the pass already running will see the newer model, so the
    // change is not lost by waiting. Not counted as a poll -- it asked Archicad
    // nothing.
    if (ExtractionWorker::Get ().IsRunning ()) {
        ++gStats.skippedBusy;
        return;
    }

    // Nothing to refresh once the viewport has gone. Stops the timer rather than
    // polling ACAPI forever for a window nobody is looking at.
    if (!DiligentViewport::Get ().IsRunning ()) {
        Stop ();
        return;
    }

    if (gBaseline == nullptr)
        return;

    const modeldiff::Result diff = gBaseline->Poll ();
    ++gStats.polls;

    if (!diff.ok) {
        // ⚠️ LOGGED ONCE PER DISTINCT MESSAGE, not once per tick. A generator
        // that fails usually keeps failing, and a per-tick log would bury
        // archviz.log under the same line at the poll rate.
        if (gStats.lastError != diff.error) {
            gStats.lastError = diff.error;
            ArchVizLog ("model watch: the difference generator failed -- " + diff.error);
        }
        return;
    }
    gStats.lastError.clear ();

    gStats.lastDiffMs  = diff.elapsedMs;
    gStats.worstDiffMs = std::max (gStats.worstDiffMs, diff.elapsedMs);

    // ---- adapt the cadence to what this project actually costs --------------
    // Driven by the WORST poll rather than the last one: the cost varies with
    // what changed, and a cadence set from a cheap tick would be undone by the
    // next expensive one and oscillate.
    const int64_t wanted = std::max<int64_t> (gFloorMs, gStats.worstDiffMs * kDutyDivisor);
    const uint32_t next  = (uint32_t) std::min<int64_t> (wanted, kCeilingMs);
    if (next != gIntervalMs) {
        if (next >= kCeilingMs && !gCeilingLogged) {
            gCeilingLogged = true;
            ArchVizLog ("model watch: the difference generator costs " +
                        std::to_string (gStats.worstDiffMs) +
                        " ms on this project, so the watch has backed off to its " +
                        std::to_string (kCeilingMs) +
                        " ms ceiling. Edits will take that long to appear; use Refresh "
                        "for an immediate rebuild.");
        }
        Rearm (next);
    }

    // `firstCall` is the baseline being established, NOT an unchanged model --
    // re-extracting on it would rebuild the scene the viewer has only just built.
    if (diff.firstCall || !diff.AnythingChanged ())
        return;

    if (StartPass ()) {
        ++gStats.refreshes;
        ArchVizLog ("model watch: re-extracting -- " + std::to_string (diff.created.size ()) +
                    " new, " + std::to_string (diff.modified.size ()) + " modified, " +
                    std::to_string (diff.deleted.size ()) + " deleted" +
                    (diff.environmentChanged ? ", environment changed" : ""));
    }
}

void Rearm (uint32_t intervalMs)
{
    if (gTimer != 0)
        ::KillTimer (nullptr, gTimer);
    gTimer = ::SetTimer (nullptr, 0, intervalMs, WatchTimerProc);
    if (gTimer == 0) {
        ArchVizLog ("model watch: SetTimer failed; the viewer will not follow edits. "
                    "Use Refresh to rebuild it by hand.");
        gStats.running = false;
        return;
    }
    gIntervalMs        = intervalMs;
    gStats.intervalMs  = intervalMs;
    gStats.running     = true;
}

}   // namespace

bool Start (uint32_t floorMs)
{
    gFloorMs = std::max<uint32_t> (floorMs, 100);

    // ⚠️ A FRESH BASELINE ON EVERY ARM. A baseline left over from a previous
    // session would make the first tick report every edit made while the viewer
    // was closed as if it had just happened -- a full re-extraction of a scene
    // that was correct when it was built.
    gBaseline = std::make_unique<modeldiff::Baseline> (modeldiff::Scope::Model);
    gStats = Stats {};
    gCeilingLogged = false;

    Rearm (gFloorMs);
    if (!gStats.running)
        return false;

    // Establish the baseline immediately rather than on the first tick, so the
    // interval between opening the viewer and the first poll is not a blind spot.
    const modeldiff::Result first = gBaseline->Poll ();
    if (!first.ok) {
        gStats.lastError = first.error;
        ArchVizLog ("model watch: could not establish a baseline -- " + first.error);
    } else {
        gStats.lastDiffMs  = first.elapsedMs;
        gStats.worstDiffMs = first.elapsedMs;
        ArchVizLog ("model watch: armed, polling every " + std::to_string (gIntervalMs) +
                    " ms (baseline took " + std::to_string (first.elapsedMs) + " ms). "
                    "No observers are attached and nothing is written to the project.");
    }
    return true;
}

void Stop ()
{
    if (gTimer != 0) {
        ::KillTimer (nullptr, gTimer);
        gTimer = 0;
    }
    gBaseline.reset ();
    gIntervalMs    = 0;
    gStats.running = false;
    gStats.intervalMs = 0;
}

bool RefreshNow ()
{
    if (!StartPass ())
        return false;
    ++gStats.refreshes;
    // ⚠️ RE-BASELINE TOO. Without this the next tick reports everything that
    // changed before the manual refresh as still outstanding, and immediately
    // re-extracts the model that was just rebuilt.
    if (gBaseline != nullptr)
        gBaseline->Poll (/*reset*/ true);
    ArchVizLog ("model watch: manual refresh -- re-extracting the whole model.");
    return true;
}

Stats Get () { return gStats; }

}   // namespace modelwatch
}   // namespace archviz
}   // namespace geomsrv
