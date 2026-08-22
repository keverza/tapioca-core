#include "APIEnvir.h"
#include "ACAPinc.h"

#include "PythonHost.hpp"
#include "PathUtils.hpp"
#include "MainThreadGate.hpp"
#include "ApiDispatcher.hpp"
#include "EvPPyApi.h"
#include "Geometry/MeshStore.hpp"

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// GS::uchar_t is UInt16 and, under the SDK's forced /Zc:wchar_t-, so is wchar_t
// — the same type. So UniString::ToUStr() is directly a `const uint16_t*` for
// the EvPPyApi boundary, with no conversion. See PathUtils.cpp for the full
// story on the flag (and why <fstream> is banned here).

namespace {

bool HasPythonDll (const GS::UniString& dir)
{
    const GS::UniString path (dir + GS::UniString ("\\python312.dll"));
    return evp::PathExists (path);
}

// Runtime resolution order. P4 replaces (2) with a real bootstrap of the bundled
// embeddable distro; (3) keeps the spike runnable on a dev box that just has the
// 3.12 installer's output.
bool ResolveRuntimeHome (GS::UniString& home, GS::UniString& error)
{
    GS::UniString candidate;
    if (evp::ReadEnv (L"EVP_PYTHON_HOME", candidate) && HasPythonDll (candidate)) {
        home = candidate;
        return true;
    }

    GS::UniString localAppData;
    if (evp::ReadEnv (L"LOCALAPPDATA", localAppData)) {
        const GS::UniString managed (evp::EvpDataDir () + GS::UniString ("\\runtime"));
        if (HasPythonDll (managed)) {
            home = managed;
            return true;
        }
        const GS::UniString installed (localAppData + GS::UniString ("\\Programs\\Python\\Python312"));
        if (HasPythonDll (installed)) {
            home = installed;
            return true;
        }
    }

    error = "python312.dll not found. Looked at %EVP_PYTHON_HOME%, "
            "%LOCALAPPDATA%\\Tapioca\\runtime and %LOCALAPPDATA%\\Programs\\Python\\Python312. "
            "Set EVP_PYTHON_HOME to a folder containing python312.dll.";
    return false;
}

// EvPPy.dll ships beside the .apx, so locate it relative to this module rather
// than the process — Archicad.exe lives somewhere else entirely.
bool GetOwnDirectory (GS::UniString& dir, GS::UniString& error)
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) &ResolveRuntimeHome, &self) == 0) {
        error = "GetModuleHandleEx failed for the add-on module.";
        return false;
    }

    std::vector<wchar_t> buffer (MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW (self, (LPWSTR) buffer.data (), (DWORD) buffer.size ());
        if (written == 0) {
            error = "GetModuleFileName failed for the add-on module.";
            return false;
        }
        if (written < buffer.size () - 1)
            break;
        buffer.resize (buffer.size () * 2); // path was truncated
    }

    const GS::UniString path (buffer.data ());
    const UIndex separator = path.FindLast ('\\');
    if (separator == MaxUIndex) {
        error = "The add-on module path has no directory component.";
        return false;
    }
    dir = path.GetSubstring (0, separator);
    return true;
}

std::mutex transcriptMutex;
GS::UniString scriptTranscript;

// ---- the Layer 1 bus, as EvPPy.dll sees it ---------------------------------
// Called from Zone B's worker (GIL released by _evp.call). DispatchApiCall does
// its own main-thread marshaling, so there is nothing thread-related to do here.
int ApiCallCallback (const char* command, const char* paramsJson, char** resultJsonOut)
{
    if (resultJsonOut == nullptr)
        return EVPPY_ERROR;

    const GS::UniString envelope =
        evp::DispatchApiCall (GS::UniString (command, CC_UTF8), GS::UniString (paramsJson, CC_UTF8));
    const auto utf8 = envelope.ToCStr (0, MaxUSize, CC_UTF8);
    const size_t length = strlen (utf8.Get ());

    // Allocated here, freed by FreeCallback below — both in the .apx, so the
    // allocator never crosses the DLL boundary.
    char* const buffer = (char*) malloc (length + 1);
    if (buffer == nullptr)
        return EVPPY_ERROR;
    memcpy (buffer, utf8.Get (), length + 1);

    *resultJsonOut = buffer;
    return EVPPY_OK;
}

