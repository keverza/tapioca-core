#ifndef EVP_ARCHVIZ_PATCHPROFILE_HPP
#define EVP_ARCHVIZ_PATCHPROFILE_HPP

// Pinning the GPU-state hooks to ONE EXACT ARCHICAD BUILD, and refusing every
// other one (PLAT-RE153, docs/architecture/api/HANDOFF-OverlayPatch.md stage 1a).
//
// WHY THIS EXISTS. The camera-sync ladder ended with four rungs built, measured
// and still trailing, because all four learn Archicad's camera from an ACAPI
// read that happens AFTER Archicad has already drawn with it. The rung that
// removes the cause reads the transform Archicad itself uploaded to the GPU --
// which means hooking its device context, reading its constant buffers, and
// depending on where a matrix sits inside them. The user accepted that trade on
// 2026-09-07 in exactly these terms: a more invasive add-on that works against
// one exact build, rather than a portable one that keeps trailing.
//
// ⚠️ THE PRICE OF THAT TRADE IS THIS FILE, AND IT IS NOT OPTIONAL. A build-pinned
// patch that best-efforts its way through a mismatch is how this ends as a crash
// in someone else's Archicad: Graphisoft ships an update, a constant-buffer slot
// or a struct offset moves, and a hook that assumed the old layout reads a
// different buffer and feeds garbage into a live render. The symptom would be a
// corrupted frame or a hang inside Archicad's own renderer, with no obvious
// cause and nothing pointing at us. So the deal is: detect the build, verify the
// bytes, and refuse outright on anything else.
//
// ⚠️ IT FAILS CLOSED. No profile pinned, an unreadable file, a hash that does
// not match, a target function whose first bytes have changed -- every one of
// them is a refusal, never a warning and never a best effort. The fallback is
// not degraded: `wakepredict` and `hookdraw` are version-independent, still
// work, and still carry the old residual. Losing the GPU-state upgrade on an
// Archicad update is a return to the previous behaviour; guessing through it is
// a crash.
//
// ⚠️ IT GATES THE GPU-STATE PATH ONLY, NOT THE PRESENT HOOK. `PresentHook` swaps
// three slots of IDXGISwapChain's vtable, and that table is a public COM ABI
// fixed by every program on the system -- it cannot move with an Archicad
// update, and `hookdiag`/`hookdraw` have been measured across builds on it
// already. Putting those behind a pin would break the portable fallback on every
// machine that has not pinned, which is the opposite of what the pin is for.
// What is version-dependent is reading ARCHICAD'S OWN buffers, and that is
// exactly the path this gates.
//
// WHAT IS RECORDED, AND WHY EACH PIECE.
//
//   * `Archicad.exe` -- its file version and its SHA-256. The version is what a
//     human reads in a bug report; the hash is what actually decides, because a
//     hotfix can ship the same version string with different code.
//   * Every system module whose vtable a hook patches (`dxgi.dll`, `d3d11.dll`).
//     Not Archicad's, but a Windows update moves these too, and a vtable index
//     that was right for one d3d11.dll writing into an unrelated slot of another
//     is a crash inside the driver with no clue as to why.
//   * Each patched slot's TARGET: which module it lives in, its RVA within that
//     module, and the first bytes of the function. ⚠️ THE RVA AND THE PROLOGUE
//     ARE THE ONLY CHECK THAT CATCHES A WRONG INDEX. A hash of d3d11.dll says
//     the file is the one we pinned; it says nothing about whether slot 44 is
//     still `RSSetViewports`. Recording what was at each slot when the profile
//     was pinned, and refusing when it differs, is what makes "verify the
//     expected bytes at each target" mean something.
//
// ⚠️ THE PROFILE IS PINNED BY THE USER, ON PURPOSE, AND NEVER INFERRED. There is
// no compiled-in table of blessed hashes, because there could not be an honest
// one: this add-on is built on a machine that has no idea which Archicad build
// the user runs, and a table populated by guessing would bless a build nobody
// verified. Pinning is an explicit act -- `Tapioca.ViewerPatchProfile {pin: true}`
// -- performed on the build that was actually tested, and it writes a file the
// user can read, copy between machines and delete. Until they do, the GPU-state
// path refuses and says so.
//
// ⚠️ THE HASH IS COMPUTED LAZILY, NOT AT `Initialize`. `Archicad.exe` is a large
// file and hashing it is hundreds of milliseconds of disk and CPU; spending that
// on every Archicad startup, for a diagnostic path almost no session arms, would
// be a startup regression paid by everybody. It is computed on the first call
// that needs it and cached for the session.
//
// THREAD SAFETY: MAIN THREAD ONLY. It does file IO and it is called from mode
// arming and from bus commands, both of which are already there. Nothing here
// may be called from a detour.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace patchprofile {

struct ModuleIdentity {
    std::string name;    // "d3d11.dll", lower case
    std::string version; // file version, or "?" when the module carries none
    std::string sha256;  // lower-case hex
    uint64_t sizeBytes = 0;
};

