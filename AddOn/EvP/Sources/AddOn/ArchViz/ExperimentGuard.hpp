#ifndef EVP_ARCHVIZ_EXPERIMENTGUARD_HPP
#define EVP_ARCHVIZ_EXPERIMENTGUARD_HPP

// The crash-loop guard for camera-sync experiments (PLAT-RE81).
//
// WHY THIS EXISTS AT ALL. The camera-sync ladder ends in a DLL that detours
// Archicad's own D3D11 present path (PLAT-RE78/RE79). A failure there is not a
// bad frame -- it is an Archicad that does not reach its UI, and an add-on
// cannot be switched off from inside an Archicad that never finishes starting.
// Every other reversibility measure in that plan (a runtime mode, nothing
// persisted, one commit per step) assumes the user can still get to a command
// line inside Archicad. This is the one that holds when they cannot.
//
// THE MECHANISM IS ONE FILE A PROCESS, DELIBERATELY.
//
//   * `EXPERIMENT_ARMED` is written BEFORE an experimental mechanism installs
//     itself, HELD OPEN while it is live, and deleted AFTER it has cleanly torn
//     down (ArchViz/Breadcrumb.hpp). Finding it at startup with nobody holding it
//     therefore means exactly one thing: a session died while that mechanism was
//     live. The session that finds it refuses to arm anything experimental, and
//     deletes it -- so ONE bad launch costs one degraded session, not a loop.
//
//   * `SAFE_MODE` is the user's own switch, created by hand from Explorer with
//     Archicad closed. It is never deleted by us, so it holds until they remove
//     it. This is the documented recovery instruction and it needs no rebuild,
//     no console and no working add-on UI.
//
// ⚠️ ONE FIXED FILENAME, NOT `ARMED_<mode>`. A per-mode name would need a
// directory enumeration at startup, on a path that must work when everything
// else is broken; the mode goes in the file's CONTENTS instead. It also means
// the recovery instruction is one exact path that can be given to the user
// verbatim rather than a wildcard they have to interpret.
//
// ⚠️ TWO ARCHICADS DO NOT CONFUSE EACH OTHER ANY MORE, AND THE OLD TRADE-OFF WAS
// WRONG ABOUT WHICH WAY THEY DID. It was accepted on the grounds that the failure
// was a missed guard on an experimental feature. But the 3D overlay's own camera
// sync arms 'hookdiag', so the second Archicad to start read the first's live
// breadcrumb as a crash and BLOCKED: no hooks and no 3D overlay for its whole
// session (2026-10-02, `BLOCKED AT Hook`), the first's breadcrumb deleted on the
// way. A held file has no stale-entry problem -- Windows lets go of it when its
// process dies -- so each running process holds its own (`EXPERIMENT_ARMED`, then
// `-2` ...), and the startup check passes over the held ones.
//
// ⚠️ WITHIN ONE PROCESS IT IS STILL ONE BREADCRUMB. Every mechanism that arms
// rewrites the same file with its mode, and the first `Disarm` deletes it while
// another mechanism may still be live -- as before.
//
// THREAD SAFETY: `CheckAtStartup` runs once from `Initialize`; everything after
// it is main-thread only, called from the camera-sync mode switch.

#include <string>

namespace geomsrv {
namespace archviz {
namespace experimentguard {

// Consult the two files and latch the verdict for this session. Call ONCE, from
// the add-on's `Initialize`, before anything can arm. Safe to call with no
// %LOCALAPPDATA% -- it then blocks experiments, because a guard that cannot
// write its breadcrumb cannot protect the next launch either.
void CheckAtStartup ();

// True when experimental mechanisms must refuse to arm for this whole session.
bool Blocked ();

// Why, in a sentence fit for a command's error message. Empty when not blocked.
const std::string& WhyBlocked ();

// The two files, as absolute paths, reported by the code that actually uses
// them.
//
// ⚠️ SO THE RECOVERY INSTRUCTION CANNOT DRIFT FROM THE IMPLEMENTATION AGAIN. The
// docs said `ARMED_<mode>` and the code wrote `EXPERIMENT_ARMED` for weeks; the
// mismatch surfaced only when the guard was hand-tested for the first time, and
// the same wrong name was in "what to do if Archicad will not start" -- an
// instruction that would have done nothing at the one moment it was needed.
// Anything telling a user which file to create or delete must ask here rather
// than repeat a name from memory.
std::string BreadcrumbFilePath ();
std::string SafeModeFilePath ();

// Write the breadcrumb naming `mode`. Returns false with `error` filled if it
// cannot be written -- and ⚠️ THE CALLER MUST THEN NOT ARM. An experimental
// detour with no breadcrumb behind it is the exact situation this file exists to
// prevent, so a failed write is a refusal, never a warning.
bool Arm (const char* mode, std::string& error);

// Delete this process's breadcrumb after a clean teardown -- never another
// Archicad's. Idempotent.
void Disarm ();

} // namespace experimentguard
} // namespace archviz
} // namespace geomsrv

#endif