void FreeCallback (char* buffer)
{
    free (buffer);
}

// ---- zero-copy geometry ----------------------------------------------------
// The token is a heap-allocated shared_ptr to the snapshot. Holding it is what
// makes ReleaseSnapshot safe while numpy views are live: the store drops its
// reference, this one keeps the memory alive, and the last view's dealloc frees
// it. The plan's "no dangling pointers by construction", made real.
//
// No main-thread hop: snapshots are immutable and MeshStore is mutex-guarded.
int AcquireBufferCallback (const char* requestJson, const void** dataOut, int64_t* sizeOut, char* metaJsonOut,
                           int metaSize, void** tokenOut)
{
    const auto fail = [&] (const char* why) {
        if (metaJsonOut != nullptr && metaSize > 0) {
            strncpy (metaJsonOut, why, (size_t) metaSize - 1);
            metaJsonOut[metaSize - 1] = '\0';
        }
        return EVPPY_ERROR;
    };

    GS::ObjectState request;
    if (JSON::ConvertToObjectState (GS::UniString (requestJson, CC_UTF8), request) != NoError)
        return fail ("buffer request is not valid JSON");

    GS::UniString kind;
    GS::Int32 meshIndex = -1;
    request.Get ("kind", kind);
    request.Get ("mesh", meshIndex);

    const std::shared_ptr<const geomsrv::Snapshot> snapshot = geomsrv::MeshStore::Get ().Current ();
    if (snapshot == nullptr)
        return fail ("no snapshot is live - call EvP.BuildSnapshot first");
    if (meshIndex < 0 || (size_t) meshIndex >= snapshot->meshes.size ())
        return fail ("mesh index out of range");

    const geomsrv::Mesh& mesh = snapshot->meshes[(size_t) meshIndex];

    const void* data = nullptr;
    int64_t bytes = 0;
    const char* dtype = nullptr;
    int64_t rows = 0;
    int cols = 0;

    if (kind == "vertices") {
        data = mesh.vertices.data ();
        bytes = (int64_t) (mesh.vertices.size () * sizeof (double));
        dtype = "float64";
        rows = (int64_t) mesh.VertexCount ();
        cols = 3;
    }
    else if (kind == "normals") {
        data = mesh.normals.data ();
        bytes = (int64_t) (mesh.normals.size () * sizeof (float));
        dtype = "float32";
        rows = (int64_t) (mesh.normals.size () / 3);
        cols = 3;
    }
    else if (kind == "triangles") {
        data = mesh.triangles.data ();
        bytes = (int64_t) (mesh.triangles.size () * sizeof (uint32_t));
        dtype = "uint32";
        rows = (int64_t) mesh.TriangleCount ();
        cols = 3;
    }
    else if (kind == "triMaterial") {
        data = mesh.triMaterial.data ();
        bytes = (int64_t) (mesh.triMaterial.size () * sizeof (int32_t));
        dtype = "int32";
        rows = (int64_t) mesh.triMaterial.size ();
        cols = 1;
    }
    else {
        return fail ("unknown buffer kind (want vertices|normals|triangles|triMaterial)");
    }

    if (bytes == 0)
        return fail ("that buffer is empty for this mesh");

    // snapshot_id travels with every buffer: views are explicitly point-in-time,
    // so a trace must show exactly which state a read came from.
    const GS::UniString meta = GS::UniString::Printf (
        "{\"dtype\":\"%s\",\"shape\":[%lld,%d],\"bytes\":%lld,\"snapshot_id\":%llu,\"kind\":\"%T\"}", dtype,
        (long long) rows, cols, (long long) bytes, (unsigned long long) snapshot->id, kind.ToPrintf ());
    const auto metaUtf8 = meta.ToCStr (0, MaxUSize, CC_UTF8);
    if ((int) strlen (metaUtf8.Get ()) >= metaSize)
        return fail ("buffer meta does not fit");
    strcpy (metaJsonOut, metaUtf8.Get ());

    *dataOut = data;
    *sizeOut = bytes;
    *tokenOut = new std::shared_ptr<const geomsrv::Snapshot> (snapshot); // the strong ref
    return EVPPY_OK;
}

