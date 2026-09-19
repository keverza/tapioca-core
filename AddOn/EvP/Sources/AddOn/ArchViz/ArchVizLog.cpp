#include "APIEnvir.h" // Win32Interface only -- no ACAPinc.h, exactly as PathUtils.cpp

#include "ArchViz/ArchVizLog.hpp"

#include "Python/PathUtils.hpp" // ACAPI-FREE by design — see its header. That is
                                // what makes it legal from the render thread.

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

std::mutex logMutex;

// ⚠️ ONE HANDLE FOR THE SESSION, AND IT IS A MEASURED
// CHANGE RATHER THAN A TIDY ONE. This used to resolve the data directory TWICE,
// verify the log directory chain, stat the file for rotation, open it, write,
// `FlushFileBuffers` and close -- per line. Replaying that exact sequence of
// Win32 calls against this machine's own log volume, 400 lines at a time:
//
//     as it stands             2609.1 us/line
//     open/close, no flush      247.4 us/line
//     handle held, no flush        3.1 us/line
//
// 2.6 MILLISECONDS a line, 830x what it needs to be. A frame at 60 Hz is 16.7
// ms, and this function is called from the render thread as well as the main
// one, so a single line was a sixth of a frame budget. The four-line sun block
// that ModelWatch emitted on every environment poll cost 10.4 ms of main thread
// each time it ran.
//
// ⚠️ THE FLUSH WAS THE LARGER HALF AND IT BOUGHT
// NOTHING. `FlushFileBuffers` forces the write past the OS file cache to the
// device; it protects against a power cut or a bugcheck. It does NOT protect
// against the case it was there for -- a process that dies. Windows' file cache
// is kernel-side, so a force-quit, an access violation or a kill still lands
// every buffered byte on disk, and a reader tailing the file sees them
// immediately either way. `PathUtils::AppendTextLine` keeps its flush, because
// the startup breadcrumbs it serves are genuinely about code that may not
// survive the next STATEMENT and are a handful of lines per session; this is
// the log that runs at kilohertz.
//
// ⚠️ THE HANDLE IS CLOSED AT `FreeData`, LIKE EVERY
// OTHER RESOURCE THIS DLL HOLDS. It is reopened lazily, so a line written after
// the close simply opens it again -- there is no window in which logging is
// silently dropped, which would be the one failure mode that hides its own
// cause.
const uint64_t kLogCapBytes = 5ull * 1024ull * 1024ull;

GS::UniString g_path;
HANDLE g_file = INVALID_HANDLE_VALUE;
uint64_t g_bytes = 0;

GS::UniString ArchVizLogPath ()
{
    const GS::UniString dataDir = evp::EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    return dataDir + GS::UniString ("\\logs\\archviz.log");
}

void CloseLocked ()
{
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle (g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_bytes = 0;
}

// The shared 5 MiB cap, "<name>.1<ext>", discarding the older generation --
// the same scheme every other host log rotates under (PathUtils.cpp).
//
// ⚠️ THE HANDLE COMES OUT FIRST. A file cannot be
// renamed while this process holds it open, and a rotation that silently failed
// would grow one log without limit while reporting that it had rotated.
void RotateLocked ()
{
    CloseLocked ();
    if (g_path.IsEmpty ())
        return;

    const UIndex dot = g_path.FindLast ('.');
    const UIndex sep = g_path.FindLast ('\\');
    GS::UniString backup;
    if (dot != MaxUIndex && (sep == MaxUIndex || dot > sep))
        backup =
            g_path.GetSubstring (0, dot) + GS::UniString (".1") + g_path.GetSubstring (dot, g_path.GetLength () - dot);
    else
        backup = g_path + GS::UniString (".1");

    ::DeleteFileW ((LPCWSTR) backup.ToUStr ().Get ());
    ::MoveFileW ((LPCWSTR) g_path.ToUStr ().Get (), (LPCWSTR) backup.ToUStr ().Get ());
}

bool OpenLocked ()
{
    if (g_file != INVALID_HANDLE_VALUE)
        return true;

    if (g_path.IsEmpty ()) {
        g_path = ArchVizLogPath ();
        if (g_path.IsEmpty ())
            return false; // no %LOCALAPPDATA%; nothing to do, and not worth failing a render over
    }

    // CreateDirectoryChain, because the open below does not create parents and on
    // a fresh install logs\ does not exist -- the mistake AddOnMain's startup log
    // already paid for once. Once per open rather than once per line.
    const GS::UniString logs (evp::EvpDataDir () + GS::UniString ("\\logs"));
    evp::CreateDirectoryChain (logs);

    // FILE_SHARE_READ so the log can be tailed while a run is in flight.
    g_file = ::CreateFileW ((LPCWSTR) g_path.ToUStr ().Get (), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = {};
    g_bytes = ::GetFileSizeEx (g_file, &size) != 0 ? (uint64_t) size.QuadPart : 0;
    return true;
}

// Wall-clock stamp. Lines from the render thread, the extraction thread and the
// main thread interleave here, and "how long did that take" is answered by
// reading two of them.
std::string Stamp ()
{
    const std::time_t now = std::time (nullptr);
    std::tm tm = {};
    if (localtime_s (&tm, &now) != 0)
        return std::string ("--:--:--");
    char buf[16] = {};
    std::snprintf (buf, sizeof (buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string (buf);
}

} // namespace

void ArchVizLog (const std::string& line)
{
    // ⚠️ BUILT OUTSIDE THE LOCK, AND THE BYTES ARE
    // WRITTEN AS THEY ARE. Callers already hand UTF-8 (the warning glyphs in
    // every one of these files are UTF-8 literals); the old path converted to
    // GS::UniString and straight back to UTF-8, which is an identity for valid
    // input and a corruption for invalid input.
    const std::string text (Stamp () + "  " + line + "\r\n");

    std::lock_guard<std::mutex> lock (logMutex);
    if (!OpenLocked ())
        return;

    if (g_bytes + text.size () > kLogCapBytes) {
        RotateLocked ();
        if (!OpenLocked ())
            return;
    }

    DWORD written = 0;
    if (::WriteFile (g_file, text.data (), (DWORD) text.size (), &written, nullptr) == 0) {
        // The handle stopped being usable -- the file was moved out from under
        // it, or the volume went away. Drop it; the next line reopens.
        CloseLocked ();
        return;
    }
    g_bytes += written;
}

void ArchVizLogClose ()
{
    std::lock_guard<std::mutex> lock (logMutex);
    CloseLocked ();
}

} // namespace archviz
} // namespace geomsrv
