#ifndef EVPPY_API_H
#define EVPPY_API_H

// The contract between EvP.apx and EvPPy.dll — the ONLY thing the two binaries
// share. Included verbatim by both sides.
//
// Why two binaries at all (decided in P0, see evp-command-system-plan.md):
// python312.dll exports data symbols (_Py_NoneStruct, the type objects behind
// every Py*_Check, every PyExc_*), and the Windows loader cannot delay-load a
// data import — LNK1194. So a binary that touches the CPython C API must import
// python312.dll at load time. The .apx must NOT: Archicad loads it from Program
// Files, and the loader would then search only Archicad.exe's directory,
// System32 and PATH — never %LOCALAPPDATA%\EvP\runtime. EvP would fail to load
// on any machine without Python parked somewhere findable.
//
// So EvPPy.dll takes the load-time dependency, and the .apx loads EvPPy.dll
// itself, by full path, after pre-loading python312.dll by full path. The
// loader keys modules by base name, so EvPPy.dll's import binds to the instance
// we already chose. The runtime can live anywhere; EvP loads fine with no
// Python installed at all.
//
// ABI rules for everything below — the reason this interface is POD and C:
//   * The .apx is built with the SDK's forced /Zc:wchar_t- (wchar_t is a
//     typedef for unsigned short); EvPPy.dll is built with standard flags
//     (wchar_t is a distinct builtin). Wide strings therefore cross as an
//     explicit uint16_t*, so neither side depends on the other's flag.
//   * extern "C": no C++ mangling, nothing to mismatch.
//   * No C++ types, no allocation across the boundary: the caller owns every
//     buffer. EvPPy.dll never calls ACAPI — it reports through a callback the
//     .apx installs.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVPPY_OK 0
#define EVPPY_ERROR 1
// E9 — run() unwound on evp.Cancelled: the user stopped it, the palette closed, or
// the command's timeout_s elapsed. A CLEAN outcome, not a failure, so the caller
// reports "cancelled" and writes no traceback. Only EvpPy_RunCommand returns it.
// (An int return code is POD, so this stays inside the ABI rules above.)
#define EVPPY_CANCELLED 2

// Layer 1 bus. `_evp.call` in Python bottoms out here: EvPPy.dll knows nothing
// about Archicad, so the .apx installs the implementation.
//
// Ownership: the .apx ALLOCATES *resultJsonOut and the .apx frees it, via the
// paired free function — never mix allocators across the boundary, even though
// both binaries happen to share the /MD CRT heap today.
// `resultJsonOut` is a NUL-terminated UTF-8 JSON envelope. Returns EVPPY_OK
// whenever an envelope was produced, INCLUDING one carrying ok=false: a failed
// Archicad call is a normal envelope, not a transport failure. EVPPY_ERROR means
// the bus itself broke.
typedef int (*EvpPy_ApiCallFn) (const char* command, const char* paramsJson, char** resultJsonOut);
typedef void (*EvpPy_FreeFn) (char* buffer);

// Installs the bus. Call after EvpPy_Initialize, before running any script.
typedef void (*EvpPy_SetApiCallFn) (EvpPy_ApiCallFn apiCall, EvpPy_FreeFn freeResult);

// Prepends `dir` to sys.path. Used to give the add-on's own `evp` package
// precedence over anything pip may later drop into site-packages — the plan's
// supply-chain rule: `evp`/`_evp` must be unshadowable.
typedef int (*EvpPy_AddSysPathFrontFn) (const uint16_t* dir, char* errorUtf8, int errorSize);

