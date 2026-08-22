#ifndef EVP_EXTERNALRUNNER_HPP
#define EVP_EXTERNALRUNNER_HPP

// Zone C — `runtime="external"`: the command runs in a bundled python.exe
// subprocess instead of the embedded interpreter.
//
// Why it exists (from the plan's risk register): in-process Zone B means a
// segfaulting C-extension takes Archicad down, and a script deep inside one long
// native call cannot be interrupted. A subprocess can simply be killed. Commands
// that are ctypes/C-extension-heavy are meant to declare external up front.
//
// Transparency is the design requirement: the subprocess reaches the SAME
// dispatcher over loopback HTTP (`POST /evp/call`), so gate, undo scoping and
// transactions are identical and only `meta.zone` differs. The script cannot tell.
//
// ACAPI-free on purpose — pure Win32 + GS::UniString, like PathUtils.

#include "UniString.hpp"
#include "Array.hpp"

#include <cstdint>

namespace evp {

// E7 — reconcile the embedded runtime's managed packages with evp/_env.py.
//   op="ensure": install the given `requires` if missing (no-op when satisfied).
//   op="reset":  wipe managed site-packages (keeping pip) and reinstall baseline +
//                `requires` (pass the union of every command's requires).
// Runs the runtime's python.exe against _env.py as a standalone file (stdlib-only,
// so no PYTHONPATH; -s -E hermetic). `resultJson` is the JSON result on stdout,
// `progress` the pip log on stderr. BLOCKS (pip can take seconds) — call from a
// worker thread, never the main thread. Returns false + `error` on spawn failure or
// a non-zero exit (a broken/typo'd requirement); `resultJson` still holds the
// structured reason in that case.
// `requirements`: the pinned package specs (JSON side calls this "requires"; that is a
// C++20 keyword, hence the rename).
bool RunEnvManager (const GS::UniString& op, const GS::Array<GS::UniString>& requirements,
                    const GS::UniString& runtimeHome, const GS::UniString& packageDir, GS::UniString& resultJson,
                    GS::UniString& progress, GS::UniString& error);

// Runs <folder>\command.py in a subprocess and BLOCKS until it exits. Call from a
// worker thread (Zone B's thread is fine) — never the main thread, which must stay
// free to serve the /evp/call traffic this very subprocess is about to generate.
// Deadlocks itself otherwise.
//
// `port` is the add-on's running HTTP server; the server MUST be started first, as
// it is the subprocess's only way back in.
//
// Combined stdout+stderr lands in `output` whether the run succeeds or fails —
// per the standing rule that a failure must never be information-free.
// Returns false and fills `error` when the process cannot start or exits non-zero.
//
// E9 — CANCELLABLE. `runGeneration` is the token generation RunSelected got from
// RunCancel::BeginRun; the drain loop watches it and TerminateProcess'es the
// subprocess when the run is cancelled (Stop, panel close, or timeout_s), instead
// of the old WaitForSingleObject(..., INFINITE) that could wait forever on a
// wedged child. Killing it is safe: the subprocess reaches Archicad ONLY over the
// HTTP bus, so there is no half-applied main-thread state beyond whatever
// transaction already committed. Whatever the child had already written is still
// drained into `output` — a killed run must not lose its transcript.
// `cancelled` is set true when that happened, so the caller reports "cancelled"
// rather than a bogus failure.
bool RunCommandExternal (const GS::UniString& folder, const GS::UniString& paramsJson,
                         const GS::UniString& action,     // empty for an ordinary run
                         const GS::UniString& menuRegion, // set only from the right-click menu
                         unsigned short port,
                         const GS::UniString& runtimeHome, // holds python.exe
                         const GS::UniString& packageDir,  // holds evp\ + _evp_external_main.py
                         uint64_t runGeneration, GS::UniString& output, bool& cancelled, GS::UniString& error);

} // namespace evp

#endif
