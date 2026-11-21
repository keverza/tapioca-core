// ArchViz/ExtractionDeferredUv -- see the header.

#include "ArchViz/ExtractionDeferredUv.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "Geometry/MeshStore.hpp"

#include <atomic>
#include <mutex>

namespace geomsrv {
namespace archviz {
namespace extractiondeferreduv {

namespace {

std::mutex& Mutex ()
{
    static std::mutex mutex;
    return mutex;
}

std::set<std::string>& Pending ()
{
    static std::set<std::string> pending;
    return pending;
}

std::atomic<bool> g_suspended { false };

} // namespace

void Note (const std::string& guid)
{
    if (guid.empty ())
        return;
    std::lock_guard<std::mutex> lock (Mutex ());
    Pending ().insert (guid);
}

std::set<std::string> Take ()
{
    std::lock_guard<std::mutex> lock (Mutex ());
    std::set<std::string> taken;
    taken.swap (Pending ());
    return taken;
}

bool Suspended ()
{
    return g_suspended.load ();
}

} // namespace extractiondeferreduv

// After a complete full pass: the textured elements again, with their coordinates. A partial
// pass reads them, so this does not come back here. ⚠️ A GUID THE MODEL NO LONGER HAS IS
// REMOVED by that pass, which is right: it was deleted after the full pass saw it.
void ExtractionWorker::RunDeferredUv (const Options& opt, bool afterCompleteFullPass)
{
    if (!afterCompleteFullPass)
        return; // a partial pass reads its coordinates; an unfinished full pass keeps them noted
    const std::set<std::string> guids = extractiondeferreduv::Take ();
    if (guids.empty () || stopFlag_.load () || extractiondeferreduv::g_suspended.load ())
        return;
    ArchVizLog ("extraction: texture coordinates for " + std::to_string (guids.size ()) +
                " textured elements, after the geometry (#14)");
    struct Suspend {
        Suspend ()
        {
            extractiondeferreduv::g_suspended.store (true);
        }
        ~Suspend ()
        {
            extractiondeferreduv::g_suspended.store (false);
        }
    } suspend;
    // ⚠️ A CAPTURE OF ITS OWN, SO THE STAMP MOVES. SnapshotAssembly::CanUpdate splices a partial
    // pass onto the full pass's snapshot only at that snapshot's stamp + 1 -- the step every edit
    // gets from the model watch before its pass starts. Without it this pass acquired at the full
    // pass's own stamp, was refused as `incompatible-incremental-baseline` and ran as ANOTHER full
    // pass (89 s live, 2026-11-21), and every edit during it restarted it as full: the whole scene
    // reloaded after each change. If Archicad changes the model meanwhile, the stamp is past + 1
    // and the pass is full, as it should be.
    MeshStore::Get ().BumpCaptureStamp ();
    RunPass (opt, /*full*/ false, guids, nullptr);
}

} // namespace archviz
} // namespace geomsrv
