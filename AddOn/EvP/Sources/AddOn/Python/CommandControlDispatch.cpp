#include "CommandControlDispatch.hpp"

#include "CommandCatalog.hpp"
#include "CommandLaunch.hpp"
#include "DispatcherError.hpp"
#include "MainThreadGate.hpp"
#include "PythonHost.hpp"
#include "RunCancel.hpp"
#include "CommandRunState.hpp"
#include "NativeCommands/CameraSetStore.hpp"
#include "NativeCommands/SelectionSetStore.hpp"
#include "Preview/PreviewRuntimeState.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerState.hpp"

#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace {

struct RunStartOutcome {
    bool started = false;
    uint64_t generation = 0;
    GS::UniString title;
    GS::UniString error;
};
struct PendingRoleRestore {
    geomsrv::SelectionSetStore::State selections;
    geomsrv::CameraSetStore::State cameras;
    GS::UniString temporaryOwner;
    uint64_t selectionRevision = 0, cameraRevision = 0;
    uint64_t generation = 0;
};
// Main-thread only: one command run is allowed, so one temporary role owner.
std::optional<PendingRoleRestore> s_pendingRoles;

} // namespace

namespace evp {

bool DispatchCommandControlVerb (const GS::UniString& backend, const GS::UniString& name, const GS::UniString& command,
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

                // Bus/HUD launches have no palette selection change to declare the
                // command's roles. Same-owner launches retain captured values; a
                // temporary different owner gets the prior state back at finish.
                const auto previousSelections = geomsrv::SelectionSetStore::Get ().Capture ();
                const auto previousCameras = geomsrv::CameraSetStore::Get ().Capture ();
                const bool restoreRoles = previousSelections.owner != info.path || previousCameras.owner != info.path;
                geomsrv::SelectionSetStore::Get ().Configure (info.path, info.selectionSets,
                                                              info.exclusiveSelectionSets);
                geomsrv::CameraSetStore::Get ().Configure (info.path, info.cameraSets);

                const uint64_t generation = RunCancel::Get ().BeginRun (info.timeoutSeconds);
                if (!CommandRunState::Get ().Begin (generation, info.title)) {
                    if (restoreRoles) {
                        geomsrv::SelectionSetStore::Get ().Restore (previousSelections);
                        geomsrv::CameraSetStore::Get ().Restore (previousCameras);
                    }
                    RunCancel::Get ().EndRun (generation);
                    start->error = "A command-control run is already active.";
                    return;
                }

                CommandRunState::Get ().SetStatus ("Running " + info.title + "...");
                // The two empty strings are `action` and `menuRegion`: this verb
                // starts runs, never output actions — those are the control
                // palette's action bar and right-click menu, which act on a stored
                // result this verb does not select.
                // Armed when the palette's preview or the overlays' annotations will show it.
                const bool watchArmed = preview::PreviewRuntimeState::Get ().AcceptsWatchTraces () &&
                                        (info.previewKind == "plan2d" || info.previewKind == "3d");
                const CommandLaunchRequest request { info.path,        info.folder,        info.title,
                                                     requestedParams,  GS::UniString (),   GS::UniString (),
                                                     info.requiresApi, info.requiresTapir, info.requirements,
                                                     watchArmed,       external,           port,
                                                     generation };
                const GS::UniString commandPath = info.path;
                const uint64_t selectionRevision = geomsrv::SelectionSetStore::Get ().Revision ();
                const uint64_t cameraRevision = geomsrv::CameraSetStore::Get ().Revision ();
                if (restoreRoles)
                    s_pendingRoles = PendingRoleRestore { previousSelections, previousCameras, commandPath,
                                                          selectionRevision,  cameraRevision,  generation };
                else
                    s_pendingRoles.reset ();
                LaunchCommand (request, [previousSelections, previousCameras, restoreRoles, commandPath,
                                         selectionRevision,
                                         cameraRevision] (uint64_t finishedGeneration, const GS::UniString& status) {
                    if (!restoreRoles) {
                        CommandRunState::Get ().Finish (finishedGeneration, status);
                        return;
                    }
                    GS::UniString gateError;
                    const GS::UniString finishedStatus = status;
                    const bool posted = MainThreadGate::Get ().Post (
                        [previousSelections, previousCameras, commandPath, selectionRevision, cameraRevision,
                         finishedGeneration, finishedStatus] {
                            // Restore before Finish releases the run token. Any
                            // declaration/content edit or project clear advances
                            // the store revision and vetoes this old snapshot.
                            const auto run = CommandRunState::Get ().Snapshot ();
                            if (run.generation == finishedGeneration) {
                                auto& selections = geomsrv::SelectionSetStore::Get ();
                                if (selections.Owner () == commandPath && selections.Revision () == selectionRevision)
                                    selections.Restore (previousSelections);
                                auto& cameras = geomsrv::CameraSetStore::Get ();
                                if (cameras.Owner () == commandPath && cameras.Revision () == cameraRevision)
                                    cameras.Restore (previousCameras);
                            }
                            if (s_pendingRoles && s_pendingRoles->generation == finishedGeneration)
                                s_pendingRoles.reset ();
                            CommandRunState::Get ().Finish (finishedGeneration, finishedStatus);
                        },
                        gateError);
                    if (!posted)
                        CommandRunState::Get ().Finish (finishedGeneration, status);
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
        const CommandRunSnapshot snapshot = CommandRunState::Get ().Snapshot ();
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
            CommandRunState::Get ().SetStatus ("Stopping...");
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

void ResetCommandRolesForProject ()
{
    // If a temporary bus command was active, restore the palette owner's role
    // declarations first, then drop every old-document value. The completion's
    // owner/revision checks will refuse its obsolete snapshot afterward.
    if (s_pendingRoles) {
        const auto& selections = geomsrv::SelectionSetStore::Get ();
        const auto& cameras = geomsrv::CameraSetStore::Get ();
        // A → temporary B → palette C must keep C. Restore A only while B's
        // exact declaration/content incarnation is still current.
        const bool temporaryCurrent = selections.Owner () == s_pendingRoles->temporaryOwner &&
                                      selections.Revision () == s_pendingRoles->selectionRevision &&
                                      cameras.Owner () == s_pendingRoles->temporaryOwner &&
                                      cameras.Revision () == s_pendingRoles->cameraRevision;
        if (temporaryCurrent) {
            geomsrv::SelectionSetStore::Get ().Restore (s_pendingRoles->selections);
            geomsrv::CameraSetStore::Get ().Restore (s_pendingRoles->cameras);
        }
        s_pendingRoles.reset ();
    }
    geomsrv::SelectionSetStore::Get ().ResetValues ();
    geomsrv::CameraSetStore::Get ().ResetValues ();
}

} // namespace evp
