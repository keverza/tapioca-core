#include "APIEnvir.h"
#include "ACAPinc.h"

#include "DynamoHost.hpp"
#include "DynamoBridge.hpp"
#include "Python/PathUtils.hpp"

#include <string>
#include <vector>

namespace evp::dynamo {
namespace {

constexpr const wchar_t* RuntimeDirectoryName = L"DynamoCoreRuntime4.2.1.5887";
constexpr const wchar_t* SandboxName = L"DynamoSandbox.exe";

HANDLE s_process = nullptr;
DWORD s_processId = 0;

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

bool ResolveSandbox (std::wstring& executable, std::wstring& workingDirectory)
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
        if (candidate.size () < wcslen (SandboxName) ||
            _wcsicmp (candidate.c_str () + candidate.size () - wcslen (SandboxName), SandboxName) != 0)
            candidate += L"\\" + std::wstring (SandboxName);
        if (!FileExists (candidate))
            continue;

        executable = candidate;
        workingDirectory = ParentDirectory (candidate);
        return true;
    }
    return false;
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
    return CopyPackageFile (sourceRoot + L"\\pkg.json", root + L"\\pkg.json", error) &&
           CopyPackageFile (sourceRoot + L"\\bin\\Tapioca.Dynamo.dll", bin + L"\\Tapioca.Dynamo.dll", error);
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
    if (s_process == nullptr)
        return false;

    DWORD exitCode = 0;
    if (GetExitCodeProcess (s_process, &exitCode) == 0 || exitCode != STILL_ACTIVE) {
        CloseHandle (s_process);
        s_process = nullptr;
        s_processId = 0;
        return false;
    }

    EnumWindows (FindProcessWindow, (LPARAM) s_processId);
    return true;
}

bool StartEditor (GS::UniString& error)
{
    if (ActivateRunningEditor ())
        return true;

    std::wstring executable;
    std::wstring workingDirectory;
    if (!ResolveSandbox (executable, workingDirectory)) {
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

    std::wstring commandLine = L"\"" + executable + L"\" --NoNetworkMode";
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
        bridge.Stop ();
        error = GS::UniString::Printf ("Could not start DynamoSandbox.exe (Win32 error %u). Verify the .NET 10 "
                                       "Windows Desktop Runtime and the Dynamo runtime folder.",
                                       (unsigned) GetLastError ());
        return false;
    }

    bridge.SetClientProcess ((uint32_t) process.dwProcessId, process.hProcess);
    CloseHandle (process.hThread);

    if (WaitForSingleObject (process.hProcess, 250) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess (process.hProcess, &exitCode);
        CloseHandle (process.hProcess);
        bridge.Stop ();
        error = GS::UniString::Printf (
            "DynamoSandbox.exe exited during startup (code %u). Verify the .NET 10 Windows Desktop Runtime and "
            "the Dynamo runtime folder.",
            (unsigned) exitCode);
        return false;
    }

    s_process = process.hProcess;
    s_processId = process.dwProcessId;
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

void Release ()
{
    DynamoBridge::Get ().Stop ();
    if (s_process != nullptr)
        CloseHandle (s_process);
    s_process = nullptr;
    s_processId = 0;
}

} // namespace evp::dynamo
