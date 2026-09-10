#include "GhWorkerLocate.hpp"

#include "Python/PathUtils.hpp" // ReadEnv

#include <vector>

namespace evp {
namespace grasshopper {
namespace locate {

namespace {

// The worker's staged folder and file name, as the build writes them.
constexpr const wchar_t* WorkerFolderName = L"GhWorker";
constexpr const wchar_t* WorkerExecutableName = L"Tapioca.GhWorker.exe";

// Fifteen seconds. Long enough that a slow component does not look wedged,
// short enough that a wedged one is noticed while the user is still watching.
constexpr uint64_t DefaultHeartbeatDeadlineMs = 15000;

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

} // namespace

GS::UniString FromWide (const std::wstring& text)
{
    if (text.empty ())
        return GS::UniString ();
    return GS::UniString (text.c_str ());
}

GS::UniString FromUtf8Std (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
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

} // namespace locate
} // namespace grasshopper
} // namespace evp
