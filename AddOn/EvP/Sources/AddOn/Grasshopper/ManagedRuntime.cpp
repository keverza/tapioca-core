#include "APIEnvir.h" // Win32Interface only — no ACAPinc.h here, on purpose
#include "ManagedRuntime.hpp"
#include "HostFxrApi.h"

#include <vector>

namespace evp {
namespace grasshopper {

namespace {

// hostfxr reports its own failures through a writer callback, and the return
// code alone is a bare hex number ("0x80008083"). The writer is what turns that
// into "The framework 'Microsoft.WindowsDesktop.App', version '8.0.0' was not
// found" — the difference between an actionable diagnostic and a support ticket.
// One global because hostfxr_set_error_writer is itself process-global.
std::wstring lastHostFxrError;

void EVP_HOSTFXR_CALLTYPE CaptureHostFxrError (const evp_char_t* message)
{
    if (message == nullptr)
        return;
    if (!lastHostFxrError.empty ())
        lastHostFxrError += L"; ";
    lastHostFxrError += (const wchar_t*) message;
}

bool ReadEnvironment (const wchar_t* name, std::wstring& value)
{
    std::vector<wchar_t> buffer (512);
    DWORD written = GetEnvironmentVariableW ((LPCWSTR) name, (LPWSTR) buffer.data (), (DWORD) buffer.size ());
    if (written == 0)
        return false;
    if (written >= buffer.size ()) {
        buffer.resize (written + 1);
        written = GetEnvironmentVariableW ((LPCWSTR) name, (LPWSTR) buffer.data (), (DWORD) buffer.size ());
        if (written == 0)
            return false;
    }
    value.assign (buffer.data (), written);
    return true;
}

bool ReadRegistryString (HKEY root, const wchar_t* subKey, const wchar_t* valueName, std::wstring& value)
{
    HKEY key = nullptr;
    // KEY_WOW64_64KEY: the .apx is 64-bit, but saying so explicitly is free and
    // survives ever being built any other way.
    if (RegOpenKeyExW (root, (LPCWSTR) subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buffer[MAX_PATH] = { 0 };
    DWORD size = sizeof (buffer);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW (key, (LPCWSTR) valueName, nullptr, &type, (LPBYTE) buffer, &size);
    RegCloseKey (key);
    if (status != ERROR_SUCCESS || type != REG_SZ)
        return false;
    value = buffer;
    return true;
}

bool DirectoryExists (const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW ((LPCWSTR) path.c_str ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists (const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW ((LPCWSTR) path.c_str ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Every place a .NET installation is allowed to be, most specific first. The
// order matters: DOTNET_ROOT is how a developer points at a private install,
// and it must win over the machine one.
void CollectDotnetRoots (std::vector<std::wstring>& roots)
{
    std::wstring value;
    if (ReadEnvironment (L"DOTNET_ROOT", value) && !value.empty ())
        roots.push_back (value);
    if (ReadRegistryString (HKEY_LOCAL_MACHINE, L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x64", L"InstallLocation",
                            value) &&
        !value.empty ())
        roots.push_back (value);
    if (ReadEnvironment (L"ProgramFiles", value) && !value.empty ())
        roots.push_back (value + L"\\dotnet");
}

// "10.0.11" against "8.0.14" — compare component by component. Lexical order
// gets this wrong the moment a two-digit component appears, which it now has.
bool IsNewerVersion (const std::wstring& candidate, const std::wstring& current)
{
    size_t a = 0;
    size_t b = 0;
    while (a < candidate.size () || b < current.size ()) {
        long left = 0;
        long right = 0;
        while (a < candidate.size () && candidate[a] >= L'0' && candidate[a] <= L'9')
            left = left * 10 + (candidate[a++] - L'0');
        while (b < current.size () && current[b] >= L'0' && current[b] <= L'9')
            right = right * 10 + (current[b++] - L'0');
        if (left != right)
            return left > right;
        if (a < candidate.size ())
            ++a;
        if (b < current.size ())
            ++b;
    }
    return false;
}

long MajorVersion (const std::wstring& version)
{
    long major = 0;
    for (size_t i = 0; i < version.size () && version[i] >= L'0' && version[i] <= L'9'; ++i)
        major = major * 10 + (version[i] - L'0');
    return major;
}

// The lowest hostfxr that can serve a net8.0 application. Older ones exist on
// developer machines and would fail later, with a worse message.
const long MinimumHostFxrMajor = 8;

} // namespace

bool FindHostFxr (std::wstring& path, std::wstring& error)
{
    std::vector<std::wstring> roots;
    CollectDotnetRoots (roots);

    std::wstring searched;
    std::wstring bestVersion;
    std::wstring bestPath;

    for (size_t index = 0; index < roots.size (); ++index) {
        const std::wstring fxrDir = roots[index] + L"\\host\\fxr";
        if (!searched.empty ())
            searched += L", ";
        searched += fxrDir;
        if (!DirectoryExists (fxrDir))
            continue;

        WIN32_FIND_DATAW found = { 0 };
        const std::wstring pattern = fxrDir + L"\\*";
        HANDLE handle = FindFirstFileW ((LPCWSTR) pattern.c_str (), &found);
        if (handle == INVALID_HANDLE_VALUE)
            continue;
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            const std::wstring name = (const wchar_t*) found.cFileName;
            if (name == L"." || name == L"..")
                continue;
            if (MajorVersion (name) < MinimumHostFxrMajor)
                continue;
            const std::wstring candidate = fxrDir + L"\\" + name + L"\\hostfxr.dll";
            if (!FileExists (candidate))
                continue;
            if (bestVersion.empty () || IsNewerVersion (name, bestVersion)) {
                bestVersion = name;
                bestPath = candidate;
            }
        } while (FindNextFileW (handle, &found) != FALSE);
        FindClose (handle);
    }

    if (bestPath.empty ()) {
        error = L"No .NET 8 or newer runtime host was found. Looked in: " + searched +
                L". Install the .NET Desktop Runtime 8 (x64), or set DOTNET_ROOT.";
        return false;
    }
    path = bestPath;
    return true;
}

bool ManagedRuntime::Start (const std::wstring& runtimeConfigPath, std::wstring& error)
{
    if (IsLoaded ())
        return true;

    if (!FileExists (runtimeConfigPath)) {
        error = L"The managed host's runtime configuration is missing: " + runtimeConfigPath +
                L". Rebuild and redeploy the add-on; command sync cannot deploy managed assemblies.";
        return false;
    }

    std::wstring hostFxrPath;
    if (!FindHostFxr (hostFxrPath, error))
        return false;

    // Full path, and no PATH mutation anywhere in this file — the handoff's
    // build rules forbid discovery by PATH, and LOAD_WITH_ALTERED_SEARCH_PATH
    // keeps hostfxr's own neighbours resolving out of its own directory.
    HMODULE module = LoadLibraryExW ((LPCWSTR) hostFxrPath.c_str (), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        error = L"hostfxr.dll could not be loaded from " + hostFxrPath;
        return false;
    }

    const evp_hostfxr_initialize_for_runtime_config_fn initialize =
        (evp_hostfxr_initialize_for_runtime_config_fn) GetProcAddress (module, "hostfxr_initialize_for_runtime_config");
    const evp_hostfxr_get_runtime_delegate_fn getDelegate =
        (evp_hostfxr_get_runtime_delegate_fn) GetProcAddress (module, "hostfxr_get_runtime_delegate");
    const evp_hostfxr_close_fn close = (evp_hostfxr_close_fn) GetProcAddress (module, "hostfxr_close");
    const evp_hostfxr_set_error_writer_fn setErrorWriter =
        (evp_hostfxr_set_error_writer_fn) GetProcAddress (module, "hostfxr_set_error_writer");
    if (initialize == nullptr || getDelegate == nullptr || close == nullptr) {
        FreeLibrary (module);
        error = L"hostfxr.dll at " + hostFxrPath + L" does not export the .NET hosting entry points.";
        return false;
    }

    lastHostFxrError.clear ();
    if (setErrorWriter != nullptr)
        setErrorWriter (CaptureHostFxrError);

    void* context = nullptr;
    const int32_t initStatus = initialize ((const evp_char_t*) runtimeConfigPath.c_str (), nullptr, &context);
    if (initStatus < 0 || context == nullptr) {
        if (setErrorWriter != nullptr)
            setErrorWriter (nullptr);
        if (context != nullptr)
            close (context);
        FreeLibrary (module);
        error = L"The .NET runtime could not be initialized for " + runtimeConfigPath + L".";
        if (!lastHostFxrError.empty ())
            error += L" " + lastHostFxrError;
        return false;
    }

    void* delegatePtr = nullptr;
    const int32_t delegateStatus = getDelegate (context, EVP_HDT_LOAD_ASSEMBLY_AND_GET_FUNCTION_POINTER, &delegatePtr);
    if (delegateStatus < 0 || delegatePtr == nullptr) {
        if (setErrorWriter != nullptr)
            setErrorWriter (nullptr);
        close (context);
        FreeLibrary (module);
        error = L"The .NET runtime started but would not hand out a load-assembly delegate.";
        if (!lastHostFxrError.empty ())
            error += L" " + lastHostFxrError;
        return false;
    }

    hostFxrModule = module;
    hostContext = context;
    loadAssemblyAndGetFunctionPointer = delegatePtr;

    description = L"hostfxr " + hostFxrPath;
    if (initStatus == EVP_HOSTFXR_SUCCESS_HOST_ALREADY_INITIALIZED)
        description += L" (a .NET runtime was already running in this process)";
    else if (initStatus == EVP_HOSTFXR_SUCCESS_DIFFERENT_RUNTIME_PROPERTIES)
        description += L" (a .NET runtime was already running with different properties)";
    return true;
}

bool ManagedRuntime::Resolve (const std::wstring& assemblyPath, const wchar_t* typeName, const wchar_t* methodName,
                              void** functionPointer, std::wstring& error)
{
    if (functionPointer == nullptr)
        return false;
    *functionPointer = nullptr;
    if (!IsLoaded ()) {
        error = L"The .NET runtime is not loaded.";
        return false;
    }
    if (!FileExists (assemblyPath)) {
        error = L"The managed host assembly is missing: " + assemblyPath +
                L". Rebuild and redeploy the add-on; command sync cannot deploy managed assemblies.";
        return false;
    }

    lastHostFxrError.clear ();
    const evp_load_assembly_and_get_function_pointer_fn load =
        (evp_load_assembly_and_get_function_pointer_fn) loadAssemblyAndGetFunctionPointer;
    const int32_t status =
        load ((const evp_char_t*) assemblyPath.c_str (), (const evp_char_t*) typeName, (const evp_char_t*) methodName,
              EVP_UNMANAGEDCALLERSONLY_METHOD, nullptr, functionPointer);
    if (status < 0 || *functionPointer == nullptr) {
        *functionPointer = nullptr;
        error = std::wstring (L"The managed entry point ") + methodName + L" could not be resolved in " + assemblyPath +
                L".";
        if (!lastHostFxrError.empty ())
            error += L" " + lastHostFxrError;
        return false;
    }
    return true;
}

void ManagedRuntime::Close ()
{
    if (hostFxrModule != nullptr) {
        const evp_hostfxr_set_error_writer_fn setErrorWriter =
            (evp_hostfxr_set_error_writer_fn) GetProcAddress ((HMODULE) hostFxrModule, "hostfxr_set_error_writer");
        // ⚠️ FIRST, AND UNCONDITIONALLY. CaptureHostFxrError lives in THIS DLL,
        // and hostfxr keeps the pointer process-wide. Leaving it installed past
        // an .apx unload is hostfxr calling into freed code — the same hazard as
        // every Win32 hook in AddOnMain's FreeData, arriving from a runtime that
        // outlives us by design.
        if (setErrorWriter != nullptr)
            setErrorWriter (nullptr);
        if (hostContext != nullptr) {
            const evp_hostfxr_close_fn close =
                (evp_hostfxr_close_fn) GetProcAddress ((HMODULE) hostFxrModule, "hostfxr_close");
            if (close != nullptr)
                close (hostContext);
        }
    }
    hostContext = nullptr;
    loadAssemblyAndGetFunctionPointer = nullptr;
    // hostfxr.dll itself is NOT freed. The CLR it started is still in the
    // process and holds references into it; unloading the host library out from
    // under a live runtime is not something the hosting ABI supports.
    hostFxrModule = nullptr;
}

bool ManagedRuntime::IsLoaded () const
{
    return loadAssemblyAndGetFunctionPointer != nullptr;
}

const std::wstring& ManagedRuntime::Description () const
{
    return description;
}

} // namespace grasshopper
} // namespace evp
