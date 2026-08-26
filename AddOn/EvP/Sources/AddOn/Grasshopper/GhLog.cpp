#include "GhLog.hpp"

#include "Python/PathUtils.hpp" // EvpDataDir / CreateDirectoryChain / AppendTextLine

namespace evp {
namespace grasshopper {

namespace {

GS::UniString Stamp (uint32_t generation, uint32_t pid)
{
    if (pid == 0)
        return GS::UniString::Printf ("[gen %u pid -] ", (unsigned int) generation);
    return GS::UniString::Printf ("[gen %u pid %u] ", (unsigned int) generation, (unsigned int) pid);
}

} // namespace

GS::UniString LogPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    const GS::UniString logs (dataDir + GS::UniString ("\\logs"));
    if (!evp::CreateDirectoryChain (logs))
        return GS::UniString ();
    return logs + GS::UniString ("\\grasshopper.log");
}

void LogLine (uint32_t generation, uint32_t pid, const GS::UniString& line)
{
    const GS::UniString path = LogPath ();
    if (!path.IsEmpty ())
        evp::AppendTextLine (path, Stamp (generation, pid) + line);
}

void LogWorkerLine (uint32_t generation, uint32_t pid, const GS::UniString& line)
{
    const GS::UniString path = LogPath ();
    if (!path.IsEmpty ())
        evp::AppendTextLine (path, Stamp (generation, pid) + GS::UniString ("[worker] ") + line);
}

} // namespace grasshopper
} // namespace evp
