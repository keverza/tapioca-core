#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GhBridge.hpp"
#include "GhLog.hpp"
#include "GhWorkerHost.hpp"

#include "GhWorkerLocate.hpp"

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
// PLAT-RHINO-INSIDE, P0/P0b: spawn -> handshake -> supervise -> kill, and back
// down again cleanly.
//
// The whole file is written so that EVERY failure has a name. A menu command
// that says "could not start Grasshopper" is worth nothing: the things that
// actually go wrong on a real machine are a worker that was never built, a
// missing .NET Desktop Runtime, a missing or unlicensed Rhino, a worker that
// exits during startup, a worker that never completes the handshake, and a
// worker that stops answering while a definition runs. They have six different
// fixes, so they get six different messages.
//
// ⚠️ WHAT CHANGED FROM THE IN-PROCESS HOST THIS REPLACES, AND WHY THE OLD
// PREFLIGHT CHECKS ARE GONE RATHER THAN MOVED. The opennurbs collision, the STA
// gate and the crash breadcrumb all existed to make it safe to construct Rhino
// inside Archicad.exe. Out of process there is no Rhino in Archicad's process:
// Archicad's own hidden Rhino_In/Rhino_Out add-ons keep their opennurbs.dll and
// keep working, Archicad's 3DM import and export come back, and a Rhino that
// access-violates costs a worker. Do not reinstate those checks here; they would
// refuse starts for a conflict that no longer exists.
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

// How long a worker may go without a heartbeat before the supervisor stops
// believing in it. Generous enough to cover a long solve — the worker heartbeats
// from its own IO thread, so a busy solver still answers — and short enough that
// a wedged one is noticed in the same minute.

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
std::atomic<uint32_t> disconnectedGeneration { 0 };

GS::UniString lastMessage;
GS::UniString workerPath;
uint32_t archicadPort = 0;

void Log (const GS::UniString& line)
{
    LogLine (lifecycle.Generation (), workerProcessId, line);
}

