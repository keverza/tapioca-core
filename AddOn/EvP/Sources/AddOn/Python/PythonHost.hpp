#ifndef EVP_PYTHONHOST_HPP
#define EVP_PYTHONHOST_HPP

// P0 embed spike — see archive/docs/evp-command-system-plan.md.
//
// The .apx side of the Python bridge. Deliberately contains NO CPython C API:
// it resolves the runtime, loads python312.dll and EvPPy.dll by full path, and
// drives them through the extern "C" interface in EvPPy/EvPPyApi.h (which
// explains why the split exists). Everything Python happens over there.
//
// Lifetime rules this enforces, both from the plan:
//   * the interpreter initializes ONCE, lazily, and is never finalized —
//     re-init in the same process corrupts numpy and friends;
//   * per-run freshness therefore comes from namespace isolation instead, which
//     EvPPy.dll's runner implements.
//
// P0 runs everything on the main thread (menu handler). P1 moves execution to a
// worker (Zone B) and introduces the MainThreadGate.

#include "UniString.hpp"

namespace evp {

class PythonHost {
  public:
    static PythonHost& Get ();

    // Resolves the runtime, loads both DLLs and initializes the interpreter.
    // Safe to call repeatedly; only the first call does work.
    bool EnsureInitialized (GS::UniString& error);
    bool IsInitialized () const
    {
        return initialized;
    }

    // Executes a .py file in a fresh namespace under `moduleName`.
    // stdout/stderr land in the report window; a Python exception prints its
    // traceback there, sets `error` and returns false.
    bool RunScriptFile (const GS::UniString& path, const GS::UniString& moduleName, GS::UniString& error);

    // Directory python312.dll was loaded from — also the interpreter's home, and
    // where Zone C finds python.exe.
    const GS::UniString& GetRuntimeHome () const
    {
        return runtimeHome;
    }

    // Where the .apx, EvPPy.dll and the staged `evp` package live. Zone C puts
    // <ownDir>\PyPackage on the subprocess's PYTHONPATH so both zones import the
    // very same package — a second copy would be a second source of truth.
    const GS::UniString& GetOwnDir () const
    {
        return ownDir;
    }

    // Scans a scripts root and returns the scanner's JSON:
    //   {"root":..., "commands":[...], "diagnostics":[...]}
    // Never executes anything in the root — see evp/_scanner.py.
    bool ScanCommands (const GS::UniString& root, GS::UniString& json, GS::UniString& error);

    // Runs a command folder's run(**params). `paramsJson` comes from the
    // generated dialog. Safe from any thread once initialized (takes the GIL).
    //
    // E9 — `cancelled` comes back true when run() unwound on evp.Cancelled (Stop,
    // panel close, or timeout_s). That is a SUCCESSFUL return, not a failure: the
    // command did what it was told. `error` stays empty in that case, and the
    // caller reports "cancelled" rather than "FAILED".
    // `action` empty runs the command; a name runs one of its declared output
    // actions against the last run's stored result instead.
    // `menuRegion` is set only when that action came from the palette's
    // right-click menu, and says where the click landed — the command reads it as
    // `ctx.region`. A separate parameter rather than a key in paramsJson, on the
    // same reasoning as `action`: paramsJson is the user's values and nothing else
    // should ever have to be filtered back out of it.
    bool RunCommand (const GS::UniString& path, const GS::UniString& moduleName, const GS::UniString& paramsJson,
                     const GS::UniString& action, const GS::UniString& menuRegion, bool& cancelled,
                     GS::UniString& error);

  private:
    PythonHost () = default;

    bool initialized = false;
    GS::UniString runtimeHome;
    GS::UniString ownDir;      // where the .apx (and EvPPy.dll, and the evp package) live
    void* pythonDll = nullptr; // HMODULE; both stay loaded for process life,
    void* bridgeDll = nullptr; // because the interpreter is never finalized
    void* initializeFn = nullptr;
    void* runScriptFn = nullptr;
    void* setApiCallFn = nullptr;
    void* addSysPathFn = nullptr;
    void* setBufferApiFn = nullptr;
    void* scanCommandsFn = nullptr;
    void* freeStringFn = nullptr;
    void* runCommandFn = nullptr;
};

// Writes one line to the Session Report window. Main thread only (P0 constraint).
void Report (const GS::UniString& text);

// Same, but ALSO pops a modal alert. The Session Report window is closed by
// default, so anything the user must actually see goes through here.
void ReportAlert (const GS::UniString& text);

// Drains the script output captured since the last call. P0 uses it to show a
// whole run's transcript in one alert; P1's palette console replaces this.
GS::UniString TakeScriptTranscript ();

// %LOCALAPPDATA%\EvP\spike\hello.py — created with a default body if missing, so
// the spike has something to run and the user has something to edit.
GS::UniString GetSpikeScriptPath ();
bool EnsureSpikeScriptExists (GS::UniString& error);

// The default scripts root: %LOCALAPPDATA%\Tapioca\Commands.
// Configurable and multi-root later; one root is enough for P2.
GS::UniString GetScriptsRoot ();

// Creates the scripts root and a sample command folder if absent, so the palette
// has something real to show on a fresh install. Never clobbers existing files.
bool EnsureSampleCommandExists (GS::UniString& error);

} // namespace evp

#endif
