#include "ArchViz/ArchVizLog.hpp"

#include "Python/PathUtils.hpp"   // ACAPI-FREE by design — see its header. That is
                                  // what makes it legal from the render thread.

#include <cstdio>
#include <ctime>
#include <mutex>

namespace geomsrv {
namespace archviz {

namespace {

std::mutex logMutex;

GS::UniString ArchVizLogPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    return dataDir + GS::UniString ("\\logs\\archviz.log");
}

// Wall-clock stamp. Lines from the render thread, the extraction thread and the
// main thread interleave here, and "how long did that take" is answered by
// reading two of them.
std::string Stamp ()
{
    const std::time_t now = std::time (nullptr);
    std::tm           tm  = {};
    if (localtime_s (&tm, &now) != 0)
        return std::string ("--:--:--");
    char buf[16] = {};
    std::snprintf (buf, sizeof (buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string (buf);
}

}   // namespace

void ArchVizLog (const std::string& line)
{
    const GS::UniString path = ArchVizLogPath ();
    if (path.IsEmpty ())
        return;   // no %LOCALAPPDATA%; nothing to do, and not worth failing a render over

    std::lock_guard<std::mutex> lock (logMutex);
    // CreateDirectoryChain, because AppendTextLine does not create parents and on
    // a fresh install logs\ does not exist — the mistake AddOnMain's startup log
    // already paid for once.
    const GS::UniString dataDir = evp::EvpDataDir ();
    evp::CreateDirectoryChain (dataDir + GS::UniString ("\\logs"));
    evp::AppendTextLine (path, GS::UniString ((Stamp () + "  " + line).c_str (), CC_UTF8));
}

}   // namespace archviz
}   // namespace geomsrv
