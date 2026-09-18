#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ModelWatch.hpp"

#include "ArchViz/ArchVizLog.hpp" // ArchVizLog -- one log for the whole viewer
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionEnvironment.hpp" // ReadEnvironment
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/Dxgi/CameraRecognizer.hpp" // census::NoteModelRevision
#include "ArchViz/Dxgi/HostOccluders.hpp"    // hostocclusion::SetModelRevision
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

UINT_PTR gTimer = 0;
uint32_t gFloorMs = 750;
uint32_t gIntervalMs = 0;
bool gCeilingLogged = false;

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
// See `SetKeepAlive`. Main thread, like everything in this file.
bool gKeepAlive = false;
// A change was seen while the worker was busy and still needs extracting.
bool gPendingRefresh = false;

bool StartPass ()
{
    if (ExtractionWorker::Get ().IsRunning ())
        return false;
    ExtractionWorker::Get ().Start (/*full*/ true);
    return true;
}

void CALLBACK WatchTimerProc (HWND, UINT, UINT_PTR, DWORD)
{
    // ⚠️ A PASS IN FLIGHT DEFERS THE RE-EXTRACTION, NOT THE
    // POLL. Skipping the whole tick was wrong in a way that took three runs to
    // see. The reasoning behind it was sound about GEOMETRY -- the pass already
    // running does observe the newer model, so nothing is lost on screen -- and
    // it was silent about the COUNTER. `geometryEdits` never moved for an edit
    // that arrived while the worker was busy.
    //
    // ⚠️ AND `geometryEdits` IS THE MODEL REVISION. It is what
    // `hostocclusion::SetModelRevision` publishes and what the camera recognizer
    // compares to decide that an index count moved because the MODEL moved. A
    // missed bump means the recognizer never learns the model changed, which is
    // an intermittent desync that depends on how fast the user edits -- and
    // "sometimes it desyncs, sometimes it does not" is exactly how it was
    // reported.
    //
    // Only `StartPass` joins the worker; `Poll` does not. So the poll runs, the
    // counter moves, and the refresh is remembered for the next tick.
    const bool busy = ExtractionWorker::Get ().IsRunning ();
    if (busy)
        ++gStats.skippedBusy;

    // Nothing to refresh once EVERY consumer has gone. Stops the timer rather
    // than polling ACAPI forever for a scene nobody is looking at.
    //
    // ⚠️ AND THE PORTABLE VIEWPORT IS NOT THE ONLY CONSUMER.
    // Testing only `DiligentViewport::IsRunning` made this tick suicide on its
    // first fire whenever the caller was the injected overlay, which has no such
    // viewport. See `SetKeepAlive`.
    if (!DiligentViewport::Get ().IsRunning () && !gKeepAlive) {
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

    gStats.lastDiffMs = diff.elapsedMs;
    gStats.worstDiffMs = std::max (gStats.worstDiffMs, diff.elapsedMs);

    // ---- adapt the cadence to what this project actually costs --------------
    // Driven by the WORST poll rather than the last one: the cost varies with
    // what changed, and a cadence set from a cheap tick would be undone by the
    // next expensive one and oscillate.
    const int64_t wanted = std::max<int64_t> (gFloorMs, gStats.worstDiffMs * kDutyDivisor);
    const uint32_t next = (uint32_t) std::min<int64_t> (wanted, kCeilingMs);
    if (next != gIntervalMs) {
        if (next >= kCeilingMs && !gCeilingLogged) {
            gCeilingLogged = true;
            ArchVizLog ("model watch: the difference generator costs " + std::to_string (gStats.worstDiffMs) +
                        " ms on this project, so the watch has backed off to its " + std::to_string (kCeilingMs) +
                        " ms ceiling. Edits will take that long to appear; use Refresh "
                        "for an immediate rebuild.");
        }
        Rearm (next);
    }

    // `firstCall` is the baseline being established, NOT an unchanged model --
    // re-extracting on it would rebuild the scene the viewer has only just built.
    if (diff.firstCall || !diff.AnythingChanged ()) {
        // A refresh deferred by a busy worker is taken as soon as one is free,
        // without waiting for another edit to come along and ask again.
        if (gPendingRefresh && StartPass ()) {
            gPendingRefresh = false;
            ++gStats.refreshes;
            ArchVizLog ("model watch: re-extracting a change that arrived while the "
                        "previous pass was still running");
        }
        return;
    }

    // ---- an environment-only change is NOT a reason to rebuild the model ----
    //
    // ⚠️ THIS IS THE DIFFERENCE BETWEEN ORBITING AND EDITING, AND ARCHICAD'S
    // GENERATOR DOES NOT MAKE IT FOR US. Its `isEnvironmentChanged` flag rises
    // whenever the 3D window's own projection or sun settings move -- which is
    // every time the user navigates. Treating that as a model change meant a
    // user who did nothing but orbit triggered a FULL re-extraction about twice
    // a second: 37 of them in 100 seconds on the run that found this, each one
    // destroying and recreating every element's vertex, index and side buffers
    // for geometry that was byte-for-byte identical.
    //
    // The environment is four floats and a direction. Reading it here and
    // pushing it is the whole correct response, and it costs no GPU resource at
    // all. ⚠️ MAIN THREAD, WHICH IS WHERE THIS TIMER ALREADY RUNS -- that is what
    // makes calling ACAPI from here legal (see ExtractionEnvironment.hpp).
    if (diff.created.empty () && diff.modified.empty () && diff.deleted.empty () && diff.environmentChanged) {
        EnvironmentUpload environment;
        if (ReadEnvironment (environment))
            SceneCmdQueue::Get ().PushEnvironment (environment);
        // ⚠️ COUNTED, NOT LOGGED. It happens on every poll during navigation, and
        // a line each would bury the events that matter in the one log the whole
        // viewer shares. ModelWatchState reports the counter.
        ++gStats.environmentOnly;
        return;
    }

    // ⚠️ BUMPED BEFORE THE PASS IS STARTED, AND WHETHER OR NOT IT STARTS. The
    // counter says "Archicad reported an element change", which is true even
    // when an extraction is already running and this tick's refresh is skipped.
    // Tying it to StartPass would lose exactly the edits that arrive during a
    // busy moment -- which is most of them during a drag.
    ++gStats.geometryEdits;

    // ⚠️ PUBLISHED HERE, NOT BY A COURIER ON ITS OWN CLOCK.
    // The occluder stamps `publishedRevision` at BeginBatch from the revision it
    // was last told, and it used to be told only by the overlay runtime's tick.
    // The pass that BeginBatch belongs to is started three lines below -- inside
    // this same tick -- so whenever the pass overtook the runtime tick, the batch
    // stamped the PREVIOUS revision and the chain read `model=6 published=5`
    // forever after, one behind and never catching up.
    //
    // ⚠️ AND THE GEOMETRY WAS ALWAYS CORRECT, WHICH IS WHY
    // IT SURVIVED SO LONG. Run 19:45 published `17/21 elements, 4798 triangles`
    // for exactly the edit the counter denied. Nothing was missing from the
    // screen; the only broken thing was the number the regression test reads to
    // decide whether anything is missing. A false FAIL costs as much as a false
    // PASS here, because the next stage of this project uses that test as its
    // oracle -- an oracle that cries wolf cannot referee a renderer migration.
    //
    // The runtime tick still publishes every tick. It is now a BACKSTOP for the
    // case where the watch is not running, not the only path.
    dxgi::hostocclusion::SetModelRevision (gStats.geometryEdits);
    dxgi::census::NoteModelRevision (gStats.geometryEdits);

    // ⚠️ AND IF THE PASS CANNOT START, REMEMBER IT. Waiting
    // for the NEXT change would leave this one unextracted indefinitely on a
    // model nobody touches again.
    gPendingRefresh = true;

    if (StartPass ()) {
        gPendingRefresh = false;
        ++gStats.refreshes;
        ArchVizLog ("model watch: re-extracting -- " + std::to_string (diff.created.size ()) + " new, " +
                    std::to_string (diff.modified.size ()) + " modified, " + std::to_string (diff.deleted.size ()) +
                    " deleted" + (diff.environmentChanged ? ", environment changed" : ""));
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
    gIntervalMs = intervalMs;
    gStats.intervalMs = intervalMs;
    gStats.running = true;
}

} // namespace

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
    }
    else {
        gStats.lastDiffMs = first.elapsedMs;
        gStats.worstDiffMs = first.elapsedMs;
        ArchVizLog ("model watch: armed, polling every " + std::to_string (gIntervalMs) + " ms (baseline took " +
                    std::to_string (first.elapsedMs) +
                    " ms). "
                    "No observers are attached and nothing is written to the project.");
    }
    return true;
}

void SetKeepAlive (bool keepAlive)
{
    gKeepAlive = keepAlive;
}

void Stop ()
{
    if (gTimer != 0) {
        ::KillTimer (nullptr, gTimer);
        gTimer = 0;
    }
    gBaseline.reset ();
    gIntervalMs = 0;
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

Stats Get ()
{
    return gStats;
}

} // namespace modelwatch
} // namespace archviz
} // namespace geomsrv