// THIS Archicad instance's JSON port — what a Tapir ConnectArchicad component in
// the worker has to be pointed at.
//
// ACAPI_Command_GetHttpConnectionPort is the only authority for it and it is
// main-thread ACAPI, so it is read HERE, natively, on the menu command's own
// thread. The answer is handed to the worker once, on its command line.
//
// ⚠️ TAPIR'S OWN LOOPBACK HTTP IS CORRECT AGAIN, AND THIS IS WHY IT MATTERS.
// HANDOFF §"Tapir needs no change and no fork": out of process the pinned,
// unmodified Tapir .gha works, because the thread its blocking HTTP call
// occupies belongs to the WORKER and Archicad's main thread stays free to
// answer. Tapioca does not intercept that path; it only supplies the port,
// which Tapir cannot discover for itself and guesses wrong the moment a second
// Archicad is open.
uint32_t ArchicadJsonPort ()
{
    UShort port = 0;
    const GSErrCode err = ACAPI_Command_GetHttpConnectionPort (&port);
    if (err != NoError) {
        Log (GS::UniString::Printf ("ACAPI_Command_GetHttpConnectionPort failed (%d); Tapir components will "
                                    "have to be given a port by hand",
                                    (int) err));
        return 0;
    }
    return (uint32_t) port;
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

void SupervisorLoop ()
{
    const uint64_t deadline = HeartbeatDeadlineMs ();

    while (!supervisorStopping.load ()) {
        HANDLE process = nullptr;
        {
            std::lock_guard<std::mutex> lock (controlMutex);
            process = workerProcess;
        }

        // ⚠️ AN ATTACHED PEER HAS NO PROCESS HANDLE AND STILL HAS TO BE
        // SUPERVISED. There is nothing to wait on -- the peer belongs to
        // somebody else, and asking Windows for a handle to it would be asking
        // for the right to kill it -- so this sleeps the same interval and falls
        // through to the disconnect check below, which is the only failure an
        // attached peer HAS. Returning here (as the spawned path does, where a
        // null handle means the teardown already ran) would leave a lost peer
        // looking connected forever.
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
        if (lostGeneration == lifecycle.Generation ()) {
            GS::UniString report;
            {
                std::lock_guard<std::mutex> lock (controlMutex);
                report = "The Grasshopper worker disconnected from its bridge and was stopped. Archicad is "
                         "unaffected; open Tapioca > Grasshopper Editor again to start a new one.";
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
            report = GS::UniString::Printf (
                "The Grasshopper worker stopped answering (no heartbeat for %u ms) and was stopped. A "
                "definition that will not return is the usual cause. Archicad and your project are "
                "unaffected; anything the definition had already written to the project is still written.",
                (unsigned int) silence);
            TearDownLocked (true, report, true);
        }
        ReportToUser (report);
        return;
    }
}

// ⚠️ CALLED WITH controlMutex HELD.
bool StartWorkerLocked (GS::UniString& message)
{
    disconnectedGeneration.store (0);

    std::wstring executable;
    std::wstring workingDirectory;
    if (!ResolveWorker (executable, workingDirectory)) {
        message = "Tapioca.GhWorker.exe was not found beside the add-on. Rebuild the add-on with the .NET SDK "
                  "installed, or set TAPIOCA_GH_WORKER_DIR to the folder that holds the worker.";
        return false;
    }
    workerPath = FromWide (executable);

    // The bridge FIRST. The worker connects to a name it is given on its command
    // line, and a name that is not listening yet is a startup race with no
    // upside.
    GhBridge& bridge = GhBridge::Get ();
    GS::UniString bridgeError;
    if (!bridge.Start (lifecycle.Generation (), bridgeError)) {
        message = bridgeError;
        return false;
    }

    archicadPort = ArchicadJsonPort ();

    const std::wstring pipeName ((const wchar_t*) bridge.PipeName ().ToUStr ().Get ());
    std::wstring commandLine = L"\"" + executable + L"\" --pipe " + pipeName + L" --protocol " +
                               std::to_wstring (protocol::Version) + L" --generation " +
                               std::to_wstring (lifecycle.Generation ()) + L" --archicad-port " +
                               std::to_wstring (archicadPort) + L" --mode authoring";

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
    const BOOL created = CreateProcessW ((LPCWSTR) executable.c_str (), mutableCommandLine.data (), nullptr, nullptr,
                                         FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                         (LPCWSTR) workingDirectory.c_str (), &startup, &process);
    if (created == 0) {
        const DWORD win32Error = GetLastError ();
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf ("Could not start Tapioca.GhWorker.exe (Win32 error %u). Verify the .NET 8 "
                                         "Windows Desktop Runtime is installed.",
                                         (unsigned int) win32Error);
        return false;
    }
    if (AssignProcessToJobObject (job, process.hProcess) == 0) {
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
        DWORD exitCode = 0;
        GetExitCodeProcess (process.hProcess, &exitCode);
        CloseHandle (process.hProcess);
        CloseHandle (job);
        bridge.Stop ();
        message = GS::UniString::Printf (
            "Tapioca.GhWorker.exe exited during startup (code %u). Verify the .NET 8 Windows Desktop Runtime "
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
        supervisor = std::thread (SupervisorLoop);
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
bool EnsureRunningLocked (GS::UniString& message)
{
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
    if (!StartWorkerLocked (message)) {
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
    GS::UniString text = FromUtf8Std (protocol::DescribeRunReport (report));

    // ⚠️ SAID OUT LOUD, EVERY TIME, AND NOT ONLY WHEN SOMETHING WENT WRONG. The
    // bridge refuses Tapioca write commands, but Tapir reaches Archicad over its
    // own loopback HTTP, which Tapioca does not intercept -- so a definition
    // holding Tapir write components has already changed the project by the time
    // this dialog appears, and no amount of killing the worker takes that back.
    // A run report that stayed silent about it would be read as a safety
    // guarantee it is not making.
    text += GS::UniString ("\n\nNote: Tapir components reach Archicad on their own connection, which Tapioca "
                           "does not gate. Anything this definition wrote to the project is already written.");

    ReportToUser (GS::UniString ("Grasshopper run\n\n") + text);
}

void OnWorkerDisconnected (uint32_t generation)
{
    disconnectedGeneration.store (generation);
    // Told immediately rather than when the supervisor gets round to the
    // teardown: the controller's job on this event is to stop anything from
    // being applied out of a session whose worker is gone, and that has to be
    // true from the moment the pipe drops.
    workflow.OnHostGone ("The Grasshopper worker disconnected from its bridge.");
}

// Decodes one session message and hands it to the controller.
//
// ⚠️ A PAYLOAD THAT WILL NOT DECODE IS LOGGED AND DROPPED, NEVER GUESSED AT.
// Every one of these carries a routing envelope that decides whether a solution
// may be published; a partially-read one would be a publication decision made on
// bytes nobody could vouch for.
void OnSessionMessage (protocol::MessageType type, const std::vector<uint8_t>& payload)
{
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

// ⚠️ THE BRIDGE'S HANDLERS, NOT THE EDITOR'S, AND EVERY START PATH NEEDS ALL
// FOUR. The startup acknowledgement is how the host learns a runtime is up, the
// disconnect fails an in-flight request, the session handler routes six message
// types, and the sender is what the controller writes through. A start that
// skipped them would open a session against a bridge whose replies reached
// nobody. Three paths wanted them and two copies had already drifted apart.
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

bool GhWorkerHost::AttachLocal (GS::UniString& message)
{
    if (!MainThreadGate::Get ().IsMainThread ()) {
        message = "The Grasshopper bridge can only be opened from Archicad's main thread.";
        return false;
    }

    WireBridgeLocked ();

    GhBridge& bridge = GhBridge::Get ();
    std::lock_guard<std::mutex> lock (controlMutex);

    if (bridge.IsConnected () && lifecycle.AcceptsMessages ()) {
        message = GS::UniString ("A Grasshopper peer is already connected (") +
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

    Log (GS::UniString ("===== Grasshopper bridge open for an attached peer ====="));

    GS::UniString bridgeError;
    if (!bridge.Start (lifecycle.Generation (), bridgeError)) {
        lifecycle.Fail (lifecycle.Generation (), std::string ("bridge start failed"));
        message = bridgeError;
        lastMessage = message;
        Log (message);
        return false;
    }

    // ⚠️ THE SUPERVISOR IS STARTED FOR AN ATTACHED PEER TOO. It has no process
    // to wait on, but the disconnect is the failure an attached peer HAS, and
    // the disconnect is handled in the supervisor loop -- see the null-handle
    // branch there. A peer with nothing supervising it is exactly what this
    // design exists to avoid.
    supervisorStopping.store (false);
    try {
        supervisor = std::thread (SupervisorLoop);
    }
    catch (...) {
        TearDownLocked (false, GS::UniString ());
        message = "Could not create the Grasshopper supervisor thread; the bridge was closed.";
        return false;
    }

    message = GS::UniString ("Waiting for a Grasshopper peer on \\\\.\\pipe\\") + bridge.PipeName () +
              GS::UniString (". Nothing was started: connect from a Rhino that is already running.");
    lastMessage = message;
    Log (message);
    return true;
}

bool GhWorkerHost::HideEditor (GS::UniString& message)
{
    // Deliberately does NOT start anything: "no canvas on screen" is already
    // true when there is no worker, and spawning Rhino to satisfy a request to
    // see less of it would be absurd.
    std::lock_guard<std::mutex> lock (controlMutex);
    showEditorOnConnect.store (false);
    if (!lifecycle.AcceptsMessages () || !GhBridge::Get ().IsConnected ()) {
        message = "The Grasshopper worker is not running.";
        return true;
    }
    return GhBridge::Get ().Send (protocol::MessageType::HideEditor, message);
}

void GhWorkerHost::Stop ()
{
    // ⚠️ ORDER. supervisorStopping FIRST and OUTSIDE the mutex, then the join,
    // then the mutex. The supervisor takes controlMutex during its own teardown,
    // so taking it before the join would deadlock this thread against that one.
    lifecycle.BeginStop ();
    supervisorStopping.store (true);
    if (supervisor.joinable ())
        supervisor.join ();

    std::lock_guard<std::mutex> lock (controlMutex);
    if (workerProcess == nullptr) {
        GhBridge::Get ().Stop ();
        lifecycle.CompleteStop ();
        return;
    }

    Log (GS::UniString ("===== Grasshopper worker stop ====="));

    // Cooperative first: a worker told to shut down closes its own Rhino, which
    // is the only way its temporary files and licence lease are released
    // tidily. The guarantee follows regardless.
    //
    // ⚠️ THE WAIT IS SIZED FOR RhinoCore::Dispose, NOT FOR A MESSAGE ROUND TRIP.
    // Measured on a real quit: the worker acknowledged the shutdown at once and
    // then spent well over three seconds inside Dispose, so a three-second wait
    // terminated every ordinary quit and reported it as a worker that "did not
    // shut down" -- turning the guarantee, which is meant to be the exception,
    // into the normal path and losing the tidy licence release every time.
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
    const GhBridge& bridge = GhBridge::Get ();
    GS::UniString text = GS::UniString::Printf ("Grasshopper worker: %s", DescribeHostState (lifecycle.State ()));
    text += GS::UniString::Printf ("\nRestart generation: %u", (unsigned int) lifecycle.Generation ());
    text += GS::UniString ("\nPeer: ") + GS::UniString (DescribePeerOwnership (lifecycle.Ownership ()));

    const uint32_t pid = bridge.WorkerProcessId ();
    if (pid != 0)
        text += GS::UniString::Printf ("\nWorker process: %u", (unsigned int) pid);
    else
        text += GS::UniString ("\nWorker process: none");

    if (!workerPath.IsEmpty ())
        text += GS::UniString ("\nWorker executable: ") + workerPath;

    const GS::UniString pipeName = bridge.PipeName ();
    if (!pipeName.IsEmpty ())
        text += GS::UniString ("\nBridge: \\\\.\\pipe\\") + pipeName +
                (bridge.IsConnected () ? GS::UniString (" (connected)") : GS::UniString (" (waiting for the worker)"));
    else
        text += GS::UniString ("\nBridge: not listening");

    if (bridge.IsConnected ())
        text +=
            GS::UniString::Printf ("\nLast heartbeat: %u ms ago", (unsigned int) bridge.MillisecondsSinceHeartbeat ());

    // Spelled out even when it is unavailable, because that is the case a user
    // has to act on: the port is what a Tapir ConnectArchicad component must be
    // given, and there is nowhere else to look it up for THIS instance.
    if (archicadPort != 0)
        text += GS::UniString::Printf ("\nArchicad JSON port (Tapir ConnectArchicad): %u", (unsigned int) archicadPort);
    else if (lifecycle.IsRunning ())
        text += GS::UniString ("\nArchicad JSON port: unavailable - a Tapir ConnectArchicad component will "
                               "need one entered by hand");

    // The two sides' accounts, side by side and never merged: a disagreement is
    // the first symptom of a half-torn-down worker, and averaging them into one
    // line would hide exactly the case worth seeing.
    const std::string failure = lifecycle.LastError ();
    if (!failure.empty ())
        text += GS::UniString ("\nLast failure: ") + GS::UniString (failure.c_str ());
    if (!lastMessage.IsEmpty ())
        text += GS::UniString ("\nLast message: ") + lastMessage;
    const GS::UniString workerMessage = bridge.LastWorkerMessage ();
    if (!workerMessage.IsEmpty ())
        text += GS::UniString ("\nLast worker message: ") + workerMessage;

    const GS::UniString logPath = LogPath ();
    if (!logPath.IsEmpty ())
        text += GS::UniString ("\nLog: ") + logPath;
    return text;
}

bool GhWorkerHost::RunDefinition (GS::UniString& message)
{
    std::lock_guard<std::mutex> lock (controlMutex);
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
