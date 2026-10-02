// ArchViz/ExperimentGuard -- see the header for what the two files mean.

#include "ArchViz/ExperimentGuard.hpp"

#include "ArchViz/ArchVizLog.hpp" // ArchVizLog
#include "ArchViz/Breadcrumb.hpp" // Hold, Release, Sweep
#include "Python/PathUtils.hpp"   // EvpDataDir

#include <windows.h>

#include <vector>

namespace geomsrv {
namespace archviz {
namespace experimentguard {

namespace {

bool g_checked = false;
bool g_blocked = false;
std::string g_why;
// The breadcrumb this process holds while something of it is armed (Breadcrumb.hpp).
breadcrumb::Held g_held;

// ⚠️ NO SUBDIRECTORY. Both files sit directly in the Tapioca root, next to
// `logs\` -- a recovery instruction the user follows while Archicad is broken
// should not ask them to navigate anywhere they might mistype.
GS::UniString BreadcrumbPath ()
{
    const GS::UniString root = evp::EvpDataDir ();
    return root.IsEmpty () ? GS::UniString () : root + GS::UniString ("\\EXPERIMENT_ARMED");
}

GS::UniString SafeModePath ()
{
    const GS::UniString root = evp::EvpDataDir ();
    return root.IsEmpty () ? GS::UniString () : root + GS::UniString ("\\SAFE_MODE");
}

// ⚠️ THE NAMES THE DOCS TOLD PEOPLE TO USE, HONOURED AS WELL AS THE REAL ONE.
// The plan and the handoff both describe the breadcrumb as `ARMED_<mode>`; the
// implementation settled on one fixed `EXPERIMENT_ARMED` instead, and nothing
// reconciled the two. On 2026-08-13 that was tested for the first time: the
// hand-test asked for `ARMED_hookdiag`, nothing read it, and hookdiag armed --
// which looked exactly like a broken guard.
//
// The real cost is not the failed test. It is that "delete %LOCALAPPDATA%\
// Tapioca\ARMED_*" is the RECOVERY instruction for an Archicad that will not
// start, and it would have done nothing at the one moment it mattered. So both
// spellings are now checked. Named explicitly rather than by wildcard, because
// enumerating a directory at startup is what the fixed name existed to avoid.
const wchar_t* const kLegacyBreadcrumbNames[] = {
    L"\\ARMED_hookdiag",
    L"\\ARMED_hookdraw",
    L"\\ARMED_wake",
    L"\\ARMED_hideonnav",
};

std::wstring Wide (const GS::UniString& text)
{
    return std::wstring ((const wchar_t*) text.ToUStr ().Get ());
}

// Every file a breadcrumb may be: each process's (Breadcrumb.hpp), then the legacy
// spellings, which nothing writes and so nothing holds.
std::vector<std::wstring> BreadcrumbPaths ()
{
    std::vector<std::wstring> paths = breadcrumb::PathsOf (Wide (BreadcrumbPath ()));
    const GS::UniString root = evp::EvpDataDir ();
    for (const wchar_t* name : kLegacyBreadcrumbNames)
        paths.push_back (Wide (root) + name);
    return paths;
}

// UTF-8 out of a UniString, the way PathUtils does it. The no-argument
// `ToCStr()` is a different (locale) conversion; do not substitute it.
std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

void Block (const std::string& why)
{
    g_blocked = true;
    g_why = why;
    ArchVizLog ("experiment guard: BLOCKED for this session -- " + why);
}

} // namespace

void CheckAtStartup ()
{
    if (g_checked)
        return;
    g_checked = true;

    const GS::UniString breadcrumb = BreadcrumbPath ();
    if (breadcrumb.IsEmpty ()) {
        // No %LOCALAPPDATA% means no breadcrumb can be written later either, so
        // the next launch would have no protection. Refusing now is the only
        // answer that stays consistent.
        Block ("%LOCALAPPDATA% is unavailable, so no crash-loop breadcrumb can be "
               "written; experimental camera-sync modes are unavailable");
        return;
    }

    if (evp::PathExists (SafeModePath ())) {
        Block ("SAFE_MODE is present in the Tapioca folder; delete it to re-enable "
               "experimental camera-sync modes");
        return;
    }

    // ⚠️ DELETED HERE, BEFORE THE BLOCK IS LATCHED. One bad launch must cost
    // ONE degraded session; leaving the file behind would make every
    // subsequent launch refuse too, which is a different kind of stuck.
    // ⚠️ AND ONLY WHAT NO RUNNING PROCESS HOLDS. Another Archicad's breadcrumb, held
    // while its modes are armed, is not a crash: it is neither read as one nor deleted.
    const breadcrumb::Found found = breadcrumb::Sweep (BreadcrumbPaths ());
    if (found.held > 0)
        ArchVizLog ("experiment guard: " + std::to_string (found.held) +
                    " breadcrumb(s) held by another running Archicad -- its armed modes, not a crash");
    if (!found.left.empty ()) {
        std::string armedMode = found.left.front ();
        if (armedMode.empty ())
            armedMode = "(unnamed -- a hand-written breadcrumb)";
        Block ("the previous Archicad session ended while the experimental mode '" + armedMode +
               "' was armed; experimental camera-sync modes are disabled for this "
               "session and will be available again after the next restart");
        return;
    }

    ArchVizLog ("experiment guard: clean start, experimental camera-sync modes available");
}

bool Blocked ()
{
    return g_blocked;
}

std::string BreadcrumbFilePath ()
{
    return Utf8 (BreadcrumbPath ());
}

std::string SafeModeFilePath ()
{
    return Utf8 (SafeModePath ());
}

const std::string& WhyBlocked ()
{
    return g_why;
}

bool Arm (const char* mode, std::string& error)
{
    if (g_blocked) {
        error = g_why;
        return false;
    }

    const GS::UniString path = BreadcrumbPath ();
    if (path.IsEmpty ()) {
        error = "%LOCALAPPDATA% is unavailable, so the crash-loop breadcrumb cannot be written";
        return false;
    }

    // Armed again while armed: the one breadcrumb this process holds names the new mode,
    // as the single file always has.
    if (g_held.file != nullptr) {
        if (!breadcrumb::Rewrite (g_held, mode)) {
            error = "the crash-loop breadcrumb could not be rewritten; refusing to arm without it";
            return false;
        }
    }
    else {
        g_held = breadcrumb::Hold (Wide (path), mode);
        if (g_held.file == nullptr) {
            error = "the crash-loop breadcrumb could not be written (the Tapioca folder refused it, or "
                    "every one of EXPERIMENT_ARMED to EXPERIMENT_ARMED-" +
                    std::to_string (breadcrumb::kMaxHolders) +
                    " is held by a running Archicad); refusing to arm without it";
            return false;
        }
    }

    std::string where;
    if (g_held.path != Wide (path)) {
        // EXPERIMENT_ARMED-<n>: the name is ASCII.
        where = " in ";
        for (wchar_t c : g_held.path.substr (g_held.path.find_last_of (L'\\') + 1))
            where += c < 128 ? char (c) : '?';
        where += " -- another running Archicad holds EXPERIMENT_ARMED";
    }
    ArchVizLog ("experiment guard: armed '" + std::string (mode) + "'" + where);
    return true;
}

void Disarm ()
{
    // ⚠️ ONLY THIS PROCESS'S. Another Archicad's breadcrumb guards its own session.
    if (g_held.file == nullptr)
        return;
    breadcrumb::Release (g_held);
    ArchVizLog ("experiment guard: disarmed cleanly");
}

} // namespace experimentguard
} // namespace archviz
} // namespace geomsrv