void ReleaseBufferCallback (void* token)
{
    delete static_cast<std::shared_ptr<const geomsrv::Snapshot>*> (token);
}

// EvPPy.dll calls this from whichever thread is running Python — which, since
// P1, is Zone B's worker, WITH the GIL held. ACAPI_WriteReport is main-thread
// only, so it has to go through the gate.
//
// Invoke (synchronous, ~3ms) rather than Post, deliberately: every print() is
// then a real gate round trip issued from a thread that is actively executing
// Python, which is exactly the configuration P1 spike A could not cover. P2's
// palette console should switch to Post/batching — 3ms per line does not scale
// to a chatty script.
void ReportCallback (const char* utf8Line, int isErr)
{
    const GS::UniString line (utf8Line, CC_UTF8);
    const GS::UniString text ((isErr != 0 ? GS::UniString ("[EvP py!] ") : GS::UniString ("[EvP py] ")) + line);

    {
        std::lock_guard<std::mutex> lock (transcriptMutex);
        scriptTranscript += (isErr != 0) ? "! " : "  ";
        scriptTranscript += line;
        scriptTranscript += "\n";
    }

    GS::UniString gateError;
    // `text` by VALUE: on timeout the gate may still run this after we return.
    if (!evp::MainThreadGate::Get ().Invoke ([text] { evp::Report (text); }, 5000, gateError)) {
        // Output is not worth failing a run over, but it must not vanish either.
        std::lock_guard<std::mutex> lock (transcriptMutex);
        scriptTranscript += "! [gate] ";
        scriptTranscript += gateError;
        scriptTranscript += "\n";
    }
}

// The sample command folder — canonical form, so the first thing a user reads is
// a correct example (see evp/_scanner.py for the contract it obeys).
const char* const sampleCommand =
    "\"\"\"A sample EvP command. Folder = one command; this file is the entry point.\"\"\"\n"
    "import evp\n"
    "\n"
    "\n"
    "@evp.command(title=\"Hello Command\", category=\"Samples\", requires_api=\">=1.0\",\n"
    "             description=\"Reports the Archicad version and the live selection.\")\n"
    "def run(\n"
    "    greeting: str = \"hello\",\n"
    "    times: evp.Int(minimum=1, maximum=10) = 1,\n"
    "    shout: bool = False,\n"
    "    scope: evp.Enum(\"selection\", \"all\") = \"selection\",\n"
    "):\n"
    "    for _ in range(times):\n"
    "        print(greeting.upper() if shout else greeting)\n"
    "\n"
    "    info = evp.call(\"API.GetProductInfo\")\n"
    "    print(\"Archicad %s build %s\" % (info.data[\"version\"], info.data[\"buildNumber\"]))\n"
    "    print(\"scope:\", scope)\n";

