#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Python/CommandLaunch.hpp"
#include "Python/CommandRunner.hpp"
#include "Python/PythonHost.hpp" // GetRuntimeHome / GetOwnDir / TakeScriptTranscript

namespace evp {

void LaunchCommand (const CommandLaunchRequest& request, std::function<void (uint64_t, const GS::UniString&)> finish)
{
    // The FOLDER, not the file: the subprocess sets its own cwd and sys.path there.
    const UIndex separator = request.path.FindLast ('\\');
    const GS::UniString folderDir = (separator == MaxUIndex) ? request.path : request.path.GetSubstring (0, separator);

    TakeScriptTranscript (); // drop anything left from a previous run

    // E7: the runner reconciles the command's declared `requires` FIRST. No-op (no
    // pip spawn) when already satisfied, so only commands that declare packages —
    // and only on a first-ever install — pay the cost. It happens on the worker, off
    // the main thread, since pip can take seconds. Both zones go through it: the
    // embedded interpreter and external python.exe import from the same
    // site-packages.
    //
    // E9 — the run has two independent ways to learn it was cancelled, and either is
    // enough. The runner reports it when run() unwound on evp.Cancelled; the token
    // reports it when the trip never reached Python at all (a killed subprocess, or a
    // command that ended on its own between the Stop press and the check). Trusting
    // only the runner would misreport the first as a failure; trusting only the token
    // would miss a Cancelled raised by something else, such as
    // ui.request_selection() timing out.
    //
    // Zone B's print() marshals through the gate into the transcript; Zone C's
    // arrives as the subprocess's captured stdout/stderr. Same log either way. A
    // cancel is a CLEAN outcome, not a failure: no "FAILED", no traceback in the
    // alert, and the log says which of Stop / panel close / timeout did it, so a run
    // that stopped by itself is not mistaken for one the user stopped.

    CommandRunSpec spec { request.path,
                          "evp_cmd_" + request.folder,
                          request.paramsJson,
                          request.title,
                          folderDir,
                          PythonHost::Get ().GetRuntimeHome (),
                          PythonHost::Get ().GetOwnDir () + GS::UniString ("\\PyPackage"),
                          request.action,
                          request.requiresApi,
                          request.requiresTapir,
                          request.requirements, // E7: installed before the command imports
                          request.external,
                          request.port,
                          request.generation };
    spec.finish = std::move (finish);
    StartCommandRun (std::move (spec));
}

} // namespace evp
