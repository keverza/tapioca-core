#ifndef EVP_COMMANDLAUNCH_HPP
#define EVP_COMMANDLAUNCH_HPP

#include "UniString.hpp"
#include "Array.hpp"

#include <cstdint>
#include <functional>

namespace evp {

// What the PALETTE knows about a run: which command was selected, what the user put
// in its parameter controls, and — for a Zone C command — the port the subprocess
// calls back on. Everything else a run needs is derivable, and is derived on the
// other side of this header.
struct CommandLaunchRequest {
    GS::UniString path;   // the command.py the scanner found
    GS::UniString folder; // its folder name; the module name comes from it
    GS::UniString title;
    GS::UniString paramsJson;
    // Empty for Run; the name of one of the command's declared output actions
    // when the user pressed a button in the action bar instead — or one of its
    // @tapioca.menu entries when the user picked one from the right-click menu.
    GS::UniString action;
    // Empty for everything EXCEPT a right-click entry, where it is the region the
    // click landed in ("panel", "params", "param:<name>", "commands", "results").
    // The palette resolves it; the command reads it as `ctx.region`.
    GS::UniString menuRegion;
    GS::UniString requiresApi, requiresTapir;
    GS::Array<GS::UniString> requirements;
    bool watchArmed = false;
    bool external = false;
    unsigned short port = 0; // Zone C only, and only while running
    uint64_t generation = 0; // the run's cancel-token generation
};

// Compose the run and start its worker.
//
// This exists so the palette does not have to know how a command is turned into a
// run — the module-name convention, the folder the subprocess makes its cwd, where
// the interpreter and the evp package live, and that a previous run's transcript has
// to be dropped first. None of that changes when the UI does, and all of it used to
// sit in ControlPalette::RunSelected, whose size exception in
// tools/quality/check_cpp.py named this extraction as the one owed next.
//
// `finish` is the shell's, not ours: it is what puts the outcome back on the palette,
// so it necessarily runs on the main thread and is Posted through the gate by the
// caller's own lambda. Passed by value and captured by value all the way down — the
// frame that starts a run is long gone by the time it ends.
void LaunchCommand (const CommandLaunchRequest& request, std::function<void (uint64_t, const GS::UniString&)> finish);

} // namespace evp

#endif