// Exercises EVERY generated control type, so the UI can be judged as a whole
// rather than one control at a time.
const char* const uiShowcaseCommand =
    "\"\"\"Every generated control type in one command — for judging the UI.\n"
    "\n"
    "Each parameter below maps to a different DG control. Run it and the values\n"
    "you set are printed back, so you can confirm the palette reads them all.\n"
    "\"\"\"\n"
    "import evp\n"
    "\n"
    "\n"
    "@evp.command(title=\"UI Showcase\", category=\"Samples\", requires_api=\">=1.0\",\n"
    "             description=\"Every control type: text, checkbox, ints, units, popups.\")\n"
    "def run(\n"
    "    label: str = \"slope symbol\",                        # TextEdit\n"
    "    enabled: bool = True,                               # CheckBox\n"
    "    visible: bool = False,                              # CheckBox (unchecked)\n"
    "    count: evp.Int(minimum=1, maximum=99) = 3,          # IntEdit, clamped\n"
    "    ratio: float = 0.75,                                # RealEdit (plain)\n"
    "    offset: evp.Float(unit=\"m\") = 0.15,                 # LengthEdit  -> Working Units\n"
    "    min_area: evp.Float(unit=\"m2\") = 2.5,               # AreaEdit    -> Working Units\n"
    "    volume: evp.Float(unit=\"m3\") = 1.25,                # VolumeEdit  -> Working Units\n"
    "    rotation: evp.Float(unit=\"deg\") = 45.0,             # AngleEdit   -> Working Units\n"
    "    scope: evp.Enum(\"selection\", \"all\", \"storey\") = \"all\",   # PopUp\n"
    "    style: evp.Enum(\"simple\", \"detailed\") = \"simple\",        # PopUp\n"
    "    layer: evp.Layer = \"Annotation\",                    # Archicad's layer picker\n"
    "    pen: evp.Pen = 1,                                   # Archicad's pen picker\n"
    "    fill: evp.Fill = \"Solid\",                           # Archicad's fill picker\n"
    "    line_type: evp.LineType = \"Solid Line\",             # Archicad's line type picker\n"
    "):\n"
    "    print(\"--- values the palette handed to run() ---\")\n"
    "    print(\"  label    (text)   =\", repr(label))\n"
    "    print(\"  enabled  (bool)   =\", enabled)\n"
    "    print(\"  visible  (bool)   =\", visible)\n"
    "    print(\"  count    (int)    =\", count)\n"
    "    print(\"  ratio    (real)   =\", ratio)\n"
    "    print(\"  offset   (m)      =\", offset)\n"
    "    print(\"  min_area (m2)     =\", min_area)\n"
    "    print(\"  volume   (m3)     =\", volume)\n"
    "    print(\"  rotation (deg)    =\", rotation)\n"
    "    print(\"  scope    (enum)   =\", repr(scope))\n"
    "    print(\"  style    (enum)   =\", repr(style))\n"
    "    print(\"  layer    (layer)  =\", repr(layer))\n"
    "    print(\"  pen      (pen)    =\", pen)\n"
    "    print(\"  fill     (fill)   =\", repr(fill))\n"
    "    print(\"  line_type (ltype) =\", repr(line_type))\n"
    "    print(\"\")\n"
    "    print(\"Unit note: offset/min_area/volume/rotation arrive in METRES and\")\n"
    "    print(\"DEGREES whatever the project's Working Units are - DG converts.\")\n"
    "    print(\"Attribute note: layer/fill/line_type arrive as NAMES from the\")\n"
    "    print(\"project's own pickers; pen is a number. None can be mistyped.\")\n";

