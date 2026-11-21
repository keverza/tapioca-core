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

bool FallBackToFull (bool refining, std::set<std::string>& filter, bool& partial, bool& full)
{
    if (refining) {
        {
            std::lock_guard<std::mutex> lock (Mutex ());
            Pending ().insert (filter.begin (), filter.end ());
        }
        ArchVizLog ("extraction: texture coordinates for " + std::to_string (filter.size ()) +
                    " elements wait -- the model changed; its own update goes first (#14)");
        return false;
    }
    // A pool renumber or missing/intermediate baseline cannot be spliced.
    // Use this same acquired model for a complete sliced viewer/study pass.
    filter.clear ();
    partial = false;
    full = true;
    MeshStore::Get ().SetFullPassInFlight (true);
    ArchVizLog ("pipeline: stage=geometry-shared-full reason=incompatible-incremental-baseline");
    return true;
}

} // namespace extractiondeferreduv

// After a complete pass: the textured elements noted by a full pass, again, with their
// coordinates -- right after that full pass, or after the edit's update that sent a texture pass
// back (FallBackToFull). An edit's own pass reads its coordinates and notes nothing. ⚠️ A GUID THE
// MODEL NO LONGER HAS IS REMOVED by this pass, which is right: it was deleted since.
void ExtractionWorker::RunDeferredUv (const Options& opt, bool afterCompletePass)
{
    if (!afterCompletePass)
        return; // an unfinished pass keeps them noted
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
    // ⚠️ AT THE FULL PASS'S OWN STAMP, NOT + 1: this pass refines that capture (SnapshotAssembly
    // `sameCapture`). Bumping the stamp here (cc5c25a4) made every edit during this pass
    // land at + 2 and turn into a full pass. An edit meanwhile refuses this pass instead
    // (FallBackToFull), and it runs again after that edit's update.
    RunPass (opt, /*full*/ false, guids, nullptr);
}

} // namespace archviz
} // namespace geomsrv
