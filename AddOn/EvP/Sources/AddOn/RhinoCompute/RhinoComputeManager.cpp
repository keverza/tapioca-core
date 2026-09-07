#include "RhinoCompute/RhinoComputeManager.hpp"

#include "RhinoCompute/ComputeLog.hpp"

#include "ACAPinc.h" // ACAPI_WriteReport, for the menu entry points only

#include <httplib.h>

#include <windows.h>

#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace evp {
namespace rhinocompute {

namespace {

// ── process state ────────────────────────────────────────────────────────────

std::mutex controlMutex;
grasshopper::HostLifecycle lifecycle;

HANDLE workerProcess = nullptr;
DWORD workerPid = 0;

// ⚠️ THE JOB OBJECT IS THE ONLY THING THAT MAKES RULE 2 TRUE WHEN WE ARE THE
// ONES WHO DIE. Stop() handles the orderly case, and APINotify_Quit covers a
// clean exit — but neither runs if Archicad crashes or is killed from Task
// Manager, and an orphaned compute.geometry.exe then holds ~650 MB and a Rhino
// licence until someone notices it in the process list.
//
// This was observed for real during development, from the other direction: the
// worker died with whatever shell launched it, repeatedly and invisibly. That
// coupling must be a DECISION rather than an accident, and this is where the
// decision is written down: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ties the worker's
// life to this process's, whatever ends it.
HANDLE workerJob = nullptr;

std::thread pollThread;
std::atomic<bool> pollStop { false };
std::atomic<int> inFlight { 0 };
std::atomic<bool> ready { false };

ComputeConfig config;
std::string sessionKey;
std::function<void (ComputeState)> stateHandler;

constexpr uint32_t PollIntervalMs = 2000;
// A worker that dies this fast never reached Rhino, so its exit code is the only
// diagnostic there will be. Same reasoning and same window as GhWorkerHost.
constexpr uint32_t StartupExitWindowMs = 250;
constexpr uint32_t CooperativeShutdownMs = 3000;

std::string BaseUrl ()
{
    return "http://" + config.host + ":" + std::to_string (config.port);
}

// A per-session secret for RHINO_COMPUTE_KEY. Unset, the worker logs "API
// authentication is disabled. All endpoints are open to any caller." — and it is
// bound to loopback, but loopback includes every other program on the machine.
std::string MakeSessionKey ()
{
    static const char* alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device device;
    std::uniform_int_distribution<int> pick (0, 61);

    std::string key;
    key.reserve (40);
    for (int i = 0; i < 40; ++i)
        key.push_back (alphabet[pick (device)]);

    return key;
}

void SetState (ComputeState state)
{
    std::function<void (ComputeState)> handler;
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        handler = stateHandler;
    }

    // ⚠️ CALLED OUTSIDE THE LOCK, AND ON THE POLL THREAD. The handler ends up
    // posting to MainThreadGate; holding controlMutex across it would let a main
    // thread calling Stop() deadlock against a poll thread reporting readiness.
    if (handler)
        handler (state);
}

GS::UniString ModuleDirectory ()
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) &ModuleDirectory, &self) == 0) {
        return GS::UniString ();
    }

    std::vector<wchar_t> buffer (32768);
    const DWORD written = GetModuleFileNameW (self, buffer.data (), (DWORD) buffer.size ());
    if (written == 0 || written >= buffer.size ())
        return GS::UniString ();

    std::wstring path (buffer.data (), written);
    const size_t slash = path.find_last_of (L"\\/");
    if (slash == std::wstring::npos)
        return GS::UniString ();

    return GS::UniString ((const GS::uchar_t*) path.substr (0, slash).c_str ());
}