const char* const helloScript =
    "\"\"\"EvP spike — edit me and re-run: the change must show up without\n"
    "restarting Archicad (fresh namespace per run, interpreter never finalized).\n"
    "\"\"\"\n"
    "import sys\n"
    "import time\n"
    "\n"
    "import evp\n"
    "\n"
    "evp.debug(True)   # trace every envelope -> logs\\api_trace.log\n"
    "\n"
    "GREETING = \"hello from embedded CPython\"\n"
    "\n"
    "print(GREETING)\n"
    "print(\"version:   \", sys.version.replace(\"\\n\", \" \"))\n"
    "print(\"module:    \", __name__)\n"
    "print(\"run at:    \", time.strftime(\"%H:%M:%S\"))\n"
    "\n"
    "# Layer 1: the explicit bus. Every Archicad operation looks like this.\n"
    "print(\"evp.API_VERSION:\", evp.API_VERSION)\n"
    "res = evp.call(\"EvP.GetStatus\")\n"
    "print(\"EvP.GetStatus ok:\", res.ok)\n"
    "print(\"  backend:\", res.meta.get(\"backend\"),\n"
    "      \"| duration_ms: %.2f\" % res.meta.get(\"duration_ms\", 0.0),\n"
    "      \"| main_thread_ms: %.2f\" % res.meta.get(\"main_thread_ms\", 0.0),\n"
    "      \"| call_id:\", res.meta.get(\"call_id\"))\n"
    "for key in sorted(res.data or {}):\n"
    "    print(\"  data.%s = %r\" % (key, res.data[key]))\n"
    "\n"
    "# Errors are structured and never swallowed.\n"
    "bad = evp.call(\"EvP.NoSuchCommand\", raise_on_error=False)\n"
    "print(\"error path:\", bad.ok, bad.error and bad.error.get(\"code\"))\n"
    "\n"
    "# --- API.* : Archicad's official JSON interface, in-process -----------\n"
    "info = evp.call(\"API.GetProductInfo\")\n"
    "print(\"API.GetProductInfo ok:\", info.ok, \"| backend:\", info.meta.get(\"backend\"))\n"
    "for key in sorted(info.data or {}):\n"
    "    print(\"   %s = %r\" % (key, info.data[key]))\n"
    "nope = evp.call(\"API.NoSuchJsonCommand\", raise_on_error=False)\n"
    "print(\"API error path:\", nope.ok, nope.error and nope.error.get(\"code\"))\n"
    "\n"
    "# --- transactions: the rollback proof -------------------------------\n"
    "# 1) a GOOD batch -> 2 level dimensions, ONE undo step.\n"
    "with evp.transaction(\"EvP test: good batch\") as tx:\n"
    "    tx.call(\"EvP.PlaceLevelDimension\", {\"x\": 0.0, \"y\": 0.0, \"value\": 1.0, \"text\": \"EvP A\"})\n"
    "    tx.call(\"EvP.PlaceLevelDimension\", {\"x\": 2.0, \"y\": 0.0, \"value\": 2.0, \"text\": \"EvP B\"})\n"
    "print(\"good batch committed:\", len(tx.results), \"steps\")\n"
    "for r in tx.results:\n"
    "    print(\"   created:\", r.get(\"guid\"))\n"
    "\n"
    "# 2) a BAD batch -> step 0 is valid, step 1 cannot work (no value for a\n"
    "#    static level dimension). Archicad must roll BOTH back: if step 0's\n"
    "#    dimension survives at (9,9), atomicity is broken.\n"
    "try:\n"
    "    with evp.transaction(\"EvP test: bad batch\") as bad_tx:\n"
    "        bad_tx.call(\"EvP.PlaceLevelDimension\", {\"x\": 9.0, \"y\": 9.0, \"value\": 3.0, \"text\": \"SHOULD NOT "
    "SURVIVE\"})\n"
    "        bad_tx.call(\"EvP.PlaceLevelDimension\", {\"x\": 9.0, \"y\": 11.0})\n"
    "    print(\"!! BAD: the bad batch committed - atomicity is BROKEN\")\n"
    "except evp.EvpError as e:\n"
    "    print(\"bad batch rejected as expected:\")\n"
    "    print(\"   \", e.code)\n"
    "    print(\"   \", e.message)\n"
    "\n"
    "\n"
    "# 3) DEFERRED HANDLES: step 1 consumes a GUID that does not exist until\n"
    "#    step 0 has run — resolved server-side, mid-batch, inside one undo step.\n"
    "with evp.transaction(\"EvP test: deferred handle\") as htx:\n"
    "    mesh = htx.call(\"EvP.CreateMesh\", {\"outline\": [20.0, 0.0, 24.0, 0.0, 24.0, 4.0, 20.0, 4.0],\n"
    "                                        \"baseLevel\": 0.0})\n"
    "    htx.call(\"EvP.PlaceLevelDimension\", {\"x\": 22.0, \"y\": 2.0, \"mode\": \"associative\",\n"
    "                                          \"parentGuid\": mesh.guid, \"text\": \"bound to the mesh\"})\n"
    "print(\"handle batch committed:\", len(htx.results), \"steps\")\n"
    "print(\"   mesh guid:\", htx.results[0].get(\"guid\"))\n"
    "print(\"   dimension:\", htx.results[1].get(\"guid\"))\n"
    "\n"
    "\n"
    "# --- zero-copy numpy -------------------------------------------------\n"
    "try:\n"
    "    import numpy as np\n"
    "    print(\"numpy:\", np.__version__)\n"
    "    snap = evp.geometry.snapshot()\n"
    "    print(\"snapshot:\", snap)\n"
    "    if not snap.meshes:\n"
    "        print(\"   (no geometry in this project - open one with elements to test views)\")\n"
    "    else:\n"
    "        m = max(snap.meshes, key=lambda x: x.triangle_count)\n"
    "        v = m.vertices()\n"
    "        t = m.triangles()\n"
    "        print(\"   biggest mesh:\", m)\n"
    "        print(\"   vertices:\", v.shape, v.dtype, \"| read-only:\", not v.flags.writeable)\n"
    "        print(\"   triangles:\", t.shape, t.dtype, \"| read-only:\", not t.flags.writeable)\n"
    "        print(\"   bbox min:\", np.round(v.min(axis=0), 3), \"max:\", np.round(v.max(axis=0), 3))\n"
    "        # ZERO-COPY proof: the view must alias the C++ buffer, not copy it.\n"
    "        print(\"   owns its data (must be False):\", v.flags.owndata)\n"
    "        root = v\n"
    "        while getattr(root, \"base\", None) is not None:\n"
    "            root = root.base\n"
    "        print(\"   base chain root (must be Buffer):\", type(root).__name__)\n"
    "        # LIFETIME proof: release the snapshot, then keep using the view.\n"
    "        checksum = float(v.sum())\n"
    "        snap.release()\n"
    "        after = float(v.sum())\n"
    "        print(\"   after ReleaseSnapshot, view still valid:\", checksum == after)\n"
    "        print(\"   (the store let go; our view's token kept the memory alive)\")\n"
    "except ImportError as e:\n"
    "    print(\"numpy unavailable in the embedded runtime:\", e)\n"
    "\n"
    "print(\"CHECK IN THE MODEL:\")\n"
    "print(\"  * exactly TWO dimensions (EvP A, EvP B) near the origin\")\n"
    "print(\"  * NOTHING at (9,9) - the bad batch rolled back\")\n"
    "print(\"  * a 4x4 mesh at (20,0) with an associative dimension on it\")\n"
    "print(\"  * each batch costs exactly ONE Undo\")\n";

} // namespace

