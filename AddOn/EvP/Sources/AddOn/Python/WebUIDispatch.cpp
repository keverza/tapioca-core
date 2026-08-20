#include "WebUIDispatch.hpp"

#include "CommandCatalog.hpp"
#include "CommandLaunch.hpp"
#include "DispatcherError.hpp"
#include "MainThreadGate.hpp"
#include "PythonHost.hpp"
#include "RunCancel.hpp"
#include "WebUIRunState.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerState.hpp"

#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

struct RunStartOutcome {
    bool started = false;
    uint64_t generation = 0;
    GS::UniString title;
    GS::UniString error;
};

} // namespace

namespace evp {

bool DispatchWebUIVerb (const GS::UniString& backend, const GS::UniString& name, const GS::UniString& command,
                        const GS::ObjectState& params, GS::ObjectState& data, GS::ObjectState& error, bool& ok)
{
    if (backend != "EvP")
        return false;

    if (name == "GetCommands") {
        const auto scan = std::make_shared<ScanOutcome> ();
        GS::UniString gateError;
        const bool gated = MainThreadGate::Get ().Invoke ([scan] () { *scan = ScanCommandFolders (); },
                                                          MainThreadGate::DefaultTimeoutMs, gateError);
        if (!gated) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else {
            data.Add ("commands", CommandInfoJsons (scan->commands));
            data.Add ("status", scan->status);
            data.Add ("broken", (GS::Int32) scan->broken);
            ok = true;
        }
        return true;
    }

    if (name == "RunCommand") {
        GS::UniString folder;
        GS::ObjectState commandParams;
        params.Get ("folder", folder);
        params.Get ("params", commandParams);

        GS::UniString commandParamsJson;
        if (folder.IsEmpty () || JSON::CreateFromObjectState (commandParams, commandParamsJson) != NoError) {
            error = MakeError ("BadParams", "RunCommand requires folder and an object-valued params field.", command);
            return true;
        }

        const auto start = std::make_shared<RunStartOutcome> ();
        const GS::UniString requestedFolder = folder;
        const GS::UniString requestedParams = commandParamsJson;
        GS::UniString gateError;
        const bool gated = MainThreadGate::Get ().Invoke (
            [start, requestedFolder, requestedParams] () {
                if (RunCancel::Get ().IsRunning ()) {
                    start->error = "A command is already running.";
                    return;
                }

                GS::UniString initializationError;
                if (!PythonHost::Get ().EnsureInitialized (initializationError)) {
                    start->error = "Python init failed: " + initializationError;
                    return;
                }

                const ScanOutcome scan = ScanCommandFolders ();
                const auto found = std::find_if (
                    scan.commands.begin (), scan.commands.end (),
                    [requestedFolder] (const CommandInfo& info) { return info.folder == requestedFolder; });
                if (found == scan.commands.end ()) {
                    start->error = "No scanned command matches folder '" + requestedFolder + "'.";
                    return;
                }

                const CommandInfo& info = *found;
                const bool external = info.runtime == "external";
                unsigned short port = 0;
                if (external) {
                    geomsrv::HttpServer& server = geomsrv::SharedHttpServer ();
                    if (!server.Start () || server.Port () <= 0) {
                        start->error = "The Tapioca server could not start for an external command.";
                        return;
                    }
                    port = (unsigned short) server.Port ();
                }

                const uint64_t generation = RunCancel::Get ().BeginRun (info.timeoutSeconds);
                if (!WebUIRunState::Get ().Begin (generation, info.title)) {
                    RunCancel::Get ().EndRun (generation);
                    start->error = "A WebUI command run is already active.";
                    return;
                }

                WebUIRunState::Get ().SetStatus ("Running " + info.title + "...");
                // The empty string is `action`: the WebUI starts RUNS, never output
                // actions — those are the control palette's action bar, which acts
                // on a stored result the WebUI has no equivalent of.
                const CommandLaunchRequest request {
                    info.path,        info.folder,        info.title,        requestedParams, GS::UniString (),
                    info.requiresApi, info.requiresTapir, info.requirements, external,        port,
                    generation
                };
                LaunchCommand (request, [] (uint64_t finishedGeneration, const GS::UniString& status) {
                    WebUIRunState::Get ().Finish (finishedGeneration, status);
                });

                start->started = true;
                start->generation = generation;
                start->title = info.title;
            },
            MainThreadGate::DefaultTimeoutMs, gateError);

        if (!gated) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else if (!start->started) {
            error = MakeError ("CommandFailed", start->error, command);
        }
        else {
            data.Add ("started", true);
            data.Add ("generation", (GS::Int64) start->generation);
            data.Add ("title", start->title);
            ok = true;
        }
        return true;
    }

    if (name == "GetRunState") {
        const WebUIRunSnapshot snapshot = WebUIRunState::Get ().Snapshot ();
        data.Add ("active", snapshot.active);
        data.Add ("generation", (GS::Int64) snapshot.generation);
        data.Add ("title", snapshot.title);
        data.Add ("status", snapshot.status);
        data.Add ("headers", snapshot.headers);
        data.Add ("rows", snapshot.rows);
        data.Add ("resultText", snapshot.resultText);
        ok = true;
        return true;
    }

    if (name == "CancelRun") {
        const bool requested = RunCancel::Get ().IsRunning ();
        if (requested) {
            RunCancel::Get ().Request (CancelReason::StopButton);
            WebUIRunState::Get ().SetStatus ("Stopping...");
        }
        data.Add ("requested", requested);
        ok = true;
        return true;
    }

    if (name == "GetServerState") {
        const geomsrv::ServerState& state = geomsrv::ServerState::Get ();
        data.Add ("running", state.serverRunning.load ());
        data.Add ("port", (GS::Int32) state.port.load ());
        ok = true;
        return true;
    }

    if (name == "StartServer" || name == "StopServer") {
        const bool startServer = name == "StartServer";
        const auto result = std::make_shared<std::pair<bool, int>> (false, 0);
        GS::UniString gateError;
        const bool gated = MainThreadGate::Get ().Invoke (
            [startServer, result] () {
                geomsrv::HttpServer& server = geomsrv::SharedHttpServer ();
                if (startServer)
                    server.Start ();
                else
                    server.Stop ();
                result->first = startServer ? server.IsRunning () : !server.IsRunning ();
                result->second = server.Port ();
            },
            MainThreadGate::DefaultTimeoutMs, gateError);
        if (!gated) {
            error = MakeError ("GateUnavailable", gateError, command);
        }
        else if (startServer) {
            if (!result->first) {
                error =
                    MakeError ("ServerStartFailed", "The Tapioca server could not bind its localhost port.", command);
            }
            else {
                data.Add ("started", true);
                data.Add ("port", (GS::Int32) result->second);
                ok = true;
            }
        }
        else {
            data.Add ("stopped", result->first);
            ok = true;
        }
        return true;
    }

    return false;
}

} // namespace evp
