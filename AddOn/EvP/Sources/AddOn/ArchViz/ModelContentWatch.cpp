// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/ModelContentWatch -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ModelContentWatch.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "Geometry/GeometryExtractor.hpp"

#include <Model.hpp>
#include <ModelElement.hpp>
#include <ModelMeshBody.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace geomsrv {
namespace archviz {
namespace modelcontentwatch {

namespace {

Reading Read (const ModelerAPI::Model& model)
{
    Reading reading;
    reading.count = ModelElementCount (model);
    if (reading.count > 0) {
        reading.first = ElementGuidAt (model, 1);
        reading.middle = ElementGuidAt (model, (reading.count + 1) / 2);
        reading.last = ElementGuidAt (model, reading.count);
    }
    return reading;
}

// Ticks between attempts after an acquire failed.
constexpr uint32_t kAcquireBackoffTicks = 8;

// ---- the sweep: the detector (see the header) ---------------------------------
//
// ⚠️ A HIDE THAT KEEPS THE ELEMENT IN THE MODEL'S LIST IS INVISIBLE TO `Read`: three elements
// hidden at 11:45 left the count at 3889 and the sampled GUIDs alone, and only the difference
// generator saw them -- whose poll costs up to 2833 ms of the main thread on that project, so it
// runs every 30 s. The walk sees them: measured 12:30-12:31, ~5.4 ms a walk over 3 ticks, a hide
// seen 12 s and an edit 10 s before the generator reported them.

constexpr double kSweepBudgetMs = 2.0;
// A sweep that found nothing different still says what it cost, this often.
constexpr uint32_t kSweepHeartbeat = 20;

Signatures g_sweepPrevious;
Signatures g_sweepCurrent;
int32_t g_sweepNext = 1; // 1-based, like every ModelerAPI index
int32_t g_sweepCount = 0;
int32_t g_sweepPreviousCount = -1;
double g_sweepMs = 0.0;
double g_sweepTickMaxMs = 0.0;
uint32_t g_sweepTicks = 0;
uint32_t g_sweeps = 0;

std::string SampleOf (const std::vector<std::string>& guids)
{
    std::string sample;
    for (size_t i = 0; i < guids.size () && i < 3; ++i)
        sample += (i > 0 ? " " : "") + guids[i];
    return sample;
}

void FinishSweep ()
{
    ++g_sweeps;
    const SweepDiff diff = g_sweepPrevious.empty () ? SweepDiff {} : Compare (g_sweepPrevious, g_sweepCurrent);
    const std::vector<std::string>& stamped = diff.stamped;
    const std::vector<std::string>& reshaped = diff.reshaped;
    const bool changed = diff.Any ();
    if (g_sweeps == 1 || changed || g_sweeps % kSweepHeartbeat == 0) {
        char cost[200] = {};
        std::snprintf (cost, sizeof (cost), "%d elements in %.1f ms over %u ticks (at most %.2f ms a tick)",
                       g_sweepCount, g_sweepMs, g_sweepTicks, g_sweepTickMaxMs);
        std::string line = "model watch: content sweep #" + std::to_string (g_sweeps) + ", " + cost;
        if (changed)
            line += "; since the last: change stamp moved on " + std::to_string (stamped.size ()) + " (" +
                    std::to_string (diff.stampedAndReshaped) + " of them reshaped), vertices changed on " +
                    std::to_string (reshaped.size ()) + " (to none " + std::to_string (diff.emptied) + ", from none " +
                    std::to_string (diff.filled) + "), appeared " + std::to_string (diff.appeared.size ()) +
                    ", vanished " + std::to_string (diff.vanished.size ()) + " -- e.g. stamp [" + SampleOf (stamped) +
                    "] vertices [" + SampleOf (reshaped) + "]";
        ArchVizLog (line);
    }
    // ⚠️ AND WHAT DIFFERS IS UPDATED: exactly those elements, or the model when they are many.
    if (changed)
        modelwatch::NoteSweepChange (diff.Changed (), size_t (std::max (g_sweepCount, 0)),
                                     g_sweepPreviousCount >= 0 && g_sweepPreviousCount != g_sweepCount);
    g_sweepPrevious.swap (g_sweepCurrent);
    g_sweepPreviousCount = g_sweepCount;
    g_sweepCurrent.clear ();
}

void SweepTick (const ModelerAPI::Model& model)
{
    if (g_sweepNext == 1) {
        g_sweepCount = model.GetElementCount ();
        g_sweepCurrent.clear ();
        g_sweepCurrent.reserve (size_t (std::max (g_sweepCount, 0)));
        g_sweepMs = 0.0;
        g_sweepTickMaxMs = 0.0;
        g_sweepTicks = 0;
    }
    if (g_sweepCount <= 0 || model.GetElementCount () != g_sweepCount) {
        g_sweepNext = 1; // the content changed under the walk; the next one starts over
        return;
    }
    const auto started = std::chrono::steady_clock::now ();
    double elapsed = 0.0;
    do {
        ModelerAPI::Element element;
        model.GetElement (g_sweepNext, &element);
        Signature signature;
        signature.genId = element.GetGenId ();
        const Int32 bodies = element.GetTessellatedBodyCount ();
        for (Int32 body = 1; body <= bodies; ++body) {
            ModelerAPI::MeshBody mesh;
            element.GetTessellatedBody (body, &mesh);
            signature.vertices += mesh.GetVertexCount ();
        }
        g_sweepCurrent[APIGuidToString (GSGuid2APIGuid (element.GetElemGuid ())).ToCStr ().Get ()] = signature;
        ++g_sweepNext;
        elapsed = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    } while (g_sweepNext <= g_sweepCount && elapsed < kSweepBudgetMs);
    g_sweepMs += elapsed;
    g_sweepTickMaxMs = std::max (g_sweepTickMaxMs, elapsed);
    ++g_sweepTicks;
    if (g_sweepNext > g_sweepCount) {
        FinishSweep ();
        g_sweepNext = 1;
    }
}

ModelerAPI::Model* g_model = nullptr;
Settle g_settle;
uint32_t g_acquireWait = 0;
Stats g_stats;

} // namespace

void Release ()
{
    delete g_model; // on the main thread, where it was made
    g_model = nullptr;
    g_settle.Reset (Reading {});
    g_stats.elements = -1;
    // A model taken afresh is compared with nothing: its first sweep is the baseline.
    g_sweepPrevious.clear ();
    g_sweepCurrent.clear ();
    g_sweepNext = 1;
    g_sweepPreviousCount = -1;
    g_sweeps = 0;
}

void Tick (bool threeDInFront)
{
    if (!threeDInFront) {
        Release ();
        return;
    }
    modelwatch::ServePending ();
    const bool passRunning = ExtractionWorker::Get ().IsRunning ();
    if (g_model == nullptr) {
        // ⚠️ NOT BEFORE A PASS IN FLIGHT HAS ACQUIRED ITS OWN: until then Archicad may still be
        // generating the 3D, and acquiring would wait for it here, on the main thread.
        const std::string phase = passRunning ? ExtractionWorker::Get ().Snapshot ().phase : std::string ();
        if (passRunning && phase != "extracting" && phase != "re-extracting")
            return;
        if (g_acquireWait > 0) {
            --g_acquireWait;
            return;
        }
        ModelerAPI::Model* model = new ModelerAPI::Model ();
        if (!AcquireCurrentModel (*model)) {
            delete model;
            g_acquireWait = kAcquireBackoffTicks;
            return;
        }
        g_model = model;
        g_settle.Reset (Read (*g_model));
        g_stats.elements = g_settle.Baseline ().count;
        ++g_stats.acquires;
        return;
    }

    const Reading now = Read (*g_model);
    const int32_t before = g_settle.Baseline ().count;
    if (g_settle.Observe (now)) {
        // ⚠️ THE REVISION MOVES AT ONCE, SO THE CAMERA RE-PINS; WHICH ELEMENTS TO READ IS THE
        // SWEEP'S, a walk later (~1 s) -- unless there is no walk to compare with yet.
        const bool sweepReads = !g_sweepPrevious.empty ();
        ++g_stats.changes;
        ArchVizLog ("model watch: the 3D window's content changed, " + std::to_string (before) + " -> " +
                    std::to_string (now.count) + " elements (isolation, a layer or a filter) -- " +
                    (sweepReads ? std::string ("the content sweep says which")
                                : std::string (passRunning ? "the pass in flight starts again on it"
                                                           : "re-extracting what it shows now")));
        g_stats.elements = now.count;
        modelwatch::NoteContentChanged (/*extract*/ !sweepReads && !passRunning);
    }
    SweepTick (*g_model);
}

Stats Get ()
{
    return g_stats;
}

} // namespace modelcontentwatch
} // namespace archviz
} // namespace geomsrv
