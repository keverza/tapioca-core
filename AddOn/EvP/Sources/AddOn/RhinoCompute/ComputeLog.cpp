#include "RhinoCompute/ComputeLog.hpp"

#include "Python/PathUtils.hpp" // EvpDataDir / CreateDirectoryChain / AppendTextLine

namespace evp {
namespace rhinocompute {

namespace {

GS::UniString Stamp (uint32_t generation, uint32_t pid)
{
    if (pid == 0)
        return GS::UniString::Printf ("[gen %u pid -] ", (unsigned int) generation);
    return GS::UniString::Printf ("[gen %u pid %u] ", (unsigned int) generation, (unsigned int) pid);
}

} // namespace

// ⚠️ THE SEPARATORS HERE ARE DOUBLED, AND THAT IS NOT PEDANTRY. Written once as
// "\logs" and "\rhinocompute.log", this produced
// "...\Tapiocalogs<CR>hinocompute.log": MSVC drops the backslash in the
// unrecognised escape "\l", and "\r" IS A CARRIAGE RETURN. The result is a path
// that cannot be created, on a logger whose failure mode is silence — so the
// symptom was "no log at all", three layers away from a typo in a string.
//
// Every path separator in this file is "\\". A single backslash before a letter
// is either a different character or a dropped one, never what it looks like.
GS::UniString LogPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir (); // %LOCALAPPDATA%\Tapioca
    if (dataDir.IsEmpty ())
        return GS::UniString ();

    const GS::UniString logs (dataDir + GS::UniString ("\\logs"));
    if (!evp::CreateDirectoryChain (logs))
        return GS::UniString ();

    return logs + GS::UniString ("\\rhinocompute.log");
}

void LogLine (uint32_t generation, uint32_t pid, const GS::UniString& line)
{
    const GS::UniString stamped = Stamp (generation, pid) + line;
    const GS::UniString path = LogPath ();

    // A LOG THAT CAN VANISH IS WORSE THAN NO LOG, because its absence gets read
    // as "the code never ran". That is exactly what happened on the first live
    // run of the menu commands: startup.log proved both handlers fired, and
    // rhinocompute.log did not exist, which made a working command look dead.
    //
    // So a failed write falls back to startup.log, which is the channel already
    // proven to work in that same session, and it NAMES THE PATH IT TRIED —
    // which is what finally exposed the broken separators above. Losing the
    // dedicated file costs tidiness; losing the line costs the diagnosis.
    if (!path.IsEmpty () && evp::AppendTextLine (path, stamped))
        return;

    const GS::UniString reason = path.IsEmpty () ? GS::UniString ("no %LOCALAPPDATA% to build a log path from")
                                                 : GS::UniString ("could not append to ") + path;
    evp::StartupTrace (GS::UniString ("[rhinocompute log unavailable: ") + reason + GS::UniString ("] ") + stamped);
}

} // namespace rhinocompute
} // namespace evp