namespace evp {

void Report (const GS::UniString& text)
{
    // Text goes through as an ARGUMENT, never as the format string — script
    // output is arbitrary and would otherwise be reinterpreted as % specifiers.
    ACAPI_WriteReport ("%T", false, text.ToPrintf ());
}

void ReportAlert (const GS::UniString& text)
{
    ACAPI_WriteReport ("%T", true, text.ToPrintf ());
}

GS::UniString TakeScriptTranscript ()
{
    std::lock_guard<std::mutex> lock (transcriptMutex);
    const GS::UniString taken = scriptTranscript;
    scriptTranscript.Clear ();
    return taken;
}

PythonHost& PythonHost::Get ()
{
    static PythonHost instance;
    return instance;
}

bool PythonHost::EnsureInitialized (GS::UniString& error)
{
    if (initialized)
        return true;

    // ⚠️ EVERY STEP BELOW IS TRACED, because none of them can report a HARD
    // failure: LoadLibrary and Py_Initialize either return an error this function
    // turns into a message, or they take the process with them and leave nothing.
    // Two Archicad sessions died somewhere in this call with no trace at all.
    StartupTrace ("PythonHost: resolving the runtime home");
    if (!ResolveRuntimeHome (runtimeHome, error))
        return false;

    // Order matters: pre-load python312.dll by full path so that EvPPy.dll's
    // load-time import of it binds to this instance (the loader keys modules by
    // base name) instead of searching PATH for some other Python.
    if (pythonDll == nullptr) {
        StartupTrace ("PythonHost: LoadLibrary python312.dll");
        const GS::UniString dllPath (runtimeHome + GS::UniString ("\\python312.dll"));
        pythonDll = (void*) LoadLibraryW ((LPCWSTR) dllPath.ToUStr ().Get ());
        if (pythonDll == nullptr) {
            error = GS::UniString::Printf ("LoadLibrary failed for %T (error %lu).", dllPath.ToPrintf (),
                                           (unsigned long) GetLastError ());
            return false;
        }
    }

    if (bridgeDll == nullptr) {
        if (!GetOwnDirectory (ownDir, error))
            return false;

        StartupTrace ("PythonHost: LoadLibrary EvPPy.dll");
        const GS::UniString bridgePath (ownDir + GS::UniString ("\\EvPPy.dll"));
        bridgeDll = (void*) LoadLibraryW ((LPCWSTR) bridgePath.ToUStr ().Get ());
        if (bridgeDll == nullptr) {
            error = GS::UniString::Printf ("LoadLibrary failed for %T (error %lu). "
                                           "EvPPy.dll must sit next to EvP.apx.",
                                           bridgePath.ToPrintf (), (unsigned long) GetLastError ());
            return false;
        }

        initializeFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_INITIALIZE_SYMBOL);
        runScriptFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_RUNSCRIPTFILE_SYMBOL);
        setApiCallFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_SETAPICALL_SYMBOL);
        addSysPathFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_ADDSYSPATHFRONT_SYMBOL);
        setBufferApiFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_SETBUFFERAPI_SYMBOL);
        scanCommandsFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_SCANCOMMANDS_SYMBOL);
        freeStringFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_FREESTRING_SYMBOL);
        runCommandFn = (void*) GetProcAddress ((HMODULE) bridgeDll, EVPPY_RUNCOMMAND_SYMBOL);
        if (initializeFn == nullptr || runScriptFn == nullptr || setApiCallFn == nullptr || addSysPathFn == nullptr ||
            setBufferApiFn == nullptr || scanCommandsFn == nullptr || freeStringFn == nullptr ||
            runCommandFn == nullptr) {
            error = "EvPPy.dll does not export the expected entry points — stale build?";
            return false;
        }
    }

    // The interpreter itself starts here. A trail that stops on this line means
    // the embedded CPython died inside Py_Initialize without returning anything.
    StartupTrace ("PythonHost: initializing the interpreter");
    char errorBuffer[512] = { 0 };
    if (((EvpPy_InitializeFn) initializeFn) (runtimeHome.ToUStr ().Get (), ReportCallback, errorBuffer,
                                             (int) sizeof (errorBuffer)) != EVPPY_OK) {
        error = GS::UniString (errorBuffer, CC_UTF8);
        return false;
    }

    // Wire the Layer 1 bus and the zero-copy channel before anything can import evp.
    ((EvpPy_SetApiCallFn) setApiCallFn) (ApiCallCallback, FreeCallback);
    ((EvpPy_SetBufferApiFn) setBufferApiFn) (AcquireBufferCallback, ReleaseBufferCallback);

    // Our `evp` package ships beside the .apx and goes to the FRONT of sys.path:
    // the plan's supply-chain rule is that site-packages can never shadow it.
    // P4 moves the environment to %LOCALAPPDATA%; this stays read-only either way.
    const GS::UniString packageDir (ownDir + GS::UniString ("\\PyPackage"));
    if (((EvpPy_AddSysPathFrontFn) addSysPathFn) (packageDir.ToUStr ().Get (), errorBuffer,
                                                  (int) sizeof (errorBuffer)) != EVPPY_OK) {
        error = GS::UniString (errorBuffer, CC_UTF8);
        return false;
    }

    StartupTrace ("PythonHost: interpreter ready");
    initialized = true;
    return true;
}

