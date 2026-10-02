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

#include <string>

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

ModelerAPI::Model* g_model = nullptr;
Settle g_settle;
bool g_passWasRunning = false;
uint32_t g_acquireWait = 0;
Stats g_stats;

} // namespace

void Release ()
{
    delete g_model; // on the main thread, where it was made
    g_model = nullptr;
    g_settle.Reset (Reading {});
    g_stats.elements = -1;
}

void Tick (bool threeDInFront)
{
    if (!threeDInFront) {
        Release ();
        return;
    }
    const bool passRunning = ExtractionWorker::Get ().IsRunning ();
    const bool passEnded = g_passWasRunning && !passRunning;
    g_passWasRunning = passRunning;
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
    if (passEnded) {
        const ExtractionWorker::Progress pass = ExtractionWorker::Get ().Snapshot ();
        if (pass.done && int32_t (pass.total) != now.count) {
            ++g_stats.changes;
            ArchVizLog ("model watch: the 3D window's content changed as the last pass ended, " +
                        std::to_string (pass.total) + " -> " + std::to_string (now.count) +
                        " elements -- re-extracting what it shows now");
            g_settle.Reset (now);
            g_stats.elements = now.count;
            modelwatch::NoteContentChanged (/*passRunning*/ false);
            return;
        }
    }
    const int32_t before = g_settle.Baseline ().count;
    if (!g_settle.Observe (now))
        return;

    ++g_stats.changes;
    ArchVizLog ("model watch: the 3D window's content changed, " + std::to_string (before) + " -> " +
                std::to_string (now.count) + " elements (isolation, a layer or a filter) -- " +
                (passRunning ? "the pass in flight starts again on it" : "re-extracting what it shows now"));
    g_stats.elements = now.count;
    modelwatch::NoteContentChanged (passRunning);
}

Stats Get ()
{
    return g_stats;
}

} // namespace modelcontentwatch
} // namespace archviz
} // namespace geomsrv