// Zero-copy geometry. The snapshot lives in the .apx (MeshStore); EvPPy.dll only
// wraps it for Python, so the .apx hands over a raw pointer plus an OWNING TOKEN.
//
// The token IS the lifetime guarantee: it holds a strong reference to the
// snapshot (a shared_ptr), so ReleaseSnapshot only ever drops the STORE's
// reference — the memory survives until the last numpy view dies and its buffer
// object's dealloc releases the token. No dangling pointer is possible by
// construction; that is the plan's rule, implemented rather than asserted.
//
// `requestJson`: {"kind":"vertices"|"normals"|"triangles"|"triMaterial", "mesh": <index>}
// `metaJsonOut`: {"dtype":"float64", "shape":[n,3], "snapshot_id":N, ...} — the
// dtype/shape Python needs for np.frombuffer, and snapshot_id so a trace shows
// exactly which state a read came from.
//
// Requires NO main-thread hop: snapshots are immutable and MeshStore is
// mutex-guarded, so a worker reads them directly.
typedef int (*EvpPy_AcquireBufferFn) (const char* requestJson, const void** dataOut, int64_t* sizeOut,
                                      char* metaJsonOut, int metaSize, void** tokenOut);
typedef void (*EvpPy_ReleaseBufferFn) (void* token);

typedef void (*EvpPy_SetBufferApiFn) (EvpPy_AcquireBufferFn acquire, EvpPy_ReleaseBufferFn release);

// Scans a scripts root by driving evp._scanner, which reads each command.py's
// AST WITHOUT EXECUTING IT — scripts are arbitrary code and scanning happens for
// every folder at startup, including ones the user just received from someone.
// Returns the whole result as JSON: {"root", "commands":[...], "diagnostics":[...]}.
// *jsonOut is allocated by EvPPy.dll and MUST be freed with EvpPy_FreeString.
typedef int (*EvpPy_ScanCommandsFn) (const uint16_t* root, char** jsonOut, char* errorUtf8, int errorSize);

// Runs a command folder's run(**params). Unlike RunScriptFile (which only
// executes the module), this calls run() with the dialog's values and performs
// the runtime cross-check that the real @evp.command decorator registered the
// metadata the scanner read statically.
// `actionName` (UTF-8, may be null or empty) selects WHICH entry point runs:
// empty means run() itself; a name means one of the command's declared output
// actions, executed against the LAST run's stored result. It is another POD
// pointer, so the rules above still hold — and it is a parameter rather than a
// key smuggled into paramsJson because paramsJson is the user's values and
// nothing else should ever have to be filtered back out of it.
typedef int (*EvpPy_RunCommandFn) (const uint16_t* scriptPath, const char* moduleName, const char* paramsJson,
                                   const char* actionName, char* errorUtf8, int errorSize);
typedef void (*EvpPy_FreeStringFn) (char* text);

#define EVPPY_SETAPICALL_SYMBOL "EvpPy_SetApiCall"
#define EVPPY_ADDSYSPATHFRONT_SYMBOL "EvpPy_AddSysPathFront"
#define EVPPY_SETBUFFERAPI_SYMBOL "EvpPy_SetBufferApi"
#define EVPPY_SCANCOMMANDS_SYMBOL "EvpPy_ScanCommands"
#define EVPPY_RUNCOMMAND_SYMBOL "EvpPy_RunCommand"
#define EVPPY_FREESTRING_SYMBOL "EvpPy_FreeString"

// Receives one complete line of script output. UTF-8, no trailing newline.
typedef void (*EvpPy_ReportFn) (const char* utf8Line, int isErr);

// Initializes the interpreter with `runtimeHome` as its Python home (UTF-16,
// NUL-terminated). Idempotent: the second call is a no-op returning EVPPY_OK.
// The interpreter is never finalized — see the plan's never-Py_Finalize rule.
typedef int (*EvpPy_InitializeFn) (const uint16_t* runtimeHome, EvpPy_ReportFn report, char* errorUtf8, int errorSize);

// Runs `scriptPath` (UTF-16) in a fresh namespace under `moduleName`, then
// evicts it from sys.modules so the next run re-reads the file.
typedef int (*EvpPy_RunScriptFileFn) (const uint16_t* scriptPath, const char* moduleName, char* errorUtf8,
                                      int errorSize);

#define EVPPY_INITIALIZE_SYMBOL "EvpPy_Initialize"
#define EVPPY_RUNSCRIPTFILE_SYMBOL "EvpPy_RunScriptFile"

#ifdef __cplusplus
}
#endif

#endif
