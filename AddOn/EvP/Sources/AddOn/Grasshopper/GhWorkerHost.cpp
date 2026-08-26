#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GhBridge.hpp"
#include "GhLog.hpp"
#include "GhWorkerHost.hpp"
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

constexpr const wchar_t* WorkerFolderName = L"GhWorker";
constexpr const wchar_t* WorkerExecutableName = L"Tapioca.GhWorker.exe";

// How long a worker may go without a heartbeat before the supervisor stops
// believing in it. Generous enough to cover a long solve — the worker heartbeats
// from its own IO thread, so a busy solver still answers — and short enough that
// a wedged one is noticed in the same minute.
constexpr uint64_t DefaultHeartbeatDeadlineMs = 15000;

// How long the supervisor waits between checks. One second: this is a liveness
// poll, not a latency path.
constexpr DWORD SupervisorIntervalMs = 1000;

// A worker that exits this fast did not fail at Grasshopper; it failed at
// starting at all, and the exit code is the only diagnostic there will be.
constexpr DWORD StartupExitWindowMs = 250;

HostLifecycle lifecycle;

std::mutex controlMutex;
HANDLE workerProcess = nullptr;
DWORD workerProcessId = 0;
std::thread supervisor;
std::atomic<bool> supervisorStopping { false };
std::atomic<bool> showEditorOnConnect { false };

GS::UniString lastMessage;
GS::UniString workerPath;
uint32_t archicadPort = 0;

GS::UniString FromWide (const std::wstring& text)
{
    if (text.empty ())
        return GS::UniString ();
    return GS::UniString (text.c_str ());
}

void Log (const GS::UniString& line)
{
    LogLine (lifecycle.Generation (), workerProcessId, line);
}

std::wstring ParentDirectory (const std::wstring& path)
{
    const size_t separator = path.find_last_of (L"\\/");
    if (separator == std::wstring::npos)
        return {};
    return path.substr (0, separator);
}

// The .apx's own directory. The worker is staged beside it by the build, so this
// is where it is looked for — never the process directory, which is Archicad's.
bool OwnDirectory (std::wstring& directory)
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) &OwnDirectory, &self) == 0)
        return false;

    std::vector<wchar_t> buffer (MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW (self, (LPWSTR) buffer.data (), (DWORD) buffer.size ());
        if (written == 0)
            return false;
        if (written < buffer.size () - 1)
            break;
        buffer.resize (buffer.size () * 2);
    }

    directory = ParentDirectory (buffer.data ());
    return !directory.empty ();
}

bool FileExists (const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW ((LPCWSTR) path.c_str ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// TAPIOCA_GH_WORKER_DIR first, so a developer can point Archicad at a worker
// built somewhere else without reinstalling the add-on. Then the staged folder
// beside the .apx, which is what a shipped installation has.
bool ResolveWorker (std::wstring& executable, std::wstring& workingDirectory)
{
    std::vector<std::wstring> directories;

    GS::UniString configured;
    if (evp::ReadEnv (L"TAPIOCA_GH_WORKER_DIR", configured))
        directories.emplace_back ((const wchar_t*) configured.ToUStr ().Get ());

    std::wstring own;
    if (OwnDirectory (own)) {
        directories.push_back (own + L"\\" + std::wstring (WorkerFolderName));
        directories.push_back (own);
    }

    for (const std::wstring& directory : directories) {
        if (directory.empty ())
            continue;
        const std::wstring candidate = directory + L"\\" + std::wstring (WorkerExecutableName);
        if (!FileExists (candidate))
            continue;
        executable = candidate;
        workingDirectory = directory;
        return true;
    }
    return false;
}

uint64_t HeartbeatDeadlineMs ()
{
    GS::UniString configured;
    if (!evp::ReadEnv (L"TAPIOCA_GH_HEARTBEAT_MS", configured) || configured.IsEmpty ())
        return DefaultHeartbeatDeadlineMs;

    const auto text = configured.ToCStr ();
    const long parsed = strtol (text.Get (), nullptr, 10);
    // A nonsense value silently becoming "never time out" is worse than
    // ignoring it: this deadline is the only thing that notices a wedged worker.
    return parsed > 0 ? (uint64_t) parsed : DefaultHeartbeatDeadlineMs;
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
void TearDownLocked (bool killWorker, const GS::UniString& reason)
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
    workerProcessId = 0;
    showEditorOnConnect.store (false);

    if (lifecycle.BeginStop ())
        lifecycle.CompleteStop ();
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
        if (process == nullptr)
            return;

        if (WaitForSingleObject (process, SupervisorIntervalMs) == WAIT_OBJECT_0) {
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
                TearDownLocked (false, report);
            }
            // ⚠️ A WORKER CRASH IS A RECOVERABLE EVENT WITH A UI, NOT A CRASH
            // REPORT. HANDOFF §"Supervision is the point".
            ReportToUser (report);
            return;
        }

        if (supervisorStopping.load ())
            return;

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
            TearDownLocked (true, report);
        }
        ReportToUser (report);
        return;
    }
}

