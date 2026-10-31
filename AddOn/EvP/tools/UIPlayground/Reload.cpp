#include "Reload.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace tapioca::playground {
namespace fs = std::filesystem;
namespace {
void* Allocate (size_t bytes)
{
    return std::malloc (bytes);
}
void Release (void* memory)
{
    std::free (memory);
}
std::string WindowsError ()
{
    return "Win32 error " + std::to_string (GetLastError ());
}
} // namespace
std::string ReadText (const fs::path& path)
{
    std::ifstream stream (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}
Module::~Module ()
{
    Clear ();
}
void Module::Clear ()
{
    if (instance && destroy)
        destroy (instance);
    instance = nullptr;
    if (library)
        FreeLibrary (library);
    library = nullptr;
    destroy = nullptr;
    draw = nullptr;
}
bool Module::Swap (const fs::path& source, const std::string& state, std::string& error)
{
    error.clear ();
    const auto folder = source.parent_path () / "modules" / std::to_string (GetCurrentProcessId ());
    std::error_code ec;
    fs::create_directories (folder, ec);
    const auto candidatePath = folder / ("TapiocaUIPlaygroundUI-" + std::to_string (++attempt) + ".dll");
    if (ec || !fs::copy_file (source, candidatePath, fs::copy_options::overwrite_existing, ec)) {
        error = "Cannot stage UI DLL: " + ec.message ();
        return false;
    }
    HMODULE candidate = LoadLibraryExW (candidatePath.c_str (), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!candidate) {
        error = "Cannot load staged UI DLL: " + WindowsError ();
        fs::remove (candidatePath, ec);
        return false;
    }
    const auto version =
        reinterpret_cast<decltype (&TapiocaUiAbiVersion)> (GetProcAddress (candidate, "TapiocaUiAbiVersion"));
    const auto create = reinterpret_cast<decltype (&TapiocaUiCreate)> (GetProcAddress (candidate, "TapiocaUiCreate"));
    const auto candidateDestroy =
        reinterpret_cast<decltype (&TapiocaUiDestroy)> (GetProcAddress (candidate, "TapiocaUiDestroy"));
    const auto candidateDraw =
        reinterpret_cast<decltype (&TapiocaUiDraw)> (GetProcAddress (candidate, "TapiocaUiDraw"));
    const TapiocaUiHost host { sizeof (TapiocaUiHost), Allocate, Release };
    void* created = nullptr;
    if (version && create && candidateDestroy && candidateDraw && version () == TAPIOCA_UI_ABI_VERSION)
        created = create (&host, state.c_str ());
    if (!created) {
        error = "Candidate rejected (missing export, ABI mismatch, or state/Create failure); previous UI retained.";
        FreeLibrary (candidate);
        fs::remove (candidatePath, ec);
        return false;
    }
    // Transactional replacement: no previous object/code is released until its
    // replacement is ready. Called only after the previous frame was rendered.
    Clear ();
    library = candidate;
    instance = created;
    destroy = candidateDestroy;
    draw = candidateDraw;
    activePath = candidatePath;
    ++generation;
    std::vector<fs::path> copies;
    for (fs::directory_iterator it (folder, ec), end; !ec && it != end; it.increment (ec))
        if (it->path ().extension () == ".dll")
            copies.push_back (it->path ());
    std::sort (copies.begin (), copies.end (), [] (const auto& a, const auto& b) {
        const auto sequence = [] (const auto& path) {
            const auto name = path.stem ().string ();
            return std::stoull (name.substr (name.find_last_of ('-') + 1));
        };
        return sequence (a) > sequence (b);
    });
    for (size_t i = 5; i < copies.size (); ++i)
        if (copies[i] != activePath)
            fs::remove (copies[i], ec);
    return true;
}
bool Module::Draw (const TapiocaUiFrame& frame, TapiocaUiResult& result)
{
    result = {};
    return instance && draw && draw (instance, &frame, &result) != 0;
}
ReloadLoop::ReloadLoop (fs::path sourcePath, fs::path buildPath, fs::path cmakePath, std::wstring configuration)
    : source (std::move (sourcePath)), build (std::move (buildPath)), cmake (std::move (cmakePath)),
      config (std::move (configuration)), files (Scan ())
{
}
std::map<fs::path, fs::file_time_type> ReloadLoop::Scan () const
{
    std::map<fs::path, fs::file_time_type> result;
    const auto add = [&] (const fs::path& path) {
        std::error_code ec;
        const auto time = fs::last_write_time (path, ec);
        if (!ec)
            result[path] = time;
    };
    const auto tree = [&] (const fs::path& root) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it (root, ec), end; !ec && it != end; it.increment (ec)) {
            if (it->is_directory (ec)) {
                const auto name = it->path ().filename ().wstring ();
                if (name == L"build" || name.starts_with (L"build-") || name == L"modules")
                    it.disable_recursion_pending ();
                continue;
            }
            const auto ext = it->path ().extension ().wstring ();
            if (ext == L".cpp" || ext == L".hpp" || ext == L".h" || ext == L".cmake" ||
                it->path ().filename () == L"CMakeLists.txt")
                add (it->path ());
        }
    };
    tree (source);
    const auto addon = source.parent_path ().parent_path () / "Sources" / "AddOn";
    tree (addon / "ArchViz" / "TapiocaUI");
    for (const auto* folder : { L"ArchViz", L"Metadata", L"NodeGraph" }) {
        std::error_code ec;
        for (fs::directory_iterator it (addon / folder, ec), end; !ec && it != end; it.increment (ec)) {
            const auto name = it->path ().filename ().wstring ();
            if ((std::wstring (folder) != L"ArchViz" || it->path ().extension () == L".hpp" ||
                 it->path ().extension () == L".h" || name.starts_with (L"HudShell") ||
                 name.starts_with (L"HudMassingStats") || name.starts_with (L"MassingUsage") ||
                 name.starts_with (L"MassingAreaCalculations") || name.starts_with (L"HudFloorProgramme") ||
                 name.starts_with (L"FloorProgramme") || name.starts_with (L"GraphicsSettings") ||
                 name.starts_with (L"OverlayLayers") || name.starts_with (L"OverlayHitMap")) &&
                it->is_regular_file (ec))
                add (it->path ());
        }
    }
    return result;
}
void ReloadLoop::StartBuild ()
{
    pending = false;
    buildingChange = changed;
    status = "Building UI DLL in background; previous UI continues rendering...";
    const auto exe = cmake;
    const auto directory = build;
    const auto configuration = config;
    running = std::async (std::launch::async, [exe, directory, configuration] {
        BuildResult result;
        const auto start = std::chrono::steady_clock::now ();
        const auto log = directory / "playground-build.log";
        SECURITY_ATTRIBUTES security { sizeof (SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE output = CreateFileW (log.c_str (), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) {
            result.output = "Cannot create build log: " + WindowsError ();
            return result;
        }
        HANDLE input = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup { sizeof (STARTUPINFOW) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = input;
        PROCESS_INFORMATION process {};
        std::wstring command = L"\"" + exe.wstring () + L"\" --build \"" + directory.wstring () +
                               L"\" --target TapiocaUIPlaygroundUI --config \"" + configuration + L"\" --parallel 4";
        const bool launched = CreateProcessW (exe.c_str (), command.data (), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                              nullptr, directory.c_str (), &startup, &process) != FALSE;
        const auto launchError = GetLastError ();
        CloseHandle (output);
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle (input);
        if (launched) {
            WaitForSingleObject (process.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess (process.hProcess, &code);
            result.success = code == 0;
            CloseHandle (process.hThread);
            CloseHandle (process.hProcess);
        }
        else
            result.output = "CreateProcess failed: " + std::to_string (launchError) + "\n";
        result.output += ReadText (log);
        if (result.output.size () > 16000)
            result.output.erase (0, result.output.size () - 16000);
        result.milliseconds =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
        return result;
    });
}
bool ReloadLoop::Tick (bool requestBuild)
{
    const auto now = std::chrono::steady_clock::now ();
    if (now >= nextScan) {
        const auto current = Scan ();
        if (files != current) {
            files = current;
            pending = true;
            changed = now;
        }
        nextScan = now + std::chrono::milliseconds (250);
    }
    if (requestBuild) {
        pending = true;
        changed = now - std::chrono::milliseconds (400);
    }
    bool built = false;
    if (running.valid () && running.wait_for (std::chrono::seconds (0)) == std::future_status::ready) {
        const auto result = running.get ();
        built = result.success;
        lastLatencyMs = std::chrono::duration<double, std::milli> (now - buildingChange).count ();
        status = (built ? "Build succeeded in " : "Build FAILED; last working UI retained. Build took ") +
                 std::to_string (result.milliseconds) + " ms.\n" + result.output;
    }
    if (!running.valid () && pending && now - changed >= std::chrono::milliseconds (400))
        StartBuild ();
    return built;
}
} // namespace tapioca::playground