// One patched vtable slot, described by what was IN it rather than by its index.
struct TargetIdentity {
    std::string name;     // "ID3D11DeviceContext::RSSetViewports"
    std::string module;   // the module the slot points into
    uint64_t rva = 0;     // offset within that module
    std::string prologue; // first kPrologueBytes bytes, lower-case hex
};

struct Identity {
    bool valid = false;
    std::string why;      // when !valid
    std::string hostPath; // full path to Archicad.exe
    std::string hostVersion;
    std::string hostSha256;
    uint64_t hostSizeBytes = 0;
    std::vector<ModuleIdentity> modules;
    std::vector<TargetIdentity> targets;
};

// How much of each target function is fingerprinted. Enough to catch a different
// function or a recompiled one; short enough that a hot-patch trampoline
// installed by an unrelated product is the only false positive -- and that one
// SHOULD refuse, because somebody else detouring the same function inline is
// exactly the situation where their byte patch and our vtable swap interact.
constexpr size_t kPrologueBytes = 16;

// This session's identity, computed once on first call. `valid` false with `why`
// filled if the host could not be read at all.
//
// ⚠️ TARGETS ARE NOT IN IT UNTIL `RecordTarget` HAS BEEN CALLED FOR THEM. They
// come from vtable discovery, which lives in the hook layer; this file cannot
// produce them on its own and does not pretend to.
const Identity& Current ();

// Add a system module to the identity. Called before `Verify`, once per module a
// hook will patch. Idempotent by name.
void RecordModule (const wchar_t* moduleName);

// Fingerprint one patched slot. `fn` is what discovery read OUT of the vtable
// before anything was written to it.
//
// ⚠️ CALL THIS BEFORE PATCHING, NEVER AFTER. Afterwards the slot holds OUR
// detour, and the profile would record the add-on's own code as Archicad's --
// self-consistent, verifying forever, and checking nothing.
void RecordTarget (const char* name, const void* fn);

// Forget every recorded target. Discovery runs again on each install, and a
// stale target from a previous run would be verified against a slot nobody
// looked at this time.
void ClearTargets ();

// Is a profile pinned on this machine at all?
bool HasPin ();

// The pin file, as an absolute path, reported rather than spelled out by callers
// -- the same rule `ExperimentGuard` learned the hard way when the recovery
// instruction named a file the code had never written.
std::string PinFilePath ();

// Compare this session against the pin. False with `error` filled -- in a
// sentence fit for a log line, a HUD banner and a command failure -- on ANY
// difference, and on no pin at all.
bool Verify (std::string& error);

// ⚠️ WHY A VERIFY FAILED, BECAUSE "RE-PINNABLE" AND
// "DO NOT TOUCH" ARE NOT THE SAME ANSWER AND THE BOOLEAN COULD NOT TELL THEM
// APART. On 2026-09-19 a Microsoft Visual C++ redistributable repair moved
// `dxgi.dll` from 10.0.19041.7663 to .7725. The profile refused every arm for
// the rest of the day, and the overlay was dead with a message nobody was
// watching for -- while `d3d11.dll`, which hosts ALL TWENTY-SEVEN targets, had
// not changed by a single byte, and `dxgi.dll` hosts NONE of them.
//
// ⚠️ THE TARGETS ARE THE CONTRACT AND THE MODULE HASH IS
// A PROXY FOR THEM. Each target carries its module, its RVA and the first bytes
// of the function itself. If all twenty-seven still sit at the same offset with
// the same prologue, the vtable is intact -- that is not weaker evidence than a
// module hash, it is the evidence the module hash was standing in for. A proxy
// must not outrank the thing it stands for.
//
// So the order is inverted: targets first, then only those modules that
// actually host one. A module hosting no target is not consulted at all.
enum class Verdict {
    Ok,                       // pinned and running agree
    StaleModuleTargetsIntact, // a hosting module moved; every target still verifies
    Refuse                    // something that matters changed
};

// ⚠️ `StaleModuleTargetsIntact` IS A REPAIR THE CALLER MAY
// TAKE, NOT ONE THIS FILE TAKES. Rewriting the pin is a decision with a
// consequence -- the profile stops describing the build it was tested on -- and
// it belongs to whoever is about to install, in the open, with a log line, not
// buried in a predicate named `Verify`.
Verdict VerifyDetailed (std::string& error);

// Write this session's identity as the pin. The explicit, user-driven act
// described above; nothing calls it automatically.
bool Pin (std::string& error);

// One line naming the pinned build, for a diagnostic that has to answer "why is
// it not syncing" on a machine nobody can attach a debugger to. Empty when
// nothing is pinned.
std::string PinnedSummary ();

// The same, for what is actually running.
std::string CurrentSummary ();

} // namespace patchprofile
} // namespace archviz
} // namespace geomsrv

#endif
