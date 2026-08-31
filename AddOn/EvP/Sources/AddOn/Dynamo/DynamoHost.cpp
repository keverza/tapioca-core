#include "APIEnvir.h"
#include "ACAPinc.h"

#include "DynamoHost.hpp"
#include "DynamoBridge.hpp"
#include "Python/PathUtils.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace evp::dynamo {
namespace {

constexpr const wchar_t* RuntimeDirectoryName = L"DynamoCoreRuntime4.2.1.5887";
constexpr const wchar_t* EditorProcessName = L"DynamoSandbox.exe";
constexpr const wchar_t* HeadlessProcessName = L"Tapioca.DynamoRunner.exe";
constexpr ULONGLONG StartupTimeoutMs = 30 * 1000;
constexpr ULONGLONG RunTimeoutMs = 6 * 60 * 1000;
constexpr ULONGLONG ShutdownTimeoutMs = 3 * 1000;
constexpr size_t MaxProtocolLineBytes = 16 * 1024 * 1024;

HANDLE s_editorProcess = nullptr;
DWORD s_editorProcessId = 0;

enum class HeadlessState { Stopped, Starting, Ready, Running, Faulted };

std::mutex s_headlessMutex;
std::mutex s_headlessLifecycleMutex;
std::mutex s_headlessRunMutex;
HeadlessState s_headlessState = HeadlessState::Stopped;
GS::UniString s_headlessStatus = "Dynamo runner is stopped.";
HANDLE s_headlessProcess = nullptr;
DWORD s_headlessProcessId = 0;
HANDLE s_headlessInput = nullptr;
HANDLE s_headlessOutput = nullptr;
std::thread s_headlessStartupThread;
std::atomic<bool> s_headlessStopping { false };

bool InstallPackage (GS::UniString& error);

bool FileExists (const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW ((LPCWSTR) path.c_str ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ParentDirectory (const std::wstring& path)
{
    const size_t separator = path.find_last_of (L"\\/");
    if (separator == std::wstring::npos)
        return {};
    return path.substr (0, separator);
}

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

bool ResolveRuntime (std::wstring& executable, std::wstring& workingDirectory)
{
    std::vector<std::wstring> directories;

    GS::UniString configuredDirectory;
    if (evp::ReadEnv (L"TAPIOCA_DYNAMO_DIR", configuredDirectory))
        directories.emplace_back ((const wchar_t*) configuredDirectory.ToUStr ().Get ());

    std::wstring ownDirectory;
    if (OwnDirectory (ownDirectory)) {
        directories.push_back (ownDirectory + L"\\Dynamo");
        directories.push_back (ownDirectory + L"\\" + RuntimeDirectoryName);

        // Private workspaces keep large references outside both repositories.
        // Walking ancestors preserves that convention without baking in a user path.
        std::wstring ancestor = ownDirectory;
        for (int depth = 0; depth < 8 && !ancestor.empty (); ++depth) {
            directories.push_back (ancestor + L"\\tapioca-ref\\" + RuntimeDirectoryName);
            const std::wstring parent = ParentDirectory (ancestor);
            if (parent == ancestor)
                break;
            ancestor = parent;
        }
    }

    for (const std::wstring& directory : directories) {
        std::wstring candidate = directory;
        if (candidate.size () < wcslen (EditorProcessName) ||
            _wcsicmp (candidate.c_str () + candidate.size () - wcslen (EditorProcessName), EditorProcessName) != 0)
            candidate += L"\\" + std::wstring (EditorProcessName);
        if (!FileExists (candidate))
            continue;

        executable = candidate;
        workingDirectory = ParentDirectory (candidate);
        return true;
    }
    return false;
}

bool ResolveHeadlessRunner (std::wstring& executable, std::wstring& workingDirectory, GS::UniString& error)
{
    std::wstring ownDirectory;
    if (!OwnDirectory (ownDirectory)) {
        error = "Could not resolve the Tapioca add-on directory for the Dynamo runner.";
        return false;
    }

    executable = ownDirectory + L"\\DynamoRunner\\" + HeadlessProcessName;
    if (!FileExists (executable)) {
        error = "Tapioca.DynamoRunner.exe was not found in the DynamoRunner folder beside Tapioca.apx.";
        return false;
    }

    std::wstring sandbox;
    if (!ResolveRuntime (sandbox, workingDirectory)) {
        error = "The pinned DynamoCoreRuntime4.2.1.5887 runtime was not found. Set TAPIOCA_DYNAMO_DIR or stage "
                "the runtime as Dynamo beside Tapioca.apx.";
        return false;
    }
    return true;
}

std::vector<wchar_t> ChildEnvironment (const GS::UniString& pipeName)
{
    constexpr const wchar_t* name = L"TAPIOCA_DYNAMO_PIPE=";
    constexpr size_t nameLength = 20;
    std::vector<std::wstring> entries;
    LPWCH environment = GetEnvironmentStringsW ();
    if (environment != nullptr) {
        for (const wchar_t* entry = environment; *entry != L'\0'; entry += wcslen (entry) + 1) {
            if (_wcsnicmp (entry, name, nameLength) != 0)
                entries.emplace_back (entry);
        }
        FreeEnvironmentStringsW (environment);
    }
    entries.emplace_back (name + std::wstring ((const wchar_t*) pipeName.ToUStr ().Get ()));
    std::sort (entries.begin (), entries.end (), [] (const std::wstring& left, const std::wstring& right) {
        return _wcsicmp (left.c_str (), right.c_str ()) < 0;
    });

    size_t size = 1;
    for (const std::wstring& entry : entries)
        size += entry.size () + 1;
    std::vector<wchar_t> block;
    block.reserve (size);
    for (const std::wstring& entry : entries) {
        block.insert (block.end (), entry.begin (), entry.end ());
        block.push_back (L'\0');
    }
    block.push_back (L'\0');
    return block;
}

void CloseHandleIfSet (HANDLE& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        CloseHandle (handle);
    handle = nullptr;
}

void SetHeadlessState (HeadlessState state, const GS::UniString& status)
{
    std::lock_guard<std::mutex> lock (s_headlessMutex);
    if (s_headlessStopping.load () && state != HeadlessState::Stopped)
        return;
    s_headlessState = state;
    s_headlessStatus = status;
}

bool HeadlessProcessExited (DWORD& exitCode)
{
    std::lock_guard<std::mutex> lock (s_headlessMutex);
    if (s_headlessProcess == nullptr)
        return false;
    exitCode = STILL_ACTIVE;
    return GetExitCodeProcess (s_headlessProcess, &exitCode) == 0 || exitCode != STILL_ACTIVE;
}

bool ReadProtocolLine (HANDLE output, HANDLE process, ULONGLONG timeoutMs, std::string& line, GS::UniString& error)
{
    line.clear ();
    const ULONGLONG deadline = GetTickCount64 () + timeoutMs;
    while (!s_headlessStopping.load ()) {
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess (process, &exitCode) == 0) {
            error = GS::UniString::Printf ("Could not query the Dynamo runner process (Win32 error %u).",
                                           (unsigned) GetLastError ());
            return false;
        }

        DWORD available = 0;
        if (PeekNamedPipe (output, nullptr, 0, nullptr, &available, nullptr) == 0) {
            error = exitCode == STILL_ACTIVE
                        ? GS::UniString::Printf ("Could not read the Dynamo runner protocol (Win32 error %u).",
                                                 (unsigned) GetLastError ())
                        : GS::UniString::Printf ("The Dynamo runner exited with code %u.", (unsigned) exitCode);
            return false;
        }
        if (available > 0) {
            char byte = '\0';
            DWORD read = 0;
            if (ReadFile (output, &byte, 1, &read, nullptr) == 0 || read != 1) {
                error = "The Dynamo runner protocol stream closed unexpectedly.";
                return false;
            }
            if (byte == '\n')
                return true;
            if (byte != '\r')
                line.push_back (byte);
            if (line.size () > MaxProtocolLineBytes) {
                error = "The Dynamo runner returned an oversized protocol line.";
                return false;
            }
            continue;
        }
        if (exitCode != STILL_ACTIVE) {
            error = GS::UniString::Printf ("The Dynamo runner exited with code %u.", (unsigned) exitCode);
            return false;
        }
        if (GetTickCount64 () >= deadline) {
            error = "Timed out waiting for a response from the Dynamo runner.";
            return false;
        }
        Sleep (10);
    }
    error = "The Dynamo runner is stopping.";
    return false;
}

bool WriteProtocolLine (HANDLE input, HANDLE process, const std::string& line, GS::UniString& error,
                        bool observeStopping = true)
{
    if (line.size () >= MaxProtocolLineBytes) {
        error = "The Dynamo runner request is too large.";
        return false;
    }
    std::string framed = line;
    framed.push_back ('\n');
    size_t offset = 0;
    while (offset < framed.size ()) {
        if (observeStopping && s_headlessStopping.load ()) {
            error = "The Dynamo runner is stopping.";
            return false;
        }
        if (WaitForSingleObject (process, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess (process, &exitCode);
            error = GS::UniString::Printf ("The Dynamo runner exited with code %u.", (unsigned) exitCode);
            return false;
        }
        DWORD written = 0;
        const DWORD count = (DWORD) std::min<size_t> (framed.size () - offset, 64 * 1024);
        if (WriteFile (input, framed.data () + offset, count, &written, nullptr) == 0 || written == 0) {
            error = GS::UniString::Printf ("Could not write to the Dynamo runner (Win32 error %u).",
                                           (unsigned) GetLastError ());
            return false;
        }
        offset += written;
    }
    return true;
}

void CloseHeadlessHandles ()
{
    std::lock_guard<std::mutex> lock (s_headlessMutex);
    CloseHandleIfSet (s_headlessInput);
    CloseHandleIfSet (s_headlessOutput);
    CloseHandleIfSet (s_headlessProcess);
    s_headlessProcessId = 0;
}

void HeadlessStartup ()
{
    std::wstring executable;
    std::wstring workingDirectory;
    GS::UniString error;
    if (!ResolveHeadlessRunner (executable, workingDirectory, error)) {
        SetHeadlessState (HeadlessState::Faulted, error);
        return;
    }
    if (!InstallPackage (error)) {
        SetHeadlessState (HeadlessState::Faulted, error);
        return;
    }

    DynamoBridge& bridge = DynamoBridge::Get ();
    if (!bridge.Start (error)) {
        SetHeadlessState (HeadlessState::Faulted, error);
        return;
    }

    SECURITY_ATTRIBUTES security {};
    security.nLength = sizeof (security);
    security.bInheritHandle = TRUE;
    HANDLE childInput = nullptr;
    HANDLE parentInput = nullptr;
    HANDLE parentOutput = nullptr;
    HANDLE childOutput = nullptr;
    HANDLE childError = CreateFileW (L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (CreatePipe (&childInput, &parentInput, &security, 64 * 1024) == 0 ||
        CreatePipe (&parentOutput, &childOutput, &security, 64 * 1024) == 0 || childError == INVALID_HANDLE_VALUE ||
        SetHandleInformation (parentInput, HANDLE_FLAG_INHERIT, 0) == 0 ||
        SetHandleInformation (parentOutput, HANDLE_FLAG_INHERIT, 0) == 0) {
        error = GS::UniString::Printf ("Could not create the Dynamo runner pipes (Win32 error %u).",
                                       (unsigned) GetLastError ());
        CloseHandleIfSet (childInput);
        CloseHandleIfSet (parentInput);
        CloseHandleIfSet (parentOutput);
        CloseHandleIfSet (childOutput);
        CloseHandleIfSet (childError);
        SetHeadlessState (HeadlessState::Faulted, error);
        return;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList (nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage (attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (attributeStorage.data ());
    HANDLE inheritedHandles[] = { childInput, childOutput, childError };
    if (InitializeProcThreadAttributeList (attributes, 1, 0, &attributeBytes) == 0 ||
        UpdateProcThreadAttribute (attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles,
                                   sizeof (inheritedHandles), nullptr, nullptr) == 0) {
        error = GS::UniString::Printf ("Could not restrict Dynamo runner handle inheritance (Win32 error %u).",
                                       (unsigned) GetLastError ());
        CloseHandleIfSet (childInput);
        CloseHandleIfSet (parentInput);
        CloseHandleIfSet (parentOutput);
        CloseHandleIfSet (childOutput);
        CloseHandleIfSet (childError);
        SetHeadlessState (HeadlessState::Faulted, error);
        return;
    }

    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof (startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput;
    startup.StartupInfo.hStdOutput = childOutput;
    startup.StartupInfo.hStdError = childError;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process {};
    std::wstring commandLine = L"\"" + executable + L"\"";
    std::vector<wchar_t> mutableCommandLine (commandLine.begin (), commandLine.end ());
    mutableCommandLine.push_back (L'\0');
    std::vector<wchar_t> environment = ChildEnvironment (bridge.PipeName ());
    const BOOL created =
        CreateProcessW (executable.c_str (), mutableCommandLine.data (), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                        environment.data (), workingDirectory.c_str (), &startup.StartupInfo, &process);
    const DWORD createError = GetLastError ();
    DeleteProcThreadAttributeList (attributes);
    CloseHandleIfSet (childInput);
    CloseHandleIfSet (childOutput);
    CloseHandleIfSet (childError);
    if (created == 0) {
        CloseHandleIfSet (parentInput);
        CloseHandleIfSet (parentOutput);
        error = GS::UniString::Printf ("Could not start Tapioca.DynamoRunner.exe (Win32 error %u). Verify the .NET "
                                       "10 runtime and the staged runner files.",
                                       (unsigned) createError);
        SetHeadlessState (HeadlessState::Faulted, error);
        return;
    }
    CloseHandle (process.hThread);

    {
        std::lock_guard<std::mutex> lock (s_headlessMutex);
        s_headlessProcess = process.hProcess;
        s_headlessProcessId = process.dwProcessId;
        s_headlessInput = parentInput;
        s_headlessOutput = parentOutput;
    }
    bridge.SetClientProcess (DynamoClient::Headless, (uint32_t) process.dwProcessId, process.hProcess);

    std::string line;
    while (ReadProtocolLine (parentOutput, process.hProcess, StartupTimeoutMs, line, error)) {
        GS::ObjectState response;
        if (JSON::ConvertToObjectState (GS::UniString (line.c_str (), CC_UTF8), response) != NoError) {
            error = "The Dynamo runner returned invalid startup JSON.";
            break;
        }
        GS::UniString type;
        GS::UniString message;
        response.Get ("type", type);
        response.Get ("message", message);
        if (type == "ready") {
            // The runner is READY either way — a graph that only reads Archicad data
            // runs perfectly well without a geometry library. But a missing ASM is
            // said HERE rather than left to surface as a LibG assembly error inside
            // the first geometry node, which names a file the user never asked for
            // and no version at all. See DynamoRunner/GeometryLibrary.cs.
            bool geometry = false;
            GS::UniString geometryMessage;
            response.Get ("geometry", geometry);
            response.Get ("geometryMessage", geometryMessage);
            if (!geometry && !geometryMessage.IsEmpty ())
                SetHeadlessState (HeadlessState::Ready, "Dynamo runner is ready. " + geometryMessage);
            else
                SetHeadlessState (HeadlessState::Ready, "Dynamo runner is ready.");
            return;
        }
        if (type == "error") {
            error = message.IsEmpty () ? GS::UniString ("The Dynamo runner failed during startup.") : message;
            break;
        }
    }

    if (!s_headlessStopping.load ()) {
        TerminateProcess (process.hProcess, 1);
        WaitForSingleObject (process.hProcess, ShutdownTimeoutMs);
        CloseHeadlessHandles ();
        SetHeadlessState (HeadlessState::Faulted, error);
    }
}

bool CopyPackageFile (const std::wstring& source, const std::wstring& destination, GS::UniString& error)
{
    if (!FileExists (source)) {
        error = GS::UniString::Printf ("The staged Dynamo package is incomplete: %T",
                                       GS::UniString (source.c_str ()).ToPrintf ());
        return false;
    }
    if (CopyFileW ((LPCWSTR) source.c_str (), (LPCWSTR) destination.c_str (), FALSE) == 0) {
        error = GS::UniString::Printf ("Could not install the Dynamo package file (Win32 error %u): %T",
                                       (unsigned) GetLastError (), GS::UniString (destination.c_str ()).ToPrintf ());
        return false;
    }
    return true;
}

bool InstallPackage (GS::UniString& error)
{
    std::wstring ownDirectory;
    if (!OwnDirectory (ownDirectory)) {
        error = "Could not resolve the Tapioca add-on directory for Dynamo package installation.";
        return false;
    }

    GS::UniString appData;
    if (!evp::ReadEnv (L"APPDATA", appData)) {
        error = "%APPDATA% is unavailable, so the Tapioca Dynamo package cannot be installed.";
        return false;
    }

    const std::wstring sourceRoot = ownDirectory + L"\\DynamoPackage\\Tapioca";
    const GS::UniString destinationRoot = appData + GS::UniString ("\\Dynamo\\Dynamo Core\\4.2\\packages\\Tapioca");
    const GS::UniString destinationBin = destinationRoot + GS::UniString ("\\bin");
    if (!evp::CreateDirectoryChain (destinationBin)) {
        error = "Could not create Dynamo's Tapioca package directory at " + destinationRoot;
        return false;
    }

    const std::wstring root ((const wchar_t*) destinationRoot.ToUStr ().Get ());
    const std::wstring bin ((const wchar_t*) destinationBin.ToUStr ().Get ());
    // ⚠️ NAMED, NOT GLOBBED, and every name here is also a name in pkg.json's
    // node_libraries. A copy loop over bin\* would quietly ship a stale assembly left
    // behind by an older build, and a package assembly Dynamo cannot load is a
    // library that silently loses nodes rather than an error anybody sees.
    //
    // The three assemblies are three because Dynamo forces it: ZeroTouch and NodeModel
    // cannot share one (DynamoModel.LoadNodeLibrary drops the ZeroTouch half), and the
    // WPF view cannot join the model (the headless runner has no Windows Desktop
    // framework). See Tapioca.DynamoNodes.csproj.
    static constexpr const wchar_t* const binaries[] = {
        L"Tapioca.Dynamo.dll",
        L"Tapioca.Dynamo_DynamoCustomization.xml",
        L"Tapioca.DynamoNodes.dll",
        L"Tapioca.DynamoNodesUI.dll",
    };

    if (!CopyPackageFile (sourceRoot + L"\\pkg.json", root + L"\\pkg.json", error) ||
        !CopyPackageFile (sourceRoot + L"\\TapiocaRoundTripTest.dyn", root + L"\\TapiocaRoundTripTest.dyn", error))
        return false;

    for (const wchar_t* const name : binaries) {
        if (!CopyPackageFile (sourceRoot + L"\\bin\\" + name, bin + L"\\" + name, error))
            return false;
    }
    return true;
}

BOOL CALLBACK FindProcessWindow (HWND window, LPARAM parameter)
{
    DWORD processId = 0;
    GetWindowThreadProcessId (window, &processId);
    if (processId != (DWORD) parameter || !IsWindowVisible (window) || GetWindow (window, GW_OWNER) != nullptr)
        return TRUE;

    if (IsIconic (window))
        ShowWindow (window, SW_RESTORE);
    SetForegroundWindow (window);
    return FALSE;
}

bool ActivateRunningEditor ()
{
    if (s_editorProcess == nullptr)
        return false;

    DWORD exitCode = 0;
    if (GetExitCodeProcess (s_editorProcess, &exitCode) == 0 || exitCode != STILL_ACTIVE) {
        CloseHandle (s_editorProcess);
        s_editorProcess = nullptr;
        s_editorProcessId = 0;
        return false;
    }

    EnumWindows (FindProcessWindow, (LPARAM) s_editorProcessId);
    return true;
}

bool StartEditor (GS::UniString& error)
{
    if (ActivateRunningEditor ())
        return true;

    std::wstring executable;
    std::wstring workingDirectory;
    if (!ResolveRuntime (executable, workingDirectory)) {
        error = "DynamoSandbox.exe was not found. Set TAPIOCA_DYNAMO_DIR to the DynamoCoreRuntime4.2.1.5887 "
                "folder, or stage that runtime as Dynamo beside Tapioca.apx. Dynamo 4 also requires the .NET 10 "
                "Windows Desktop Runtime.";
        return false;
    }
    if (!InstallPackage (error))
        return false;

    DynamoBridge& bridge = DynamoBridge::Get ();
    if (!bridge.Start (error))
        return false;

    GS::UniString previousPipe;
    const bool hadPreviousPipe = evp::ReadEnv (L"TAPIOCA_DYNAMO_PIPE", previousPipe);
    const GS::UniString pipeName = bridge.PipeName ();
    SetEnvironmentVariableW (L"TAPIOCA_DYNAMO_PIPE", (LPCWSTR) pipeName.ToUStr ().Get ());

    // TAPIOCA_DYNAMO_ASM_DIR is forwarded as --GeometryPath, so the editor and the
    // headless runner resolve the geometry library the same way. Only the OVERRIDE is
    // forwarded: with none set, Dynamo's own ASM search is already what runs here, and
    // second-guessing it from C++ would duplicate DynamoShapeManager badly. The
    // runner's probe (DynamoRunner/GeometryLibrary.cs) is what turns a failed search
    // into a sentence; the editor prints its own diagnostics to its console.
    std::wstring commandLine = L"\"" + executable + L"\" --NoNetworkMode";
    GS::UniString asmDirectory;
    if (evp::ReadEnv (L"TAPIOCA_DYNAMO_ASM_DIR", asmDirectory) && !asmDirectory.IsEmpty ())
        commandLine += L" --GeometryPath \"" + std::wstring ((const wchar_t*) asmDirectory.ToUStr ().Get ()) + L"\"";
    std::vector<wchar_t> mutableCommandLine (commandLine.begin (), commandLine.end ());
    mutableCommandLine.push_back (L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION process {};
    const BOOL created = CreateProcessW ((LPCWSTR) executable.c_str (), mutableCommandLine.data (), nullptr, nullptr,
                                         FALSE, 0, nullptr, (LPCWSTR) workingDirectory.c_str (), &startup, &process);
    SetEnvironmentVariableW (L"TAPIOCA_DYNAMO_PIPE",
                             hadPreviousPipe ? (LPCWSTR) previousPipe.ToUStr ().Get () : nullptr);
    if (created == 0) {
        error = GS::UniString::Printf ("Could not start DynamoSandbox.exe (Win32 error %u). Verify the .NET 10 "
                                       "Windows Desktop Runtime and the Dynamo runtime folder.",
                                       (unsigned) GetLastError ());
        return false;
    }

    bridge.SetClientProcess (DynamoClient::Editor, (uint32_t) process.dwProcessId, process.hProcess);
    CloseHandle (process.hThread);

    if (WaitForSingleObject (process.hProcess, 250) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess (process.hProcess, &exitCode);
        CloseHandle (process.hProcess);
        error = GS::UniString::Printf (
            "DynamoSandbox.exe exited during startup (code %u). Verify the .NET 10 Windows Desktop Runtime and "
            "the Dynamo runtime folder.",
            (unsigned) exitCode);
        return false;
    }

    s_editorProcess = process.hProcess;
    s_editorProcessId = process.dwProcessId;
    return true;
}

} // namespace

void OpenFromMenu ()
{
    GS::UniString error;
    if (StartEditor (error))
        return;

    const GS::UniString report = "The Dynamo editor did not open.\n\n" + error;
    ACAPI_WriteReport ("%T", true, report.ToPrintf ());
}

bool StartHeadless ()
{
    std::lock_guard<std::mutex> lifecycleLock (s_headlessLifecycleMutex);
    if (s_headlessStartupThread.joinable ()) {
        std::lock_guard<std::mutex> stateLock (s_headlessMutex);
        if (s_headlessState == HeadlessState::Starting || s_headlessState == HeadlessState::Ready ||
            s_headlessState == HeadlessState::Running)
            return true;
    }
    if (s_headlessStartupThread.joinable ())
        s_headlessStartupThread.join ();

    {
        std::lock_guard<std::mutex> stateLock (s_headlessMutex);
        if (s_headlessState == HeadlessState::Faulted)
            return false;
        s_headlessState = HeadlessState::Starting;
        s_headlessStatus = "Dynamo runner is starting.";
    }
    s_headlessStopping.store (false);
    try {
        s_headlessStartupThread = std::thread (HeadlessStartup);
    }
    catch (...) {
        SetHeadlessState (HeadlessState::Faulted, "Could not create the Dynamo runner startup thread.");
        return false;
    }
    return true;
}

GS::UniString HeadlessStatusText ()
{
    DWORD exitCode = STILL_ACTIVE;
    if (HeadlessProcessExited (exitCode))
        SetHeadlessState (HeadlessState::Faulted,
                          GS::UniString::Printf ("The Dynamo runner exited with code %u.", (unsigned) exitCode));
    std::lock_guard<std::mutex> lock (s_headlessMutex);
    return s_headlessStatus;
}

bool IsHeadlessReady ()
{
    DWORD exitCode = STILL_ACTIVE;
    if (HeadlessProcessExited (exitCode)) {
        SetHeadlessState (HeadlessState::Faulted,
                          GS::UniString::Printf ("The Dynamo runner exited with code %u.", (unsigned) exitCode));
        return false;
    }
    std::lock_guard<std::mutex> lock (s_headlessMutex);
    return s_headlessState == HeadlessState::Ready;
}

bool RunHeadlessGraph (const GS::UniString& graphPath, const GS::UniString& paramsJson, uint64_t generation,
                       GS::UniString& result, GS::UniString& error)
{
    result.Clear ();
    error.Clear ();
    std::unique_lock<std::mutex> runLock (s_headlessRunMutex, std::try_to_lock);
    if (!runLock.owns_lock ()) {
        error = "Another Dynamo graph is already running.";
        return false;
    }
    if (generation > (uint64_t) std::numeric_limits<GS::Int64>::max ()) {
        error = "The Dynamo graph generation is outside the supported integer range.";
        return false;
    }

    GS::ObjectState inputs;
    if (paramsJson.IsEmpty () || JSON::ConvertToObjectState (paramsJson, inputs) != NoError) {
        error = "Dynamo graph inputs must be a JSON object.";
        return false;
    }

    HANDLE process = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    {
        std::lock_guard<std::mutex> lock (s_headlessMutex);
        if (s_headlessState != HeadlessState::Ready || s_headlessProcess == nullptr) {
            error = s_headlessStatus;
            return false;
        }
        s_headlessState = HeadlessState::Running;
        s_headlessStatus = "Dynamo graph is running.";
        process = s_headlessProcess;
        input = s_headlessInput;
        output = s_headlessOutput;
    }

    GS::ObjectState request;
    request.Add ("operation", "run");
    request.Add ("generation", (GS::Int64) generation);
    request.Add ("path", graphPath);
    request.Add ("inputs", inputs);
    GS::UniString requestJson;
    if (JSON::CreateFromObjectState (request, requestJson) != NoError) {
        error = "Could not serialize the Dynamo graph request.";
        SetHeadlessState (HeadlessState::Ready, "Dynamo runner is ready.");
        return false;
    }
    const auto requestUtf8 = requestJson.ToCStr (0, MaxUSize, CC_UTF8);
    if (!WriteProtocolLine (input, process, requestUtf8.Get (), error)) {
        SetHeadlessState (HeadlessState::Faulted, error);
        return false;
    }

    const ULONGLONG deadline = GetTickCount64 () + RunTimeoutMs;
    while (true) {
        const ULONGLONG now = GetTickCount64 ();
        if (now >= deadline) {
            error = "Timed out waiting for the Dynamo graph result.";
            SetHeadlessState (HeadlessState::Faulted, error);
            return false;
        }
        std::string line;
        if (!ReadProtocolLine (output, process, deadline - now, line, error)) {
            SetHeadlessState (HeadlessState::Faulted, error);
            return false;
        }
        GS::ObjectState response;
        if (JSON::ConvertToObjectState (GS::UniString (line.c_str (), CC_UTF8), response) != NoError) {
            error = "The Dynamo runner returned invalid result JSON.";
            SetHeadlessState (HeadlessState::Faulted, error);
            return false;
        }

        GS::UniString type;
        GS::UniString message;
        GS::Int64 responseGeneration = -1;
        response.Get ("type", type);
        response.Get ("message", message);
        response.Get ("generation", responseGeneration);
        if (type == "error") {
            error = message.IsEmpty () ? GS::UniString ("The Dynamo runner rejected the request.") : message;
            SetHeadlessState (HeadlessState::Ready, "Dynamo runner is ready.");
            return false;
        }
        if (type != "result" || responseGeneration != (GS::Int64) generation)
            continue;

        bool success = false;
        response.Get ("success", success);
        SetHeadlessState (HeadlessState::Ready, "Dynamo runner is ready.");
        if (success) {
            result = message;
            return true;
        }
        error = message.IsEmpty () ? GS::UniString ("The Dynamo graph failed.") : message;
        return false;
    }
}

void Release ()
{
    std::lock_guard<std::mutex> lifecycleLock (s_headlessLifecycleMutex);
    s_headlessStopping.store (true);
    if (s_headlessStartupThread.joinable ())
        s_headlessStartupThread.join ();

    std::lock_guard<std::mutex> runLock (s_headlessRunMutex);
    HANDLE process = nullptr;
    HANDLE input = nullptr;
    {
        std::lock_guard<std::mutex> stateLock (s_headlessMutex);
        process = s_headlessProcess;
        input = s_headlessInput;
    }
    if (process != nullptr && WaitForSingleObject (process, 0) == WAIT_TIMEOUT) {
        GS::UniString ignored;
        WriteProtocolLine (input, process, "{\"operation\":\"shutdown\"}", ignored, false);
        if (WaitForSingleObject (process, ShutdownTimeoutMs) == WAIT_TIMEOUT) {
            TerminateProcess (process, 1);
            WaitForSingleObject (process, ShutdownTimeoutMs);
        }
    }
    DynamoBridge::Get ().Stop ();
    CloseHeadlessHandles ();
    {
        std::lock_guard<std::mutex> stateLock (s_headlessMutex);
        s_headlessState = HeadlessState::Stopped;
        s_headlessStatus = "Dynamo runner is stopped.";
    }

    if (s_editorProcess != nullptr)
        CloseHandle (s_editorProcess);
    s_editorProcess = nullptr;
    s_editorProcessId = 0;
}

} // namespace evp::dynamo