bool PythonHost::RunScriptFile (const GS::UniString& path, const GS::UniString& moduleName, GS::UniString& error)
{
    if (!EnsureInitialized (error))
        return false;

    const auto moduleUtf8 = moduleName.ToCStr (0, MaxUSize, CC_UTF8);

    char errorBuffer[512] = { 0 };
    if (((EvpPy_RunScriptFileFn) runScriptFn) (path.ToUStr ().Get (), moduleUtf8.Get (), errorBuffer,
                                               (int) sizeof (errorBuffer)) != EVPPY_OK) {
        error = GS::UniString (errorBuffer, CC_UTF8);
        return false;
    }
    return true;
}

bool PythonHost::ScanCommands (const GS::UniString& root, GS::UniString& json, GS::UniString& error)
{
    if (!EnsureInitialized (error))
        return false;

    char* scanJson = nullptr;
    char errorBuffer[512] = { 0 };
    if (((EvpPy_ScanCommandsFn) scanCommandsFn) (root.ToUStr ().Get (), &scanJson, errorBuffer,
                                                 (int) sizeof (errorBuffer)) != EVPPY_OK) {
        error = GS::UniString (errorBuffer, CC_UTF8);
        return false;
    }

    json = GS::UniString (scanJson, CC_UTF8);
    ((EvpPy_FreeStringFn) freeStringFn) (scanJson); // allocated by EvPPy, freed by EvPPy
    return true;
}

