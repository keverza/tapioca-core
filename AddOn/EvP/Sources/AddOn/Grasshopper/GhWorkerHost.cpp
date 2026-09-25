#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GhBridge.hpp"
#include "GhLog.hpp"
#include "GhWorkerHost.hpp"

#include "GhWorkerDescription.hpp"
#include "GhWorkerArchicadPort.hpp"

#include "GhWorkerLocate.hpp"
#include "GhWorkerPeerDetach.hpp"

#include "GhWorkflowController.hpp"
#include "HostState.hpp"

#include "Python/MainThreadGate.hpp"
#include "Python/PathUtils.hpp" // ReadEnv

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// PLAT-RHINO-INSIDE: supervise an owned worker or an unowned attached peer.
// Rhino stays out of Archicad.exe; do not reinstate in-process preflight checks.
// ============================================================================

namespace evp {
namespace grasshopper {

namespace {

// The state-free half of the host: finding the worker, reading the heartbeat
// deadline, and the two string conversions everything here needs. Named so the
// call sites below read exactly as they did before the split.
using locate::FromUtf8Std;
using locate::FromWide;
using locate::HeartbeatDeadlineMs;
using locate::ResolveWorker;

// How long the supervisor waits between checks. One second: this is a liveness
// poll, not a latency path.
constexpr DWORD SupervisorIntervalMs = 1000;

// A worker that exits this fast did not fail at Grasshopper; it failed at
// starting at all, and the exit code is the only diagnostic there will be.
constexpr DWORD StartupExitWindowMs = 250;

// How long a worker gets to close Rhino on its own before it is killed. Sized
// for RhinoCore::Dispose on a cold machine; see the note at the call site.
constexpr DWORD CooperativeShutdownMs = 15000;

HostLifecycle lifecycle;

// The one workflow controller. Beside the lifecycle rather than inside it: the
// lifecycle owns whether a WORKER is up, and this owns what one SESSION on that
// worker is doing. Conflating them is how a restart ends up silently reusing a
// session id.
GhWorkflowController workflow;

std::mutex controlMutex;
HANDLE workerProcess = nullptr;
HANDLE workerJob = nullptr;
DWORD workerProcessId = 0;
std::thread supervisor;
std::atomic<bool> supervisorStopping { false };
std::atomic<bool> showEditorOnConnect { false };
std::atomic<bool> activeGh2 { false };
std::atomic<uint32_t> disconnectedGeneration { 0 };

GS::UniString lastMessage;
GS::UniString workerPath;
uint32_t archicadPort = 0;

void Log (const GS::UniString& line)
{
    LogLine (lifecycle.Generation (), workerProcessId, line);
}

// Shows a report to the user from ANY thread. The supervisor runs on its own and
// ACAPI is main-thread only, so a worker that dies at three in the morning of a
// long solve still gets a dialog rather than a silent disappearance.
void ReportToUser (const GS::UniString& report)
{
    if (MainThreadGate::Get ().IsMainThread ()) {
        ACAPI_WriteReport ("%T", true, report.ToPrintf ());
        return;
    }

    // Post, not Invoke: this is a notification, and Invoke on a job that opens a
    // dialog holds the gate for human time and then reports a bogus timeout.
    // Captured BY VALUE — the gate may run this after this frame is gone.
    GS::UniString error;
    MainThreadGate::Get ().Post ([report] () { ACAPI_WriteReport ("%T", true, report.ToPrintf ()); }, error);
}

// ⚠️ CALLED WITH controlMutex HELD, AND NEVER FROM Stop's OWN JOIN PATH. It
// closes the bridge and the process handles; it does not touch `supervisor`,
// because the supervisor thread itself calls this and a thread cannot join
// itself.
void TearDownLocked (bool killWorker, const GS::UniString& reason, bool failed = false)
{
    if (killWorker && workerProcess != nullptr) {
        // ⚠️ THE GUARANTEE, AND THE WHOLE REASON THE WORKER IS A SEPARATE
        // PROCESS. RequestAbortSolution only takes effect BETWEEN objects, so it
        // cannot recover a component stuck in native code, in a blocking socket
        // or in a loop. In process there was no recovery primitive at all. Here
        // there is exactly one and it is unconditional.
        TerminateProcess (workerProcess, 1);
        WaitForSingleObject (workerProcess, 2000);
    }

    GhBridge::Get ().Stop ();

    if (workerProcess != nullptr) {
        CloseHandle (workerProcess);
        workerProcess = nullptr;
    }
    if (workerJob != nullptr) {
        CloseHandle (workerJob);
        workerJob = nullptr;
    }
    workerProcessId = 0;
    showEditorOnConnect.store (false);
    activeGh2.store (false);
    workerPath.Clear ();
    archicadPort = 0;

    if (failed) {
        const std::string failure =
            reason.IsEmpty () ? std::string ("Grasshopper worker failed") : std::string (reason.ToCStr ().Get ());
        lifecycle.Fail (lifecycle.Generation (), failure);
    }
    else {
        lifecycle.BeginStop ();
        lifecycle.CompleteStop ();
    }
    if (!reason.IsEmpty ()) {
        lastMessage = reason;
        Log (reason);
    }
}

void SupervisorLoop (uint32_t generation)
{
    const uint64_t deadline = HeartbeatDeadlineMs ();

    while (!supervisorStopping.load ()) {
        if (generation != lifecycle.Generation ())
            return;
        HANDLE process = nullptr;
        {
            std::lock_guard<std::mutex> lock (controlMutex);
            process = workerProcess;
        }

        // An attached peer has no process handle; its pipe is supervised below.
        if (process == nullptr && !lifecycle.OwnsPeerProcess ()) {
            Sleep (SupervisorIntervalMs);
        }
        else if (process == nullptr) {
            return;
        }
        else if (WaitForSingleObject (process, SupervisorIntervalMs) == WAIT_OBJECT_0) {
            if (supervisorStopping.load ())
                return;

            DWORD exitCode = 0;
            GetExitCodeProcess (process, &exitCode);
            GS::UniString report;
            {
                std::lock_guard<std::mutex> lock (controlMutex);
                if (supervisorStopping.load () || generation != lifecycle.Generation ())
                    return;
                report = GS::UniString::Printf (
                    "The Grasshopper worker process exited (code %u). Archicad is unaffected; open "
                    "Tapioca > Grasshopper Editor again to start a new one.",
                    (unsigned int) exitCode);
                TearDownLocked (false, report, true);
            }
            // ⚠️ A WORKER CRASH IS A RECOVERABLE EVENT WITH A UI, NOT A CRASH
            // REPORT. HANDOFF §"Supervision is the point".
            ReportToUser (report);
            return;
        }

        if (supervisorStopping.load ())
            return;

        const uint32_t lostGeneration = disconnectedGeneration.exchange (0);
        if (lostGeneration == generation) {
            GS::UniString report;
            {
                std::lock_guard<std::mutex> lock (controlMutex);
                if (supervisorStopping.load () || generation != lifecycle.Generation ())
                    return;
                report = activeGh2.load ()
                             ? GS::UniString ("The Rhino 9/GH2 peer disconnected. Archicad is unaffected; "
                                              "reconnect from Rhino or start a new worker.")
                             : GS::UniString ("The Grasshopper worker disconnected from its bridge and "
                                              "was stopped. Archicad is unaffected; start it again.");
                TearDownLocked (true, report, true);
            }
            ReportToUser (report);
            return;
        }

        GhBridge& bridge = GhBridge::Get ();
        if (!bridge.IsConnected ())
            continue; // still starting; the bridge owns the connect deadline

        const uint64_t silence = bridge.MillisecondsSinceHeartbeat ();
        if (silence <= deadline)
            continue;

        GS::UniString report;
        {
            std::lock_guard<std::mutex> lock (controlMutex);
            if (supervisorStopping.load () || generation != lifecycle.Generation ())
                return;
            report = lifecycle.Ownership () == PeerOwnership::Attached
                         ? GS::UniString::Printf ("The attached Grasshopper peer stopped answering (no heartbeat "
                                                  "for %u ms). Its bridge was closed; Rhino remains yours.",
                                                  (unsigned int) silence)
                         : GS::UniString::Printf ("The Grasshopper worker stopped answering (no heartbeat for %u "
                                                  "ms) and was stopped. Archicad and your project are unaffected.",
                                                  (unsigned int) silence);
            TearDownLocked (true, report, true);
        }
        ReportToUser (report);
        return;
    }
}

// ⚠️ CALLED WITH controlMutex HELD.
bool StartWorkerLocked (GS::UniString& message, bool gh2)
{
    disconnectedGeneration.store (0);

    std::wstring executable;
    std::wstring workingDirectory;
    if (!ResolveWorker (executable, workingDirectory, gh2)) {
        message = gh2 ? "Tapioca.Gh2Worker.exe was not found. Build the Rhino 9 worker or set "
                        "TAPIOCA_GH2_WORKER_DIR to its directory."
                      : "Tapioca.GhWorker.exe was not found beside the add-on. Rebuild the add-on with the .NET SDK "
                        "installed, or set TAPIOCA_GH_WORKER_DIR to the folder that holds the worker.";
        return false;
    }
    workerPath = FromWide (executable);

    std::wstring pluginPath;
    if (gh2) {
        pluginPath = workingDirectory + L"\\GrasshopperLibraries\\TapiocaGH2.rhp";
        if (GetFileAttributesW (pluginPath.c_str ()) == INVALID_FILE_ATTRIBUTES) {
            message = "TapiocaGH2.rhp was not found beside the GH2 worker. Rebuild and stage both artifacts.";
            return false;
        }
    }

    // The bridge FIRST. The worker connects to a name it is given on its command
    // line, and a name that is not listening yet is a startup race with no
    // upside.
    GhBridge& bridge = GhBridge::Get ();
    GS::UniString bridgeError;
    if (!bridge.Start (lifecycle.Generation (), bridgeError, gh2)) {
        message = bridgeError;
        return false;
    }

    archicadPort = 0;
    if (!gh2) {
        GS::UniString failure;
        archicadPort = ReadArchicadJsonPort (failure);
        if (!failure.IsEmpty ())
            Log (failure);
    }

    const std::wstring pipeName ((const wchar_t*) bridge.PipeName ().ToUStr ().Get ());
    std::wstring commandLine = L"\"" + executable + L"\" --pipe " + pipeName + L" --protocol " +
                               std::to_wstring (protocol::Version) + L" --generation " +
                               std::to_wstring (lifecycle.Generation ());
    if (gh2)
        commandLine += L" --plugin \"" + pluginPath + L"\"";
    else
        commandLine += L" --archicad-port " + std::to_wstring (archicadPort) + L" --mode authoring";

    GS::UniString bootLog = LogPath ();
    if (!bootLog.IsEmpty ()) {
        // A SEPARATE file, and deliberately not grasshopper.log: everything the
        // worker has to say after the handshake travels over the bridge and is
        // written by the host, so grasshopper.log has exactly one writer. This
        // covers the case that writer cannot cover — a worker that dies before
        // it ever connects.
        bootLog = bootLog + GS::UniString (".worker-boot.log");
        commandLine += L" --boot-log \"" + std::wstring ((const wchar_t*) bootLog.ToUStr ().Get ()) + L"\"";
    }

    std::vector<wchar_t> mutableCommandLine (commandLine.begin (), commandLine.end ());
    mutableCommandLine.push_back (L'\0');

    HANDLE job = CreateJobObjectW (nullptr, nullptr);
    if (job == nullptr) {
        const DWORD win32Error = GetLastError ();
        bridge.Stop ();
        message = GS::UniString::Printf ("Could not create the Grasshopper worker Job Object (Win32 error %u).",
                                         (unsigned int) win32Error);
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits)) == 0) {
        const DWORD win32Error = GetLastError ();
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf ("Could not configure the Grasshopper worker Job Object (Win32 error %u).",
                                         (unsigned int) win32Error);
        return false;
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION process {};
    activeGh2.store (gh2);
    const BOOL created = CreateProcessW ((LPCWSTR) executable.c_str (), mutableCommandLine.data (), nullptr, nullptr,
                                         FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                         (LPCWSTR) workingDirectory.c_str (), &startup, &process);
    if (created == 0) {
        activeGh2.store (false);
        const DWORD win32Error = GetLastError ();
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf (
            gh2 ? "Could not start Tapioca.Gh2Worker.exe (Win32 error %u). Verify the .NET 9 Runtime is installed."
                : "Could not start Tapioca.GhWorker.exe (Win32 error %u). Verify the .NET 8 Windows Desktop "
                  "Runtime is installed.",
            (unsigned int) win32Error);
        return false;
    }
    if (AssignProcessToJobObject (job, process.hProcess) == 0) {
        activeGh2.store (false);
        const DWORD win32Error = GetLastError ();
        TerminateProcess (process.hProcess, 1);
        WaitForSingleObject (process.hProcess, 2000);
        CloseHandle (process.hThread);
        CloseHandle (process.hProcess);
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf (
            "Could not place the Grasshopper worker in its Job Object (Win32 error %u). The worker was stopped.",
            (unsigned int) win32Error);
        return false;
    }
    if (ResumeThread (process.hThread) == DWORD (-1)) {
        activeGh2.store (false);
        const DWORD win32Error = GetLastError ();
        TerminateProcess (process.hProcess, 1);
        WaitForSingleObject (process.hProcess, 2000);
        CloseHandle (process.hThread);
        CloseHandle (process.hProcess);
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf ("Could not resume the Grasshopper worker (Win32 error %u).",
                                         (unsigned int) win32Error);
        return false;
    }
    CloseHandle (process.hThread);

    // A worker that exits this fast never reached Rhino, so its exit code is the
    // only diagnostic there will be. Waiting a quarter of a second for it is
    // worth far more than the quarter of a second costs.
    if (WaitForSingleObject (process.hProcess, StartupExitWindowMs) == WAIT_OBJECT_0) {
        activeGh2.store (false);
        DWORD exitCode = 0;
        GetExitCodeProcess (process.hProcess, &exitCode);
        CloseHandle (process.hProcess);
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf (
            gh2 ? "Tapioca.Gh2Worker.exe exited during startup (code %u). Verify .NET 9 and check %T."
                : "Tapioca.GhWorker.exe exited during startup (code %u). Verify the .NET 8 Windows Desktop Runtime "
                  "and check %T for what it managed to say first.",
            (unsigned int) exitCode, bootLog.ToPrintf ());
        return false;
    }

    workerProcess = process.hProcess;
    workerJob = job;
    workerProcessId = process.dwProcessId;

    // A supervisor from a previous generation has returned by now (its worker
    // died, or Stop joined it), but the std::thread object may still be
    // joinable — and assigning over a joinable thread calls std::terminate.
    if (supervisor.joinable ())
        supervisor.join ();

    supervisorStopping.store (false);
    try {
        supervisor = std::thread (SupervisorLoop, lifecycle.Generation ());
    }
    catch (...) {
        // A worker nothing supervises is precisely the thing this design is for
        // avoiding, so it is refused rather than left running.
        TearDownLocked (true, GS::UniString ());
        message = "Could not create the Grasshopper worker supervisor thread; the worker was stopped.";
        return false;
    }

    message =
        GS::UniString::Printf ("Grasshopper worker %u starting (generation %u) on %T.", (unsigned int) workerProcessId,
                               (unsigned int) lifecycle.Generation (), bridge.PipeName ().ToPrintf ());
    return true;
}

// ⚠️ CALLED WITH controlMutex HELD.
// An Attach that is still waiting: the bridge is open, no peer has said hello,
// and the lifecycle is therefore Starting.
//
// A worker start takes over a bridge still waiting for a peer, never a connected peer.
bool WaitingForAbsentPeer ()
{
    return lifecycle.State () == HostState::Starting && lifecycle.Ownership () == PeerOwnership::Attached &&
           !GhBridge::Get ().IsConnected ();
}

// Closes a bridge nobody came to, so an ordinary start can proceed. A no-op in
// every other state, and logged when it does something: a user who pressed
// Attach is owed the news that Tapioca started its own Rhino instead.
void TakeOverFromAbsentPeer ()
{
    if (!WaitingForAbsentPeer ())
        return;

    Log (GS::UniString ("No Grasshopper peer connected to the open bridge, and Grasshopper was asked for: closing "
                        "the bridge and starting Tapioca's own worker instead."));
    GhWorkerHost::Get ().Stop ();
}

bool EnsureRunningLocked (GS::UniString& message, bool gh2 = false)
{
    if ((lifecycle.IsRunning () || lifecycle.State () == HostState::Starting) && activeGh2.load () != gh2) {
        message = "A different Grasshopper engine owns the current worker. Stop it before switching.";
        return false;
    }
    switch (lifecycle.BeginStart ()) {
        case StartDecision::AlreadyRunning:
            message = "The Grasshopper worker is already running. " + lastMessage;
            return true;
        case StartDecision::InProgress:
            message = "The Grasshopper worker is already starting.";
            return false;
        case StartDecision::Proceed:
            break;
    }

    Log (GS::UniString ("===== Grasshopper worker start ====="));
    if (!StartWorkerLocked (message, gh2)) {
        lifecycle.Fail (lifecycle.Generation (), std::string ("worker start failed"));
        lastMessage = message;
        Log (message);
        return false;
    }

    lastMessage = message;
    Log (message);
    return true;
}

// The report from a Run, arriving on the bridge's IO thread some time after the
// menu command that asked for it returned.
void OnRunResult (const protocol::RunReportPayload& report)
{
    ReportToUser (GS::UniString ("Grasshopper run\n\n") + DescribeRunResult (report));
}

void OnWorkerDisconnected (uint32_t generation)
{
    if (generation != lifecycle.Generation ())
        return;
    disconnectedGeneration.store (generation);
    // Told immediately rather than when the supervisor gets round to the
    // teardown: the controller's job on this event is to stop anything from
    // being applied out of a session whose worker is gone, and that has to be
    // true from the moment the pipe drops.
    if (!activeGh2.load ())
        workflow.OnHostGone ("The Grasshopper worker disconnected from its bridge.");
}

// Malformed GH1 session envelopes are logged, never partially published.
void OnSessionMessage (protocol::MessageType type, const std::vector<uint8_t>& payload)
{
    if (activeGh2.load ())
        return; // GH2 has no GH1 session protocol in this slice.
    const uint32_t gen = lifecycle.Generation ();
    const uint32_t pid = GhBridge::Get ().WorkerProcessId ();
    std::string error;

    switch (type) {
        case protocol::MessageType::SessionEvent: {
            protocol::SessionAckPayload event;
            if (!protocol::DecodeSessionAckPayload (payload.data (), payload.size (), event, error))
                break;
            workflow.OnSessionEvent (event);
            return;
        }
        case protocol::MessageType::SchemaResult: {
            protocol::SchemaResultPayload schema;
            if (!protocol::DecodeSchemaResultPayload (payload.data (), payload.size (), schema, error))
                break;
            workflow.OnSchemaResult (schema);
            return;
        }
        case protocol::MessageType::SolutionStarted: {
            protocol::SolutionStartedPayload started;
            if (!protocol::DecodeSolutionStartedPayload (payload.data (), payload.size (), started, error))
                break;
            workflow.OnSolutionStarted (started);
            return;
        }
        case protocol::MessageType::SolutionResult: {
            protocol::SolutionResultPayload result;
            if (!protocol::DecodeSolutionResultPayload (payload.data (), payload.size (), result, error))
                break;
            workflow.OnSolutionResult (result);
            return;
        }
        case protocol::MessageType::SolutionFailed: {
            protocol::SolutionFailedPayload failed;
            if (!protocol::DecodeSolutionFailedPayload (payload.data (), payload.size (), failed, error))
                break;
            workflow.OnSolutionFailed (failed);
            return;
        }
        case protocol::MessageType::DiagnosticsResult: {
            protocol::DiagnosticsResultPayload diagnostics;
            if (!protocol::DecodeDiagnosticsResultPayload (payload.data (), payload.size (), diagnostics, error))
                break;
            LogWorkerLine (gen, pid, GS::UniString ("diagnostics: ") + FromUtf8Std (diagnostics.report));
            return;
        }
        default:
            return;
    }

    LogLine (gen, pid,
             GS::UniString ("bridge could not read a \"") + GS::UniString (protocol::DescribeMessageType (type)) +
                 GS::UniString ("\" message: ") + FromUtf8Std (error));
}

void OnWorkerStarted (uint32_t generation, protocol::AckStatus status, const GS::UniString& message)
{
    if (status == protocol::AckStatus::Ok) {
        if (!lifecycle.CompleteStart (generation))
            return;

        // ⚠️ THE GENERATION REACHES THE CONTROLLER ONLY AFTER THE RUNTIME
        // ACKNOWLEDGEMENT, NOT AT CONNECT. A connected pipe proves the protocol;
        // it does not prove there is a Rhino behind it, and a session opened
        // against a worker whose RhinoCore never came up would be a session
        // every request refuses.
        if (!activeGh2.load ())
            workflow.OnHostGeneration (generation);
        if (showEditorOnConnect.exchange (false)) {
            GS::UniString error;
            if (!GhBridge::Get ().Send (protocol::MessageType::ShowEditor, error))
                LogLine (generation, GhBridge::Get ().WorkerProcessId (), error);
        }
        return;
    }

    lifecycle.Fail (generation, std::string (message.ToCStr ().Get ()));
}

} // namespace

// Every start path installs handlers before the bridge accepts a peer.
void WireBridgeLocked ()
{
    GhBridge& bridge = GhBridge::Get ();
    bridge.SetStartupHandler (&OnWorkerStarted);
    bridge.SetDisconnectedHandler (&OnWorkerDisconnected);
    bridge.SetRunResultHandler (&OnRunResult);
    bridge.SetSessionHandler (&OnSessionMessage);
    workflow.SetSender ([] (protocol::MessageType type, const std::vector<uint8_t>& payload, std::string& error) {
        GS::UniString failure;
        if (GhBridge::Get ().SendPayload (type, payload, failure))
            return true;
        error = failure.ToCStr ().Get ();
        return false;
    });
}

GhWorkerHost& GhWorkerHost::Get ()
{
    static GhWorkerHost instance;
    return instance;
}

bool GhWorkerHost::OpenEditor (GS::UniString& message)
{
    if (!MainThreadGate::Get ().IsMainThread ()) {
        message = "The Grasshopper worker can only be started from Archicad's main thread.";
        return false;
    }

    WireBridgeLocked ();
    if (activeGh2.load ()) {
        message = "Stop the Rhino 9/GH2 worker before opening the GH1 editor.";
        return false;
    }

    // A bridge nobody came to does not block a request for the canvas either.
    // See TakeOverFromAbsentPeer; before the lock, for the reason Stop ()
    // documents.
    TakeOverFromAbsentPeer ();

    GhBridge& bridge = GhBridge::Get ();
    std::lock_guard<std::mutex> lock (controlMutex);

    // ⚠️ THE FLAG IS SET BEFORE THE CONNECTED CHECK, NOT AFTER. The handshake
    // runs on the bridge's IO thread and can complete at any instant; setting
    // the flag afterwards leaves a window in which the handler has already fired
    // with nothing to do and the canvas is never asked for. Set first, and
    // whichever of the two paths gets there first clears it.
    showEditorOnConnect.store (true);

    if (bridge.IsConnected () && lifecycle.AcceptsMessages ()) {
        if (!showEditorOnConnect.exchange (false))
            return true; // the handler beat us to it
        if (!bridge.Send (protocol::MessageType::ShowEditor, message))
            return false;
        message = "Asked the Grasshopper worker for its canvas.";
        lastMessage = message;
        return true;
    }

    if (lifecycle.State () == HostState::Starting) {
        message = "The Grasshopper worker is starting and will show its canvas when it is ready.";
        return true;
    }

    if (!EnsureRunningLocked (message)) {
        showEditorOnConnect.store (false);
        return false;
    }
    return true;
}

bool GhWorkerHost::EnsureHeadless (GS::UniString& message)
{
    if (!MainThreadGate::Get ().IsMainThread ()) {
        message = "The Grasshopper worker can only be started from Archicad's main thread.";
        return false;
    }

    WireBridgeLocked ();
    if (activeGh2.load ()) {
        message = "Stop the Rhino 9/GH2 worker before opening a GH1 definition.";
        return false;
    }

    // ⚠️ BEFORE THE LOCK, BECAUSE Stop () JOINS THE SUPERVISOR AND THE
    // SUPERVISOR TAKES controlMutex. Same ordering hazard Stop () documents.
    TakeOverFromAbsentPeer ();

    GhBridge& bridge = GhBridge::Get ();
    std::lock_guard<std::mutex> lock (controlMutex);

    if (bridge.IsConnected () && lifecycle.AcceptsMessages ()) {
        message = "The Grasshopper worker is running.";
        return true;
    }

    if (lifecycle.State () == HostState::Starting) {
        message = "The Grasshopper worker is starting.";
        return true;
    }

    return EnsureRunningLocked (message);
}

bool GhWorkerHost::EnsureHeadlessGh2 (GS::UniString& message)
{
    if (!MainThreadGate::Get ().IsMainThread ()) {
        message = "The GH2 worker can only be started from Archicad's main thread.";
        return false;
    }

    WireBridgeLocked ();
    TakeOverFromAbsentPeer ();
    if (!activeGh2.load () && (lifecycle.IsRunning () || lifecycle.State () == HostState::Starting)) {
        message = "Stop the Rhino 8/GH1 worker before opening a GH2 definition.";
        return false;
    }

    std::lock_guard<std::mutex> lock (controlMutex);
    if (GhBridge::Get ().IsConnected () && lifecycle.AcceptsMessages ()) {
        message = "The Rhino 9 / GH2 worker is running.";
        return true;
    }
    if (lifecycle.State () == HostState::Starting) {
        message = "The Rhino 9 / GH2 worker is starting.";
        return true;
    }
    return EnsureRunningLocked (message, true);
}

// Called on the main thread; neither variant creates a process or Job Object.
static bool AttachLocalEngine (GS::UniString& message, bool gh2)
{
    if (!MainThreadGate::Get ().IsMainThread ()) {
        message = "The Grasshopper bridge can only be opened from Archicad's main thread.";
        return false;
    }

    // A supervisor from the preceding failed/stopped generation may still be
    // returning. Join before controlMutex: its teardown takes that same lock.
    const HostState before = lifecycle.State ();
    if ((before == HostState::Failed || before == HostState::Stopped) && supervisor.joinable ())
        supervisor.join ();
    WireBridgeLocked ();

    GhBridge& bridge = GhBridge::Get ();
    std::lock_guard<std::mutex> lock (controlMutex);

    if ((lifecycle.IsRunning () || lifecycle.State () == HostState::Starting) && activeGh2.load () != gh2) {
        message = "A different Grasshopper engine owns the bridge. Detach or Stop before switching.";
        return false;
    }
    if (bridge.IsConnected () && lifecycle.AcceptsMessages ()) {
        message = GS::UniString ("A Grasshopper bridge is already connected (") +
                  GS::UniString (DescribePeerOwnership (lifecycle.Ownership ())) + ").";
        return true;
    }

    switch (lifecycle.BeginStart (PeerOwnership::Attached)) {
        case StartDecision::AlreadyRunning:
            message = "A Grasshopper peer is already connected.";
            return true;
        case StartDecision::InProgress:
            message = "The Grasshopper bridge is already opening.";
            return false;
        case StartDecision::Proceed:
            break;
    }

    Log (gh2 ? GS::UniString ("===== GH2 bridge open for an attached peer =====")
             : GS::UniString ("===== Grasshopper bridge open for an attached peer ====="));

    activeGh2.store (gh2); // visible to a peer that handshakes on the new IO thread
    GS::UniString bridgeError;
    if (!bridge.Start (lifecycle.Generation (), bridgeError, gh2)) {
        activeGh2.store (false);
        lifecycle.Fail (lifecycle.Generation (), std::string ("bridge start failed"));
        message = bridgeError;
        lastMessage = message;
        Log (message);
        return false;
    }
    workerPath.Clear ();
    archicadPort = 0;

    // Even an attached peer needs disconnect and heartbeat supervision.
    supervisorStopping.store (false);
    try {
        supervisor = std::thread (SupervisorLoop, lifecycle.Generation ());
    }
    catch (...) {
        TearDownLocked (false, GS::UniString ());
        message = "Could not create the Grasshopper supervisor thread; the bridge was closed.";
        return false;
    }

    message = GS::UniString (gh2 ? "Waiting for a Rhino 9/GH2 peer on \\\\.\\pipe\\"
                                 : "Waiting for a Grasshopper peer on \\\\.\\pipe\\") +
              bridge.PipeName () +
              GS::UniString (gh2 ? ". In standalone Rhino 9 run TapiocaGh2Attach."
                                 : ". Connect from a Rhino that is already running.");
    lastMessage = message;
    Log (message);
    return true;
}

bool GhWorkerHost::AttachLocal (GS::UniString& message)
{
    return AttachLocalEngine (message, false);
}

bool GhWorkerHost::AttachLocalGh2 (GS::UniString& message)
{
    return AttachLocalEngine (message, true);
}

bool GhWorkerHost::HideEditor (GS::UniString& message)
{
    // Deliberately does NOT start anything: "no canvas on screen" is already
    // true when there is no worker, and spawning Rhino to satisfy a request to
    // see less of it would be absurd.
    std::lock_guard<std::mutex> lock (controlMutex);
    if (activeGh2.load ()) {
        message = "The headless GH2 worker has no editor to hide.";
        return true;
    }
    showEditorOnConnect.store (false);
    if (!lifecycle.AcceptsMessages () || !GhBridge::Get ().IsConnected ()) {
        message = "The Grasshopper worker is not running.";
        return true;
    }
    return GhBridge::Get ().Send (protocol::MessageType::HideEditor, message);
}

void GhWorkerHost::Stop ()
{
    // Join outside controlMutex: the supervisor takes that lock on teardown.
    const bool gh2Bootstrapping =
        activeGh2.load () && lifecycle.OwnsPeerProcess () && lifecycle.State () == HostState::Starting;
    const bool gh2PeerBootstrapping =
        activeGh2.load () && !lifecycle.OwnsPeerProcess () && lifecycle.State () == HostState::Starting;
    lifecycle.BeginStop ();
    // A peer can be waiting for Ping on the main-thread gate. Cancel before detaching.
    const bool gh2RequestInFlight =
        activeGh2.load () && GhBridge::Get ().BeginGh2Shutdown (gh2Bootstrapping || gh2PeerBootstrapping);
    supervisorStopping.store (true);
    if (supervisor.joinable ())
        supervisor.join ();

    std::lock_guard<std::mutex> lock (controlMutex);
    if (workerProcess == nullptr) {
        // Normally wait for peer release (GhWorkerPeerDetach.hpp); a GH2
        // bootstrap still needs this thread, so close its pipe instead.
        if (lifecycle.Ownership () == PeerOwnership::Attached && !gh2PeerBootstrapping && !gh2RequestInFlight) {
            GS::UniString note;
            RequestPeerDetach (note);
            Log (note);
        }

        TearDownLocked (false, GS::UniString ());
        return;
    }

    Log (GS::UniString ("===== Grasshopper worker stop ====="));

    if (gh2RequestInFlight || gh2Bootstrapping) {
        // A Ping can still be in the pipe when admission is sealed. Until the
        // startup Ack the worker may be waiting for that reply; shutdown sent
        // instead of a response cannot be relied on to release it promptly.
        Log ("GH2 bootstrap stopping: terminating the owned worker.");
        TerminateProcess (workerProcess, 1);
        TearDownLocked (false, "GH2 worker terminated during bootstrap stop.");
        return;
    }

    // Cooperative first: this releases Rhino's temporary files and licence
    // tidily. The hard-termination guarantee follows regardless.
    //
    // ⚠️ RhinoCore::Dispose took over three seconds on a real quit; the
    // 15-second wait is for disposal, not for the shutdown acknowledgement.
    GS::UniString sendError;
    if (GhBridge::Get ().Send (protocol::MessageType::Shutdown, sendError)) {
        WaitForSingleObject (workerProcess, CooperativeShutdownMs);
    }
    else {
        Log (sendError);
    }

    DWORD exitCode = 0;
    const bool exited = GetExitCodeProcess (workerProcess, &exitCode) != 0 && exitCode != STILL_ACTIVE;
    TearDownLocked (!exited, exited ? GS::UniString ("Grasshopper worker stopped.")
                                    : GS::UniString ("Grasshopper worker did not shut down and was terminated."));
}

GhWorkflowController& GhWorkerHost::Workflow ()
{
    return workflow;
}

bool GhWorkerHost::IsRunning () const
{
    return lifecycle.IsRunning ();
}

bool GhWorkerHost::IsGh2 () const
{
    return activeGh2.load ();
}

GS::UniString GhWorkerHost::LastWorkerMessage () const
{
    return GhBridge::Get ().LastWorkerMessage ();
}

bool GhWorkerHost::IsAttachedPeer () const
{
    return lifecycle.Ownership () == PeerOwnership::Attached;
}

HostState GhWorkerHost::State () const
{
    return lifecycle.State ();
}

GS::UniString GhWorkerHost::Describe () const
{
    return DescribeWorker (lifecycle.State (), lifecycle.Generation (), lifecycle.Ownership (), activeGh2.load (),
                           archicadPort, workerPath, lastMessage, lifecycle.LastError (), GhBridge::Get ());
}

bool GhWorkerHost::RunDefinition (GS::UniString& message)
{
    std::lock_guard<std::mutex> lock (controlMutex);
    if (activeGh2.load ()) {
        message = "The GH2 worker has no active editor canvas; open a GH1 worker to use Run Definition.";
        return false;
    }
    if (!lifecycle.AcceptsMessages () || !GhBridge::Get ().IsConnected ()) {
        // Deliberately does NOT spawn a worker. A Run solves the definition on
        // the canvas, and a worker that has just started has no canvas and no
        // definition -- so starting one here would answer a request to run
        // something with a ten-second wait and then "there is nothing open".
        message = "Grasshopper is not running. Open Tapioca > Grasshopper Editor and load a definition first.";
        return false;
    }

    if (!GhBridge::Get ().Send (protocol::MessageType::RunDefinition, message))
        return false;

    message = "Asked Grasshopper to solve the definition on its canvas. The result will be reported when it "
              "finishes.";
    lastMessage = message;
    Log (message);
    return true;
}

bool GhWorkerHost::CancelRun (GS::UniString& message)
{
    std::lock_guard<std::mutex> lock (controlMutex);
    if (activeGh2.load ()) {
        message = "GH2 Ping has no definition run to cancel.";
        return false;
    }
    if (!lifecycle.AcceptsMessages () || !GhBridge::Get ().IsConnected ()) {
        message = "Grasshopper is not running, so there is nothing to cancel.";
        return true;
    }
    return GhBridge::Get ().Send (protocol::MessageType::CancelRun, message);
}

void GhWorkerHost::RunFromMenu ()
{
    GhWorkerHost& host = Get ();
    GS::UniString message;
    if (host.RunDefinition (message))
        return; // the report arrives on its own; a dialog here would be in its way

    // Held in a named local: GS::UniString's operator+ yields a lazy
    // Concatenation, which has no ToPrintf and would not survive the call if it
    // did. The same owning-temporary trap as UStr, one operator along.
    const GS::UniString report = GS::UniString ("The Grasshopper definition did not run.\n\n") + message;
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

void GhWorkerHost::CloseFromMenu ()
{
    GhWorkerHost& host = Get ();
    const HostState state = host.State ();
    if ((state == HostState::NotStarted || state == HostState::Stopped || state == HostState::Failed) &&
        !GhBridge::Get ().IsConnected ()) {
        ACAPI_WriteReport ("%T", true, GS::UniString ("Grasshopper is not running.").ToPrintf ());
        return;
    }

    host.Stop ();
    const GS::UniString report = GS::UniString ("Grasshopper closed.\n\n") + host.Describe ();
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

void GhWorkerHost::OpenEditorFromMenu ()
{
    GhWorkerHost& host = Get ();
    GS::UniString message;
    if (host.OpenEditor (message)) {
        // No dialog on success. On a warm worker the canvas IS the feedback; on
        // a cold one the canvas arrives a few seconds later and a modal in front
        // of it would only be in the way.
        return;
    }

    const GS::UniString report = GS::UniString ("The Grasshopper editor did not open.\n\n") + message +
                                 GS::UniString ("\n\n") + host.Describe ();
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

} // namespace grasshopper
} // namespace evp