// ⚠️ CALLED WITH controlMutex HELD.
bool StartWorkerLocked (GS::UniString& message)
{
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
                               std::to_wstring (archicadPort);

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

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION process {};
    const BOOL created =
        CreateProcessW ((LPCWSTR) executable.c_str (), mutableCommandLine.data (), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, (LPCWSTR) workingDirectory.c_str (), &startup, &process);
    if (created == 0) {
        const DWORD win32Error = GetLastError ();
        bridge.Stop ();
        message = GS::UniString::Printf ("Could not start Tapioca.GhWorker.exe (Win32 error %u). Verify the .NET 8 "
                                         "Windows Desktop Runtime is installed.",
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
        bridge.Stop ();
        message = GS::UniString::Printf (
            "Tapioca.GhWorker.exe exited during startup (code %u). Verify the .NET 8 Windows Desktop Runtime "
            "and check %T for what it managed to say first.",
            (unsigned int) exitCode, bootLog.ToPrintf ());
        return false;
    }

    workerProcess = process.hProcess;
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
        lifecycle.FailStart (std::string ("worker start failed"));
        lastMessage = message;
        Log (message);
        return false;
    }

    // Running means "spawned and supervised", not "connected". The handshake is
    // the bridge's, and the connected handler below is what finishes the job.
    lifecycle.CompleteStart ();
    lastMessage = message;
    Log (message);
    return true;
}

void OnWorkerConnected ()
{
    if (!showEditorOnConnect.exchange (false))
        return;

    GS::UniString error;
    if (!GhBridge::Get ().Send (protocol::MessageType::ShowEditor, error))
        LogLine (lifecycle.Generation (), GhBridge::Get ().WorkerProcessId (), error);
}

} // namespace

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

    GhBridge& bridge = GhBridge::Get ();
    bridge.SetConnectedHandler (&OnWorkerConnected);

    std::lock_guard<std::mutex> lock (controlMutex);

    // ⚠️ THE FLAG IS SET BEFORE THE CONNECTED CHECK, NOT AFTER. The handshake
    // runs on the bridge's IO thread and can complete at any instant; setting
    // the flag afterwards leaves a window in which the handler has already fired
    // with nothing to do and the canvas is never asked for. Set first, and
    // whichever of the two paths gets there first clears it.
    showEditorOnConnect.store (true);

    if (bridge.IsConnected ()) {
        if (!showEditorOnConnect.exchange (false))
            return true; // the handler beat us to it
        if (!bridge.Send (protocol::MessageType::ShowEditor, message))
            return false;
        message = "Asked the Grasshopper worker for its canvas.";
        lastMessage = message;
        return true;
    }

    if (!EnsureRunningLocked (message)) {
        showEditorOnConnect.store (false);
        return false;
    }
    return true;
}

bool GhWorkerHost::HideEditor (GS::UniString& message)
{
    // Deliberately does NOT start anything: "no canvas on screen" is already
    // true when there is no worker, and spawning Rhino to satisfy a request to
    // see less of it would be absurd.
    std::lock_guard<std::mutex> lock (controlMutex);
    showEditorOnConnect.store (false);
    if (!GhBridge::Get ().IsConnected ()) {
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
    supervisorStopping.store (true);
    if (supervisor.joinable ())
        supervisor.join ();

    std::lock_guard<std::mutex> lock (controlMutex);
    if (workerProcess == nullptr) {
        GhBridge::Get ().Stop ();
        return;
    }

    Log (GS::UniString ("===== Grasshopper worker stop ====="));

    // Cooperative first: a worker told to shut down closes its own Rhino, which
    // is the only way its temporary files and licence lease are released
    // tidily. The guarantee follows regardless.
    GS::UniString sendError;
    if (GhBridge::Get ().Send (protocol::MessageType::Shutdown, sendError)) {
        WaitForSingleObject (workerProcess, 3000);
    }
    else {
        Log (sendError);
    }

    DWORD exitCode = 0;
    const bool exited = GetExitCodeProcess (workerProcess, &exitCode) != 0 && exitCode != STILL_ACTIVE;
    TearDownLocked (!exited, exited ? GS::UniString ("Grasshopper worker stopped.")
                                    : GS::UniString ("Grasshopper worker did not shut down and was terminated."));
}

bool GhWorkerHost::IsRunning () const
{
    return lifecycle.IsRunning ();
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

void GhWorkerHost::RestartFromMenu ()
{
    GhWorkerHost& host = Get ();
    const bool wasRunning = host.IsRunning ();
    host.Stop ();

    GS::UniString message;
    const bool started = host.OpenEditor (message);

    const GS::UniString headline =
        wasRunning ? GS::UniString ("The Grasshopper worker was stopped and a new one started.\n\n")
                   : GS::UniString ("There was no Grasshopper worker running; a new one was started.\n\n");
    const GS::UniString report = (started ? headline : GS::UniString ("The Grasshopper worker did not restart.\n\n")) +
                                 message + GS::UniString ("\n\n") + host.Describe ();
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

} // namespace grasshopper
} // namespace evp
