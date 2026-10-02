#ifndef EVP_ARCHVIZ_BREADCRUMB_HPP
#define EVP_ARCHVIZ_BREADCRUMB_HPP

// ArchViz/Breadcrumb -- the experiment guard's crash-loop file (ExperimentGuard.hpp),
// held open by the process that wrote it for as long as it stands.
//
// ⚠️ A BREADCRUMB A RUNNING PROCESS HOLDS IS NOT A CRASH. Every Archicad that loads
// the add-on shares %LOCALAPPDATA%\Tapioca. The file used to be written and closed,
// so a second Archicad starting while the first had the 3D overlay on (its camera
// sync arms 'hookdiag') read the first's breadcrumb as a session that had died armed:
// it refused every guarded mode for its whole session -- no hooks, no 3D overlay,
// `BLOCKED AT Hook` -- and deleted the first's breadcrumb on the way, leaving the
// first unguarded. Now the writer holds the file, sharing only reads, until it
// disarms; Windows refuses another process's open while it does, and lets go of it
// when the process dies however it dies. A breadcrumb nobody holds is exactly one
// left by a session that ended armed.
//
// ⚠️ ONE FILE A PROCESS. Two Archicads may both be armed, so the second holds
// "<base>-2", and so on (logclaim::WriterPath names them). `Sweep` reads all of them.
//
// Plain Win32 and the standard library, so the offline suite can stand two
// processes up as two handles.

#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace breadcrumb {

// How many processes may be armed at once: `base`, then "-2" to "-9".
inline constexpr unsigned kMaxHolders = 9;

// `base` and its numbered siblings, the files `Hold` takes and `Sweep` reads.
std::vector<std::wstring> PathsOf (const std::wstring& base);

struct Held {
    void* file = nullptr; // a HANDLE open for writing, sharing only reads; nullptr when none
    std::wstring path;
};

// `contents` written into the first of `PathsOf (base)` no running process holds, and
// that file held. `file` stays nullptr when all are held or the folder cannot be
// written -- and the caller must then not arm (ExperimentGuard.hpp, `Arm`).
Held Hold (const std::wstring& base, const std::string& contents);

// What a held breadcrumb says, replaced.
bool Rewrite (const Held& held, const std::string& contents);

// Deleted while still held, then let go: there is no moment at which the file exists
// and nobody holds it, which another process starting would read as a crash.
void Release (Held& held);

struct Found {
    // What each breadcrumb a session left behind said -- read, then deleted.
    std::vector<std::string> left;
    // Held by a running process (or being released by one): left alone.
    unsigned held = 0;
};

// Every file in `paths` that exists: a file nobody holds is read and deleted, one a
// running process holds is only counted.
Found Sweep (const std::vector<std::wstring>& paths);

} // namespace breadcrumb
} // namespace archviz
} // namespace geomsrv

#endif
