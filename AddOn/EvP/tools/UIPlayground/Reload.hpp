#pragma once
#include "UiAbi.h"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <string>

namespace tapioca::playground {
class Module {
  public:
    Module () = default;
    ~Module ();
    Module (const Module&) = delete;
    Module& operator= (const Module&) = delete;
    bool Swap (const std::filesystem::path& source, const std::string& state, std::string& error);
    bool Draw (const TapiocaUiFrame& frame, TapiocaUiResult& result);
    bool Ready () const
    {
        return library != nullptr;
    }
    uint64_t Generation () const
    {
        return generation;
    }

  private:
    void Clear ();
    HMODULE library = nullptr;
    void* instance = nullptr;
    decltype (&TapiocaUiDestroy) destroy = nullptr;
    decltype (&TapiocaUiDraw) draw = nullptr;
    std::filesystem::path activePath;
    uint64_t generation = 0, attempt = 0;
};

class ReloadLoop {
  public:
    ReloadLoop (std::filesystem::path source, std::filesystem::path build, std::filesystem::path cmake,
                std::wstring config);
    // Returns true only after a successful single-flight build, at a frame boundary.
    bool Tick (bool requestBuild);
    const std::string& Status () const
    {
        return status;
    }
    double LastLatencyMs () const
    {
        return lastLatencyMs;
    }

  private:
    struct BuildResult {
        bool success = false;
        std::string output;
        double milliseconds = 0;
    };
    std::map<std::filesystem::path, std::filesystem::file_time_type> Scan () const;
    void StartBuild ();
    std::filesystem::path source, build, cmake;
    std::wstring config;
    std::map<std::filesystem::path, std::filesystem::file_time_type> files;
    std::future<BuildResult> running;
    std::chrono::steady_clock::time_point nextScan {}, changed {}, buildingChange {};
    bool pending = false;
    double lastLatencyMs = 0;
    std::string status = "Watching UI sources. Save C++ to rebuild, or press Build UI DLL now.";
};
std::string ReadText (const std::filesystem::path& path);
} // namespace tapioca::playground