bool PythonHost::RunCommand (const GS::UniString& path, const GS::UniString& moduleName,
                             const GS::UniString& paramsJson, const GS::UniString& action,
                             const GS::UniString& menuRegion, bool& cancelled, GS::UniString& error)
{
    cancelled = false;
    if (!EnsureInitialized (error))
        return false;

    const auto moduleUtf8 = moduleName.ToCStr (0, MaxUSize, CC_UTF8);
    const auto paramsUtf8 = paramsJson.ToCStr (0, MaxUSize, CC_UTF8);
    const auto actionUtf8 = action.ToCStr (0, MaxUSize, CC_UTF8);
    const auto regionUtf8 = menuRegion.ToCStr (0, MaxUSize, CC_UTF8);

    char errorBuffer[512] = { 0 };
    const int code = ((EvpPy_RunCommandFn) runCommandFn) (path.ToUStr ().Get (), moduleUtf8.Get (), paramsUtf8.Get (),
                                                          actionUtf8.Get (), regionUtf8.Get (), errorBuffer,
                                                          (int) sizeof (errorBuffer));

    // E9 — a cancel is its own outcome, between "ran" and "failed": report it as a
    // success so nothing writes FAILED, but tell the caller so the status can say
    // what actually happened.
    if (code == EVPPY_CANCELLED) {
        cancelled = true;
        return true;
    }
    if (code != EVPPY_OK) {
        error = GS::UniString (errorBuffer, CC_UTF8);
        return false;
    }
    return true;
}

GS::UniString GetScriptsRoot ()
{
    const GS::UniString dataDir = EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();

    const GS::UniString root (dataDir + GS::UniString ("\\Commands"));

    // Move the generated working copy once so existing installations do not lose
    // their commands when the scripts root leaves Documents.
    GS::UniString profile;
    if (!PathExists (root) && ReadEnv (L"USERPROFILE", profile)) {
        const GS::UniString oldTapioca (profile + GS::UniString ("\\Documents\\Tapioca Commands"));
        const GS::UniString oldEvP (profile + GS::UniString ("\\Documents\\EvP Commands"));
        const GS::UniString legacy = PathExists (oldTapioca) ? oldTapioca : oldEvP;
        if (!legacy.IsEmpty () && PathExists (legacy) && CreateDirectoryChain (dataDir))
            MoveFileExW ((LPCWSTR) legacy.ToUStr ().Get (), (LPCWSTR) root.ToUStr ().Get (),
                         MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
    }
    return root;
}

bool EnsureSampleCommandExists (GS::UniString& error)
{
    const GS::UniString root = GetScriptsRoot ();
    if (root.IsEmpty ()) {
        error = "%LOCALAPPDATA% is not set.";
        return false;
    }

    struct Sample {
        const char* folder;
        const char* source;
    };
    const Sample samples[] = {
        { "\\HelloCommand", sampleCommand },
        { "\\UIShowcase", uiShowcaseCommand },
    };

    for (const Sample& sample : samples) {
        const GS::UniString folder (root + GS::UniString (sample.folder));
        if (!CreateDirectoryChain (folder)) {
            error = "Could not create " + folder;
            return false;
        }
        const GS::UniString entry (folder + GS::UniString ("\\command.py"));
        if (PathExists (entry))
            continue; // never clobber the user's edits
        if (!WriteTextFile (entry, sample.source, error))
            return false;
    }
    return true;
}

GS::UniString GetSpikeScriptPath ()
{
    const GS::UniString dataDir = EvpDataDir ();
    if (dataDir.IsEmpty ())
        return GS::UniString ();
    return dataDir + GS::UniString ("\\spike\\hello.py");
}

bool EnsureSpikeScriptExists (GS::UniString& error)
{
    const GS::UniString path = GetSpikeScriptPath ();
    if (path.IsEmpty ()) {
        error = "%LOCALAPPDATA% is not set.";
        return false;
    }
    if (PathExists (path))
        return true; // never clobber the user's edits

    const UIndex separator = path.FindLast ('\\');
    if (separator == MaxUIndex || !CreateDirectoryChain (path.GetSubstring (0, separator))) {
        error = "Could not create the spike script folder.";
        return false;
    }
    return WriteTextFile (path, helloScript, error);
}

} // namespace evp
