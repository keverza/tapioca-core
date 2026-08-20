#include "Python/CommandRunner.hpp"

#include "AddOnVersion.hpp"
#include "Diagnostics/ApiError.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/ExternalRunner.hpp"
#include "Python/PathUtils.hpp"
#include "Python/PythonHost.hpp"
#include "Python/RunCancel.hpp"
#include "Python/VersionRequirements.hpp"

#include <thread>
#include <utility>

namespace evp {

void StartCommandRun (CommandRunSpec spec)
{
    std::thread ([spec = std::move (spec)] () {
        GS::UniString runError, externalOutput, envProgress;
        bool ok = false, cancelled = false;
        bool requirementsOk = true;
        if (!SatisfiesVersionRequirement (ApiVersion, spec.requiresApi)) {
            requirementsOk = false;
            runError = VersionRequirementFailure (EVP_PRODUCT_NAME, ApiVersion, spec.requiresApi);
        }
        else if (!spec.requiresTapir.IsEmpty ()) {
            const GS::UniString tapir = InstalledTapirVersion ();
            if (!SatisfiesVersionRequirement (tapir, spec.requiresTapir)) {
                requirementsOk = false;
                runError = VersionRequirementFailure ("Tapir", tapir, spec.requiresTapir);
            }
        }

        bool envOk = requirementsOk;
        if (envOk && !spec.requirements.IsEmpty ()) {
            GS::UniString envJson, envError;
            envOk = RunEnvManager ("ensure", spec.requirements, spec.runtimeHome, spec.packageDir, envJson, envProgress,
                                   envError);
            if (!envOk)
                runError = "dependency install failed - " + envError;
        }
        if (envOk && spec.external) {
            ok = RunCommandExternal (spec.folderDir, spec.paramsJson, spec.action, spec.port, spec.runtimeHome,
                                     spec.packageDir, spec.generation, externalOutput, cancelled, runError);
        }
        else if (envOk) {
            ok = PythonHost::Get ().RunCommand (spec.path, spec.module, spec.paramsJson, spec.action, cancelled,
                                                runError);
        }

        if (RunCancel::Get ().IsCancelled (spec.generation))
            cancelled = true;
        const GS::UniString transcript = spec.external ? externalOutput : TakeScriptTranscript ();
        const GS::UniString logPath (EvpDataDir () + GS::UniString ("\\logs\\commands.log"));
        AppendTextLine (logPath, "===== " EVP_PRODUCT_NAME " " ADDON_VERSION " ============================");
        AppendTextLine (logPath, "command: " + spec.title +
                                     "   zone: " + GS::UniString (spec.external ? "external" : "embedded") +
                                     "   params: " + spec.paramsJson);
        if (!envProgress.IsEmpty ()) {
            AppendTextLine (logPath, "--- environment (evp._env ensure) ---");
            GS::Array<GS::UniString> envLines;
            envProgress.Split ("\n", &envLines);
            for (const GS::UniString& line : envLines)
                AppendTextLine (logPath, line);
            AppendTextLine (logPath, "--- command output ---");
        }
        GS::Array<GS::UniString> lines;
        transcript.Split ("\n", &lines);
        for (const GS::UniString& line : lines)
            AppendTextLine (logPath, line);

        const GS::UniString reason (RunCancel::ReasonText (RunCancel::Get ().Reason ()));
        GS::UniString status;
        if (cancelled) {
            AppendTextLine (logPath, "CANCELLED: " + reason + ".");
            status = spec.title + ": cancelled (" + reason + ")";
        }
        else if (!ok) {
            AppendTextLine (logPath, "FAILED: " + runError + "\r\n" + FailureTrailBlock ());
            status = spec.title + ": FAILED - " + runError;
        }
        else {
            status = spec.title + ": done (see logs\\commands.log)";
        }
        if (spec.finish)
            spec.finish (spec.generation, status);
        else
            RunCancel::Get ().EndRun (spec.generation);
    }).detach ();
}

} // namespace evp
