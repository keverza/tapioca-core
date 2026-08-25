#ifndef EVP_GRASSHOPPER_MANAGEDRUNTIME_HPP
#define EVP_GRASSHOPPER_MANAGEDRUNTIME_HPP

// Loads ONE .NET runtime into Archicad's process and hands back function
// pointers into one managed assembly. Nothing about Rhino or Grasshopper is in
// here; this is the hostfxr half on its own, so that "the CLR would not start"
// and "Rhino would not start" are two different diagnostics rather than one.
//
// ACAPI-free and DG-free by construction (Win32 + std only), the same promise
// Python/PathUtils.hpp makes and for the same reason: the CLR must be able to
// come up, or fail to, without touching Archicad state.
//
// ⚠️ Close() DOES NOT UNLOAD THE CLR. hostfxr_close releases the host CONTEXT
// handle; the runtime it started stays in the process for its lifetime. The
// handoff calls this out by name ("never claim hostfxr_close unloaded the
// process CLR") because assuming otherwise is what produces an add-on that
// believes it can be reloaded and takes Archicad down proving it.

#include <string>

namespace evp {
namespace grasshopper {

class ManagedRuntime {
  public:
    // Starts the runtime described by `runtimeConfigPath` (the framework-
    // dependent .runtimeconfig.json published beside the .apx). Idempotent:
    // a second call on a loaded runtime succeeds and changes nothing.
    bool Start (const std::wstring& runtimeConfigPath, std::wstring& error);

    // Resolves one [UnmanagedCallersOnly] static method. `typeName` is the
    // assembly-qualified type; `methodName` the method. Fails without loading
    // anything if Start has not succeeded.
    bool Resolve (const std::wstring& assemblyPath, const wchar_t* typeName, const wchar_t* methodName,
                  void** functionPointer, std::wstring& error);

    // Releases the host context. See the warning above for what it does not do.
    void Close ();

    bool IsLoaded () const;

    // "hostfxr 10.0.11 from C:\Program Files\dotnet" — for the startup log and
    // the probe's CLR-coexistence gate, which has to record the version that
    // was actually used, not the one that was asked for.
    const std::wstring& Description () const;

  private:
    void* hostFxrModule = nullptr; // HMODULE, kept opaque so <windows.h> stays out of this header
    void* hostContext = nullptr;
    void* loadAssemblyAndGetFunctionPointer = nullptr;
    std::wstring description;
};

// Finds the newest installed hostfxr.dll, or returns false with a reason.
// Exposed for the diagnostic path: "no .NET 8 runtime" must be able to say
// WHERE it looked, and a menu command that never starts anything should still
// be able to report that.
bool FindHostFxr (std::wstring& path, std::wstring& error);

} // namespace grasshopper
} // namespace evp

#endif