bool FileExists (const GS::UniString& path)
{
    if (path.IsEmpty ())
        return false;

    const DWORD attributes = GetFileAttributesW ((LPCWSTR) path.ToUStr ().Get ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ── HTTP ─────────────────────────────────────────────────────────────────────

// One client per call rather than a shared one. httplib::Client is not designed
// to be hammered from several threads, and a solve is already dominated by the
// solve itself; a connection pool would optimise the wrong microsecond and
// introduce a race worth days.
bool Post (const char* path, const std::string& body, uint32_t timeoutMs, std::string& response, GS::UniString& message)
{
    httplib::Client client (config.host, config.port);
    client.set_connection_timeout (0, 2000 * 1000);
    client.set_read_timeout (timeoutMs / 1000, (timeoutMs % 1000) * 1000);
    client.set_write_timeout (10, 0);

    httplib::Headers headers;
    if (!sessionKey.empty ())
        headers.emplace ("RhinoComputeKey", sessionKey);

    const httplib::Result result = client.Post (path, headers, body, "application/json");
    if (!result) {
        message = GS::UniString::Printf ("rhino.compute did not answer %s (transport error %d).", path,
                                         (int) result.error ());
        return false;
    }

    response = result->body;

    if (result->status != 200) {
        // The body carries compute's own reason and is far more useful than the
        // status. "No compute server found" in particular means the worker is up
        // but has nothing behind it.
        message = GS::UniString::Printf ("rhino.compute answered %d for %s: %s", result->status, path,
                                         response.substr (0, 400).c_str ());
        return false;
    }

    return true;
}

bool ProbeVersion (std::string& body)
{
    httplib::Client client (config.host, config.port);
    client.set_connection_timeout (0, 500 * 1000);
    client.set_read_timeout (5, 0);

    httplib::Headers headers;
    if (!sessionKey.empty ())
        headers.emplace ("RhinoComputeKey", sessionKey);

    const httplib::Result result = client.Get ("/version", headers);
    if (!result || result->status != 200)
        return false;

    body = result->body;

    // ⚠️ A 200 IS NOT READINESS. The parent answers /version by proxying to a
    // child, and with no child it returns a JSON error body. IsNoServerResponse
    // is what tells those apart.
    return !IsNoServerResponse (body) && body.find ("rhino") != std::string::npos;
}

// Every compute.geometry.exe on the machine except one we know about.
//
// ⚠️ THIS REPORTS; IT NEVER KILLS. A compute.geometry.exe Tapioca did not start
// belongs to someone else - a developer's own worker, a Hops component serving
// the user's Rhino, another Archicad. Killing it would take down work this
// add-on knows nothing about.
//
// It exists because the alternative is worse than useless: on the first live run
// of "Stop rhino.compute" the command appeared to do nothing, because the worker
// on screen had been started outside Archicad and was never Tapioca's to kill.
// Saying so is the difference between a confusing no-op and an answer.
std::vector<uint32_t> ForeignComputeProcesses (uint32_t ownPid)
{
    std::vector<uint32_t> found;

    const HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return found;

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof (entry);

    if (Process32FirstW (snapshot, &entry) != 0) {
        do {
            if (_wcsicmp (entry.szExeFile, L"compute.geometry.exe") != 0)
                continue;
            if (ownPid != 0 && entry.th32ProcessID == ownPid)
                continue;

            found.push_back ((uint32_t) entry.th32ProcessID);
        } while (Process32NextW (snapshot, &entry) != 0);
    }

    CloseHandle (snapshot);
    return found;
}

void KillWorker ()
{
    if (workerProcess != nullptr) {
        TerminateProcess (workerProcess, 1);
        WaitForSingleObject (workerProcess, 2000);
        CloseHandle (workerProcess);
        workerProcess = nullptr;
    }

    workerPid = 0;

    if (workerJob != nullptr) {
        // Closing the job kills anything still in it, which is the guarantee.
        CloseHandle (workerJob);
        workerJob = nullptr;
    }
}

// The poll thread: wins readiness, then watches for death.
void PollLoop (uint32_t generation, uint32_t pid)
{
    using clock = std::chrono::steady_clock;
    const clock::time_point deadline = clock::now () + std::chrono::milliseconds (config.readinessTimeoutMs);

    std::string version;
    bool becameReady = false;

    while (!pollStop.load ()) {
        HANDLE process = nullptr;
        {
            std::lock_guard<std::mutex> lock (controlMutex);
            process = workerProcess;
        }

        if (process == nullptr)
            return;

        if (WaitForSingleObject (process, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess (process, &exitCode);
            ready.store (false);
            {
                std::lock_guard<std::mutex> lock (controlMutex);
                if (lifecycle.State () == grasshopper::HostState::Starting)
                    lifecycle.FailStart ("compute.geometry.exe exited during startup (code " +
                                         std::to_string (exitCode) + ").");
            }

            LogLine (generation, pid,
                     GS::UniString::Printf ("compute worker exited, code %u", (unsigned int) exitCode));
            SetState (ComputeState::Failed);
            return;
        }

        if (!becameReady) {
            if (ProbeVersion (version)) {
                becameReady = true;
                ready.store (true);
                {
                    std::lock_guard<std::mutex> lock (controlMutex);
                    lifecycle.CompleteStart ();
                }

                LogLine (generation, pid, GS::UniString::Printf ("compute ready: %s", version.c_str ()));
                SetState (ComputeState::Ready);
            }
            else if (clock::now () > deadline) {
                {
                    std::lock_guard<std::mutex> lock (controlMutex);
                    lifecycle.FailStart ("compute.geometry.exe did not answer /version in time.");
                    KillWorker ();
                }

                LogLine (generation, pid, GS::UniString ("compute readiness timed out"));
                SetState (ComputeState::Failed);
                return;
            }
        }

        for (uint32_t waited = 0; waited < PollIntervalMs && !pollStop.load (); waited += 100)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
}

} // namespace

const char* DescribeComputeState (ComputeState state)
{
    switch (state) {
        case ComputeState::Offline:
            return "offline";
        case ComputeState::Starting:
            return "starting";
        case ComputeState::Ready:
            return "ready";
        case ComputeState::Busy:
            return "busy";
        case ComputeState::Failed:
            return "failed";
    }

    return "unknown";
}

RhinoComputeManager& RhinoComputeManager::Get ()
{
    static RhinoComputeManager instance;
    return instance;
}

ComputeState RhinoComputeManager::State () const
{
    std::lock_guard<std::mutex> lock (controlMutex);
    switch (lifecycle.State ()) {
        case grasshopper::HostState::NotStarted:
        case grasshopper::HostState::Stopped:
            return ComputeState::Offline;
        case grasshopper::HostState::Starting:
        case grasshopper::HostState::Stopping:
            return ComputeState::Starting;
        case grasshopper::HostState::Running:
            return inFlight.load () > 0 ? ComputeState::Busy : ComputeState::Ready;
        case grasshopper::HostState::Failed:
            return ComputeState::Failed;
    }

    return ComputeState::Offline;
}

bool RhinoComputeManager::IsReady () const
{
    return ready.load ();
}

RequestSequence& RhinoComputeManager::Sequence ()
{
    static RequestSequence sequence;
    return sequence;
}

void RhinoComputeManager::OnStateChanged (std::function<void (ComputeState)> handler)
{
    std::lock_guard<std::mutex> lock (controlMutex);
    stateHandler = std::move (handler);
}

bool RhinoComputeManager::DiscoverExecutable (GS::UniString& path, GS::UniString& message)
{
    // Beside the add-on first: a Tapioca that ships its own worker must not be
    // overridden by whatever Rhino package happens to be installed.
    const GS::UniString beside = ModuleDirectory () + "\\compute.geometry\\compute.geometry.exe";
    if (FileExists (beside)) {
        path = beside;
        return true;
    }

    // Then the Hops package, which is where a Rhino 8 with Hops installed puts
    // it. The version is part of the path, so this globs rather than guesses.
    wchar_t appData[MAX_PATH] = {};
    const DWORD written = GetEnvironmentVariableW (L"APPDATA", appData, MAX_PATH);
    if (written > 0 && written < MAX_PATH) {
        const std::wstring root = std::wstring (appData) + L"\\McNeel\\Rhinoceros\\packages\\8.0\\Hops";

        WIN32_FIND_DATAW found {};
        const std::wstring pattern = root + L"\\*";
        HANDLE search = FindFirstFileW (pattern.c_str (), &found);
        if (search != INVALID_HANDLE_VALUE) {
            std::wstring best;
            do {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    continue;
                if (found.cFileName[0] == L'.')
                    continue;

                const std::wstring candidate =
                    root + L"\\" + found.cFileName + L"\\compute.geometry\\compute.geometry.exe";
                // Lexicographic order is not version order (0.10 sorts before
                // 0.9), but Hops ships one version at a time and taking the last
                // match is honest about being a tie-break rather than a policy.
                if (FileExists (GS::UniString ((const GS::uchar_t*) candidate.c_str ())))
                    best = candidate;
            } while (FindNextFileW (search, &found) != 0);

            FindClose (search);

            if (!best.empty ()) {
                path = GS::UniString ((const GS::uchar_t*) best.c_str ());
                return true;
            }
        }
    }

    message = "Could not find compute.geometry.exe. Install Hops for Rhino 8, or set the path in Tapioca's settings.";
    return false;
}

bool RhinoComputeManager::Start (GS::UniString& message)
{
    grasshopper::StartDecision decision;
    uint32_t generation = 0;

    {
        std::lock_guard<std::mutex> lock (controlMutex);
        decision = lifecycle.BeginStart ();
        generation = lifecycle.Generation ();
    }

    if (decision == grasshopper::StartDecision::AlreadyRunning) {
        message = "rhino.compute is already running.";
        LogLine (generation, workerPid, message);
        return true;
    }

    if (decision == grasshopper::StartDecision::InProgress) {
        message = "rhino.compute is already starting.";
        LogLine (generation, workerPid, message);
        return true;
    }

    LogLine (generation, 0, "start requested");

    GS::UniString executable = config.executable;
    if (executable.IsEmpty () && !DiscoverExecutable (executable, message)) {
        // ⚠️ LOG BEFORE RETURNING. This branch was silent, and a silent refusal
        // here is indistinguishable from a menu command that never ran - which is
        // exactly how it was first reported.
        LogLine (generation, 0, GS::UniString ("start refused: ") + message);
        std::lock_guard<std::mutex> lock (controlMutex);
        lifecycle.FailStart (message.ToCStr ().Get ());
        return false;
    }

    LogLine (generation, 0, GS::UniString ("using compute.geometry.exe at ") + executable);

    if (sessionKey.empty ())
        sessionKey = MakeSessionKey ();

    // ⚠️ THE WORKING DIRECTORY IS SET EXPLICITLY BECAUSE THE WORKER TAKES ITS
    // CONTENT ROOT FROM ITS LAUNCHER'S CWD. Left alone it inherits Archicad's,
    // which is wherever the user last opened a file from.
    GS::UniString workingDirectory = executable;
    const auto slash = workingDirectory.FindLast ('\\');
    if (slash != MaxUIndex)
        workingDirectory = workingDirectory.GetSubstring (0, slash);

    // !! CONFIGURATION IS BY ENVIRONMENT. compute.geometry.exe IGNORES --port; it
    // listened on 5000 when passed --port 6002. Measured, not assumed.
    //
    // !! THE BLOCK MUST INHERIT THE CURRENT ENVIRONMENT, NOT REPLACE IT. Passing
    // CREATE_UNICODE_ENVIRONMENT with a block holding only our two variables
    // hands the child an environment with no PATH, no SystemRoot, no TEMP and no
    // APPDATA. The .NET runtime cannot initialise without those and dies before
    // it reaches Main: the child exited with 0xC0000005 (ACCESS_VIOLATION,
    // reported as 3221225477) within milliseconds of every spawn, and because
    // the crash is inside the runtime there is no managed stack and nothing in
    // any log to find. Measured live, and it cost a whole round of "compute
    // still fails to start" with no evidence to show for it.
    //
    // So: copy the whole block, drop only the two names we are about to set, and
    // append ours. Entries beginning "=" are the per-drive current directories
    // and are KEPT - dropping them changes how relative paths resolve.
    std::wstring environment;

    LPWCH inherited = GetEnvironmentStringsW ();
    if (inherited != nullptr) {
        for (LPCWSTR entry = inherited; *entry != L'\0';) {
            const size_t length = wcslen (entry);
            const bool ours =
                _wcsnicmp (entry, L"RHINO_COMPUTE_URLS=", 19) == 0 || _wcsnicmp (entry, L"RHINO_COMPUTE_KEY=", 18) == 0;
            if (!ours) {
                environment.append (entry, length);
                environment.push_back (L'\0');
            }

            entry += length + 1;
        }

        FreeEnvironmentStringsW (inherited);
    }

    const auto appendVar = [&environment] (const std::wstring& name, const std::string& value) {
        environment += name + L"=" + std::wstring (value.begin (), value.end ());
        environment.push_back (L'\0');
    };

    appendVar (L"RHINO_COMPUTE_URLS", "http://" + config.host + ":" + std::to_string (config.port));
    appendVar (L"RHINO_COMPUTE_KEY", sessionKey);

    // The block itself is terminated by a second NUL.
    environment.push_back (L'\0');

    std::wstring commandLine = L"\"" + std::wstring ((const wchar_t*) executable.ToUStr ().Get ()) + L"\"";
    std::vector<wchar_t> mutableCommandLine (commandLine.begin (), commandLine.end ());
    mutableCommandLine.push_back (L'\0');

    HANDLE job = CreateJobObjectW (nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION process {};

    const BOOL created =
        CreateProcessW ((LPCWSTR) executable.ToUStr ().Get (), mutableCommandLine.data (), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, (LPVOID) environment.data (),
                        (LPCWSTR) workingDirectory.ToUStr ().Get (), &startup, &process);
    if (created == 0) {
        const DWORD win32Error = GetLastError ();
        if (job != nullptr)
            CloseHandle (job);

        message =
            GS::UniString::Printf ("Could not start compute.geometry.exe (Win32 error %u).", (unsigned int) win32Error);
        LogLine (generation, 0, message);
        std::lock_guard<std::mutex> lock (controlMutex);
        lifecycle.FailStart (message.ToCStr ().Get ());
        return false;
    }

    if (job != nullptr)
        AssignProcessToJobObject (job, process.hProcess);

    CloseHandle (process.hThread);

    if (WaitForSingleObject (process.hProcess, StartupExitWindowMs) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess (process.hProcess, &exitCode);
        CloseHandle (process.hProcess);
        if (job != nullptr)
            CloseHandle (job);

        message = GS::UniString::Printf (
            "compute.geometry.exe exited immediately (code %u). Check that Rhino 8 is installed and licensed.",
            (unsigned int) exitCode);
        LogLine (generation, 0, message);
        std::lock_guard<std::mutex> lock (controlMutex);
        lifecycle.FailStart (message.ToCStr ().Get ());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock (controlMutex);
        workerProcess = process.hProcess;
        workerPid = process.dwProcessId;
        workerJob = job;
    }

    if (pollThread.joinable ())
        pollThread.join ();

    pollStop.store (false);
    pollThread = std::thread (PollLoop, generation, (uint32_t) process.dwProcessId);

    LogLine (generation, (uint32_t) process.dwProcessId,
             GS::UniString::Printf ("spawned compute.geometry.exe on %s", BaseUrl ().c_str ()));

    SetState (ComputeState::Starting);

    // Rule 4: accepted, not ready. Cold start is 50-90 s.
    message = "Starting rhino.compute. This takes up to a minute and a half on first use.";
    return true;
}

void RhinoComputeManager::Stop ()
{
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        owned = lifecycle.BeginStop ();
    }

    // !! NEVER RETURN BEFORE JOINING, WHATEVER BeginStop DECIDED.
    //
    // BeginStop answers false unless the state is exactly Running. So once a
    // worker has died on its own the state is Failed, this function used to
    // return right here, and the poll thread was left UNJOINED. A std::thread
    // that has finished running is still joinable, and a joinable std::thread
    // destroyed at DLL unload calls std::terminate.
    //
    // That is not a leak, it is a CRASH ON QUIT, and it is exactly what was
    // reported: the worker crashed on spawn, Archicad was closed, and Archicad
    // went down with it. Ownership of the LIFECYCLE TRANSITION and ownership of
    // the THREAD are different questions, and BeginStop only answers the first.
    ready.store (false);
    pollStop.store (true);
    if (pollThread.joinable ())
        pollThread.join ();

    {
        std::lock_guard<std::mutex> lock (controlMutex);
        if (workerProcess != nullptr) {
            // No cooperative shutdown protocol exists: compute.geometry has no
            // endpoint that asks it to exit. The wait is for an in-flight solve
            // to finish rather than for a request to be honoured.
            if (inFlight.load () > 0)
                WaitForSingleObject (workerProcess, CooperativeShutdownMs);

            KillWorker ();
        }

        if (owned)
            lifecycle.CompleteStop ();
    }

    SetState (ComputeState::Offline);
}

bool RhinoComputeManager::Restart (GS::UniString& message)
{
    Stop ();
    Sequence ().ResetAccepted ();
    return Start (message);
}

bool RhinoComputeManager::InspectDefinition (const GS::UniString& ghFile, WorkflowSchema& schema,
                                             GS::UniString& message)
{
    if (!ready.load ()) {
        message = "rhino.compute is not ready yet.";
        return false;
    }

    std::ifstream file ((const char*) ghFile.ToCStr ().Get (), std::ios::binary);
    if (!file) {
        message = "Could not read the definition: " + ghFile;
        return false;
    }

    const std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
    if (bytes.empty ()) {
        message = "The definition is empty: " + ghFile;
        return false;
    }

    inFlight.fetch_add (1);
    std::string response;
    const bool ok = Post ("/io", BuildIoRequest (ToBase64 (bytes)), config.solveTimeoutMs, response, message);
    inFlight.fetch_sub (1);

    if (!ok)
        return false;

    if (!ParseIoResponse (response, schema))
        return false;

    // A definition whose inputs collide or whose types are unsupported is
    // reported, not solved: compute answers 200 and then returns empty trees,
    // which reads as "produced nothing" rather than "is wrong".
    if (!schema.errors.empty ()) {
        message = GS::UniString ((const char*) schema.errors[0].message.c_str ());
        return false;
    }

    return true;
}

bool RhinoComputeManager::Solve (const SolveRequest& request, SolveResult& result, GS::UniString& message)
{
    if (!ready.load ()) {
        message = "rhino.compute is not ready yet.";
        return false;
    }

    inFlight.fetch_add (1);
    std::string response;
    const bool ok = Post ("/grasshopper", BuildSolveRequest (request), config.solveTimeoutMs, response, message);
    inFlight.fetch_sub (1);

    if (!ok)
        return false;

    result.requestId = request.requestId;
    if (!ParseSolveResponse (response, result))
        return false;

    if (!result.errors.empty ()) {
        message = GS::UniString ((const char*) result.errors[0].message.c_str ());
        return false;
    }

    return true;
}

void RhinoComputeManager::StartFromMenu ()
{
    // Every branch below logs BEFORE it reports. A user who hits a problem is
    // asked for logs/rhinocompute.log, and a dialog they already dismissed is
    // not evidence.
    GS::UniString message;
    const bool accepted = Get ().Start (message);

    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        generation = lifecycle.Generation ();
    }

    LogLine (generation, workerPid,
             GS::UniString ("menu Start rhino.compute: ") + (accepted ? "accepted - " : "REFUSED - ") + message);

    // ⚠️ ACCEPTED IS NOT READY, AND THE MESSAGE MUST NOT PRETEND OTHERWISE.
    // Cold start was measured at 50-90 s. Saying "started" here and leaving the
    // user to wonder why nothing happens for a minute and a half is how this
    // feature gets reported as broken.
    // Held in a NAMED LOCAL rather than concatenated inline: UniString + UniString
    // yields a Concatenation proxy, and ToPrintf on a temporary is the same
    // owning-temporary hazard GhWorkerHost hit with UStr.
    const GS::UniString report = message + GS::UniString ("\n\n") + Get ().Describe ();
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

void RhinoComputeManager::StopFromMenu ()
{
    uint32_t generation = 0;
    uint32_t pid = 0;
    {
        std::lock_guard<std::mutex> lock (controlMutex);
        generation = lifecycle.Generation ();
        pid = workerPid;
    }

    const ComputeState before = Get ().State ();
    LogLine (generation, pid,
             GS::UniString::Printf ("menu Stop rhino.compute: requested while %s", DescribeComputeState (before)));

    Get ().Stop ();

    LogLine (generation, pid, "menu Stop rhino.compute: worker terminated");

    GS::UniString report = before == ComputeState::Offline ? GS::UniString ("Tapioca was not running rhino.compute.")
                                                           : GS::UniString ("rhino.compute stopped.");

    // The case that made Stop look broken on its first live run: a worker on
    // screen that Tapioca never spawned. Name it and its pid rather than leaving
    // the user to conclude the command does nothing.
    const std::vector<uint32_t> foreign = ForeignComputeProcesses (0);
    if (!foreign.empty ()) {
        report += GS::UniString ("\n\n");
        report += GS::UniString::Printf ("%u other compute.geometry.exe process(es) are running that Tapioca did "
                                         "not start",
                                         (unsigned int) foreign.size ());
        for (uint32_t pidFound : foreign)
            report += GS::UniString::Printf (" (pid %u)", (unsigned int) pidFound);

        report += GS::UniString (". Tapioca does not kill a worker it does not own - stop it from Rhino, or end "
                                 "it in Task Manager.");

        for (uint32_t pidFound : foreign)
            LogLine (generation, pidFound, "foreign compute.geometry.exe left running (not started by Tapioca)");
    }

    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

GS::UniString RhinoComputeManager::Describe () const
{
    // ⚠️ STATE FIRST, AND OUTSIDE THE LOCK. State () takes controlMutex itself and
    // std::mutex is not recursive, so locking here and then calling State ()
    // self-deadlocks the main thread inside the one call a support question asks
    // for first.
    const ComputeState state = State ();

    std::lock_guard<std::mutex> lock (controlMutex);

    GS::UniString report = GS::UniString::Printf ("rhino.compute: %s\n", DescribeComputeState (state));
    report += GS::UniString::Printf ("endpoint: %s\n", BaseUrl ().c_str ());
    report += GS::UniString::Printf ("worker pid: %u (generation %u)\n", (unsigned int) workerPid,
                                     (unsigned int) lifecycle.Generation ());
    report += GS::UniString::Printf ("job object: %s\n", workerJob != nullptr ? "held" : "none");
    report += GS::UniString::Printf ("solves in flight: %d\n", inFlight.load ());
    report += GS::UniString::Printf ("api key: %s\n", sessionKey.empty () ? "NOT SET" : "set (per session)");

    const std::string lastError = lifecycle.LastError ();
    if (!lastError.empty ())
        report += GS::UniString::Printf ("last error: %s\n", lastError.c_str ());

    return report;
}

} // namespace rhinocompute
} // namespace evp
