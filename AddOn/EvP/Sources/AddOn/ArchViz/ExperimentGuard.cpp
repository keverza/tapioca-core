// ArchViz/ExperimentGuard -- see the header for what the two files mean.

#include "ArchViz/ExperimentGuard.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "Python/PathUtils.hpp"     // EvpDataDir, PathExists, ReadTextFile, WriteTextFile

#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace experimentguard {

namespace {

bool        g_checked = false;
bool        g_blocked = false;
std::string g_why;

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
    L"\\ARMED_hookdiag", L"\\ARMED_hookdraw", L"\\ARMED_wake", L"\\ARMED_hideonnav",
};

// The first breadcrumb present, under any of its spellings, or empty.
GS::UniString FindBreadcrumb ()
{
    const GS::UniString root = evp::EvpDataDir ();
    if (root.IsEmpty ())
        return GS::UniString ();
    const GS::UniString primary = BreadcrumbPath ();
    if (evp::PathExists (primary))
        return primary;
    for (const wchar_t* name : kLegacyBreadcrumbNames) {
        const GS::UniString candidate = root + GS::UniString (name);
        if (evp::PathExists (candidate))
            return candidate;
    }
    return GS::UniString ();
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

}   // namespace

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

    const GS::UniString found = FindBreadcrumb ();
    if (!found.IsEmpty ()) {
        GS::UniString armedMode;
        evp::ReadTextFile (found, armedMode);
        if (armedMode.IsEmpty ())
            armedMode = GS::UniString ("(unnamed -- a hand-written breadcrumb)");
        // ⚠️ DELETED HERE, BEFORE THE BLOCK IS LATCHED. One bad launch must cost
        // ONE degraded session; leaving the file behind would make every
        // subsequent launch refuse too, which is a different kind of stuck.
        ::DeleteFileW ((LPCWSTR) found.ToUStr ().Get ());
        Block ("the previous Archicad session ended while the experimental mode '" +
               Utf8 (armedMode) +
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

    GS::UniString writeError;
    if (!evp::WriteTextFile (path, GS::UniString (mode, CC_UTF8), writeError)) {
        error = "the crash-loop breadcrumb could not be written (" + Utf8 (writeError) +
                "); refusing to arm without it";
        return false;
    }

    ArchVizLog ("experiment guard: armed '" + std::string (mode) + "'");
    return true;
}

void Disarm ()
{
    const GS::UniString path = BreadcrumbPath ();
    if (path.IsEmpty () || !evp::PathExists (path))
        return;
    ::DeleteFileW ((LPCWSTR) path.ToUStr ().Get ());
    ArchVizLog ("experiment guard: disarmed cleanly");
}

}   // namespace experimentguard
}   // namespace archviz
}   // namespace geomsrv
