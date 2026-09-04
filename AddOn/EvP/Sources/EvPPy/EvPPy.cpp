// EvPPy.dll — the CPython half of the bridge. See EvPPyApi.h for why this is a
// separate binary from EvP.apx.
//
// Nothing Archicad lives here: no ACAPI, no GS types, no SDK headers. Output
// leaves through the report callback the .apx installs at initialize time.

#include "PythonEmbed.h"
#include "EvPPyApi.h"

#include <string>

namespace {

EvpPy_ReportFn reportFn = nullptr;
bool initialized = false;
PyObject* runnerGlobals = nullptr; // strong ref, intentionally never released

std::string outBuffer;
std::string errBuffer;

void SetError (char* buffer, int size, const std::string& message)
{
    if (buffer == nullptr || size <= 0)
        return;
    const int length = (int) message.size () < size - 1 ? (int) message.size () : size - 1;
    memcpy (buffer, message.c_str (), (size_t) length);
    buffer[length] = '\0';
}

void EmitLine (bool isErr, std::string line)
{
    if (!line.empty () && line.back () == '\r')
        line.pop_back ();
    if (reportFn != nullptr)
        reportFn (line.c_str (), isErr ? 1 : 0);
}

// Python writes in arbitrary chunks and print() sends its newline separately,
// but the report window only deals in whole lines — so buffer until a newline.
void Append (bool isErr, const char* text)
{
    std::string& buffer = isErr ? errBuffer : outBuffer;
    buffer += text;
    for (std::string::size_type nl = buffer.find ('\n'); nl != std::string::npos; nl = buffer.find ('\n')) {
        EmitLine (isErr, buffer.substr (0, nl));
        buffer.erase (0, nl + 1);
    }
}

void FlushStreams ()
{
    if (!outBuffer.empty ()) {
        EmitLine (false, outBuffer);
        outBuffer.clear ();
    }
    if (!errBuffer.empty ()) {
        EmitLine (true, errBuffer);
        errBuffer.clear ();
    }
}

// ---- the `_evp` builtin module ---------------------------------------------
// Builtin (PyImport_AppendInittab), not a .pyd on sys.path — that is what makes
// it unshadowable by anything pip drops into site-packages, as the plan's
// supply-chain mitigation requires. P0 exposes exactly one function; P1 grows
// this into the command bus.

PyObject* Evp_Write (PyObject* /*self*/, PyObject* args)
{
    const char* text = nullptr;
    int isErr = 0;
    if (PyArg_ParseTuple (args, "sp:write", &text, &isErr) == 0)
        return nullptr;

    Append (isErr != 0, text);
    Py_RETURN_NONE;
}

EvpPy_ApiCallFn apiCallFn = nullptr;
EvpPy_FreeFn freeFn = nullptr;

EvpPy_AcquireBufferFn acquireBufferFn = nullptr;
EvpPy_ReleaseBufferFn releaseBufferFn = nullptr;

// ---- zero-copy buffer object ----------------------------------------------
// A minimal buffer-protocol exporter over memory the .apx owns. numpy wraps it
// with np.frombuffer, which makes THIS object the array's `base` — so the token
// (a strong ref to the snapshot) outlives every view derived from it, and the
// memory cannot be freed while a view still points at it.
//
// Deliberately NOT the numpy C API: EvPPy.dll must not link numpy, which may not
// even be installed. The buffer protocol is stdlib, numpy is the caller's problem.

struct EvpBufferObject {
    PyObject_HEAD const void* data;
    Py_ssize_t size;
    void* token; // released on dealloc -> drops our snapshot reference
};

int EvpBuffer_getbuffer (PyObject* exporter, Py_buffer* view, int flags)
{
    EvpBufferObject* const self = (EvpBufferObject*) exporter;
    if (self->data == nullptr) {
        PyErr_SetString (PyExc_ValueError, "this EvP buffer has been released");
        return -1;
    }
    // readonly=1: snapshots are immutable, and a numpy view of one must never
    // be writable — mutating it would corrupt state other readers share.
    return PyBuffer_FillInfo (view, exporter, (void*) self->data, self->size, 1, flags);
}

PyBufferProcs evpBufferProcs = {
    EvpBuffer_getbuffer,
    nullptr // no releasebuffer: FillInfo's default refcounting suffices
};

void EvpBuffer_dealloc (PyObject* selfObj)
{
    EvpBufferObject* const self = (EvpBufferObject*) selfObj;
    if (self->token != nullptr && releaseBufferFn != nullptr)
        releaseBufferFn (self->token); // the last view just died -> let the snapshot go
    self->token = nullptr;
    self->data = nullptr;
    Py_TYPE (selfObj)->tp_free (selfObj);
}

PyTypeObject evpBufferType = {
    PyVarObject_HEAD_INIT (nullptr, 0) "_evp.Buffer", // tp_name
    sizeof (EvpBufferObject),                         // tp_basicsize
};

// _evp.call(command, params_json) -> result_json
//
// The whole Layer 1 bus in one function. Everything a script does to Archicad
// comes through here, which is what makes the trace complete by construction.
PyObject* Evp_Call (PyObject* /*self*/, PyObject* args)
{
    const char* command = nullptr;
    const char* paramsJson = nullptr;
    if (PyArg_ParseTuple (args, "ss:call", &command, &paramsJson) == 0)
        return nullptr;

    if (apiCallFn == nullptr || freeFn == nullptr) {
        PyErr_SetString (PyExc_RuntimeError, "the EvP API bus is not installed");
        return nullptr;
    }

    char* resultJson = nullptr;
    int status = EVPPY_ERROR;

    // Release the GIL across the call: it blocks on a main-thread round trip
    // (~3ms, longer if the user is mid-pick). Holding the GIL through a blocking
    // wait is how embedded interpreters seize up — and the .apx needs no GIL,
    // since no Python runs on the main thread.
    Py_BEGIN_ALLOW_THREADS status = apiCallFn (command, paramsJson, &resultJson);
    Py_END_ALLOW_THREADS

        if (status != EVPPY_OK || resultJson == nullptr)
    {
        if (resultJson != nullptr)
            freeFn (resultJson);
        PyErr_SetString (PyExc_RuntimeError, "the EvP API bus failed to produce a response");
        return nullptr;
    }

    PyObject* const result = PyUnicode_FromString (resultJson);
    freeFn (resultJson); // allocated by the .apx, freed by the .apx
    return result;
}

// _evp.acquire_buffer(request_json) -> (buffer, meta_json)
PyObject* Evp_AcquireBuffer (PyObject* /*self*/, PyObject* args)
{
    const char* requestJson = nullptr;
    if (PyArg_ParseTuple (args, "s:acquire_buffer", &requestJson) == 0)
        return nullptr;

    if (acquireBufferFn == nullptr || releaseBufferFn == nullptr) {
        PyErr_SetString (PyExc_RuntimeError, "the EvP buffer API is not installed");
        return nullptr;
    }

    const void* data = nullptr;
    int64_t size = 0;
    void* token = nullptr;
    char meta[1024] = { 0 };

    // No Py_BEGIN_ALLOW_THREADS: this only takes MeshStore's mutex and copies a
    // pointer — no gate hop, no blocking (snapshots are immutable).
    if (acquireBufferFn (requestJson, &data, &size, meta, (int) sizeof (meta), &token) != EVPPY_OK) {
        PyErr_SetString (PyExc_ValueError, meta[0] != '\0' ? meta : "could not acquire the buffer");
        return nullptr;
    }

    EvpBufferObject* const buffer = PyObject_New (EvpBufferObject, &evpBufferType);
    if (buffer == nullptr) {
        releaseBufferFn (token); // never leak the snapshot reference
        return nullptr;
    }
    buffer->data = data;
    buffer->size = (Py_ssize_t) size;
    buffer->token = token;

    PyObject* const result = Py_BuildValue ("(Os)", (PyObject*) buffer, meta);
    Py_DECREF (buffer); // Py_BuildValue took its own reference
    return result;
}

PyMethodDef evpMethods[] = {
    { "write", Evp_Write, METH_VARARGS, "write(text, is_err) -> None. Route text to the Archicad report window." },
    { "call", Evp_Call, METH_VARARGS, "call(command, params_json) -> result_json. The Layer 1 bus." },
    { "acquire_buffer", Evp_AcquireBuffer, METH_VARARGS,
      "acquire_buffer(request_json) -> (buffer, meta_json). Zero-copy view of the live snapshot." },
    { nullptr, nullptr, 0, nullptr }
};

PyModuleDef evpModule = { PyModuleDef_HEAD_INIT,
                          "_evp",
                          "EvP add-on bridge (P0 spike).",
                          -1, // module state in globals: one interpreter, by design
                          evpMethods,
                          nullptr,
                          nullptr,
                          nullptr,
                          nullptr };

PyObject* PyInit_evp (void)
{
    // Fill the slots here rather than in the designated initializer: the type's
    // layout differs across CPython builds, and only these four matter.
    evpBufferType.tp_flags = Py_TPFLAGS_DEFAULT;
    evpBufferType.tp_doc = "Read-only zero-copy view of EvP snapshot memory.";
    evpBufferType.tp_dealloc = EvpBuffer_dealloc;
    evpBufferType.tp_as_buffer = &evpBufferProcs;
    evpBufferType.tp_new = nullptr; // only EvP creates these
    if (PyType_Ready (&evpBufferType) < 0)
        return nullptr;

    PyObject* const module = PyModule_Create (&evpModule);
    if (module == nullptr)
        return nullptr;

    Py_INCREF (&evpBufferType);
    if (PyModule_AddObject (module, "Buffer", (PyObject*) &evpBufferType) < 0) {
        Py_DECREF (&evpBufferType);
        Py_DECREF (module);
        return nullptr;
    }
    return module;
}

// An embedded process has no stdio to inherit, so sys.stdout/sys.stderr would
// otherwise be None and print() would fail. Keep enough of the file protocol
// that traceback printing is happy too.
const char* const redirectSource = "import sys, _evp\n"
                                   "class _EvPStream:\n"
                                   "    def __init__(self, is_err):\n"
                                   "        self._is_err = is_err\n"
                                   "        self.encoding = 'utf-8'\n"
                                   "        self.errors = 'replace'\n"
                                   "    def write(self, s):\n"
                                   "        if not isinstance(s, str):\n"
                                   "            s = str(s)\n"
                                   "        if s:\n"
                                   "            _evp.write(s, self._is_err)\n"
                                   "        return len(s)\n"
                                   "    def writelines(self, lines):\n"
                                   "        for line in lines:\n"
                                   "            self.write(line)\n"
                                   "    def flush(self):\n"
                                   "        pass\n"
                                   "    def isatty(self):\n"
                                   "        return False\n"
                                   "    def readable(self):\n"
                                   "        return False\n"
                                   "    def writable(self):\n"
                                   "        return True\n"
                                   "    def seekable(self):\n"
                                   "        return False\n"
                                   "    def fileno(self):\n"
                                   "        raise OSError('the EvP console has no file descriptor')\n"
                                   "sys.stdout = _EvPStream(False)\n"
                                   "sys.stderr = _EvPStream(True)\n";

// module_from_spec gives every run a FRESH module namespace, and the sys.modules
// eviction is what makes an edited script take effect on the next run — the
// plan's substitute for finalizing the interpreter, which we never do.
const char* const runnerSource =
    "import importlib.util, json, os, sys\n"
    "def _evp_load(path, name):\n"
    "    spec = importlib.util.spec_from_file_location(name, path)\n"
    "    if spec is None or spec.loader is None:\n"
    "        raise ImportError('cannot load a module spec from ' + path)\n"
    "    module = importlib.util.module_from_spec(spec)\n"
    "    sys.modules[name] = module\n"
    "    spec.loader.exec_module(module)\n"
    "    return module\n"
    "def _evp_run(path, name):\n"
    "    try:\n"
    "        _evp_load(path, name)\n"
    "    finally:\n"
    "        sys.modules.pop(name, None)\n"
    "def _evp_run_command(path, name, params_json, action='', region='', watch_armed=False):\n"
    "    \"\"\"Import command.py and call its run() with the dialog's values.\"\"\"\n"
    "    # WHERE THIS COMMAND'S IMPORTS COME FROM -- its own folder, `_lib/` at the\n"
    "    # scripts root, and opted-in sibling command folders -- is decided in ONE\n"
    "    # place, `evp._commandpath`, which Zone C's runner and the offline dry-run\n"
    "    # harness call too. It used to be spelled out here AND there; when the two\n"
    "    # copies drifted, a helper module worked externally and failed in-process,\n"
    "    # and nothing in the command could tell you why. Read that module for the\n"
    "    # precedence rules and for why eviction below is by FILE LOCATION.\n"
    "    from evp import _commandpath\n"
    "    token = _commandpath.activate(os.path.dirname(path))\n"
    "    try:\n"
    "        module = _evp_load(path, name)\n"
    "        fn = getattr(module, 'run', None)\n"
    "        if fn is None:\n"
    "            raise AttributeError('command.py defines no run()')\n"
    "        # Runtime cross-check: the REAL decorator registers this. The scanner\n"
    "        # read the same thing from the AST without executing anything, so a\n"
    "        # mismatch means the two disagree — catch it here, not in the user's\n"
    "        # model.\n"
    "        if getattr(fn, '__evp_command__', None) is None:\n"
    "            raise TypeError('run() is not decorated with @evp.command')\n"
    "        # E9: a cancel is a CLEAN exit, not an exception to report. Stopping a\n"
    "        # command must not print a traceback the user then reads as a crash, so\n"
    "        # evp.Cancelled is turned into a sentinel the C caller recognises.\n"
    "        # Anything else propagates untouched. Mirrored in _evp_external_main.py\n"
    "        # (which exits 0xE9 instead) — change one, change the other.\n"
    "        try:\n"
    "            # How a command is CALLED -- the signature form vs the schema\n"
    "            # form -- is decided in ONE place, evp._invoke, which the\n"
    "            # external runner and the offline dry-run harness call too.\n"
    "            # Spelling fn(**params) here as well is how two conventions\n"
    "            # drift apart with nothing able to report the difference.\n"
    "            from evp import _invoke\n"
    "            folder = os.path.dirname(path)\n"
    "            # An ACTION is not a run: it acts on what the LAST run\n"
    "            # stored, because re-running the command to export its\n"
    "            # result would repeat every write that run performed.\n"
    "            if action:\n"
    "                return _invoke.run_action(fn, action, folder=folder, region=region)\n"
    "            return _invoke.invoke(fn, json.loads(params_json), folder=folder,\n"
    "                                  watch_armed=bool(watch_armed))\n"
    "        except BaseException as exc:\n"
    "            import evp\n"
    "            if not isinstance(exc, evp.Cancelled):\n"
    "                # The traceback already goes to the transcript. What it does\n"
    "                # NOT say is which Archicad calls preceded it, and that is\n"
    "                # usually the question -- so record both together, in the same\n"
    "                # file the add-on writes its own failures to. Guarded: a\n"
    "                # logging failure must not replace the exception being logged.\n"
    "                try:\n"
    "                    evp.errors.report_exception(exc, where=name)\n"
    "                except Exception:\n"
    "                    pass\n"
    "                raise\n"
    "            print('cancelled: ' + (str(exc) or 'stopped'))\n"
    "            return '__evp_cancelled__'\n"
    "    finally:\n"
    "        sys.modules.pop(name, None)\n"
    "        # Drops the sys.path entries AND evicts every module that came out of\n"
    "        # the scripts root -- the command's helpers, `_lib/`'s modules and any\n"
    "        # exporting sibling's. Without that eviction an edited helper serves its\n"
    "        # stale version for the life of the Archicad session (the interpreter is\n"
    "        # never finalized), and the symptom is an AttributeError on a function\n"
    "        # that plainly exists in the file. Cost a full user round trip on\n"
    "        # SunStudy; see evp/_commandpath.py for why it cannot be symmetric.\n"
    "        _commandpath.deactivate(token)\n";

std::string FormatPyStatus (const char* stage, const PyStatus& status)
{
    std::string message (stage);
    message += ": ";
    message += (status.err_msg != nullptr) ? status.err_msg : "unknown error";
    if (status.func != nullptr) {
        message += " (in ";
        message += status.func;
        message += ")";
    }
    return message;
}

} // namespace

extern "C" __declspec (dllexport) int EvpPy_Initialize (const uint16_t* runtimeHome, EvpPy_ReportFn report,
                                                        char* errorUtf8, int errorSize)
{
    if (initialized)
        return EVPPY_OK;

    if (runtimeHome == nullptr || report == nullptr) {
        SetError (errorUtf8, errorSize, "EvpPy_Initialize: runtimeHome and report are required.");
        return EVPPY_ERROR;
    }
    reportFn = report;

    if (PyImport_AppendInittab ("_evp", PyInit_evp) == -1) { // must precede Py_InitializeFromConfig
        SetError (errorUtf8, errorSize, "PyImport_AppendInittab failed for '_evp'.");
        return EVPPY_ERROR;
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig (&config);
    config.isolated = 1;
    config.use_environment = 0;         // Archicad's PYTHONPATH/PYTHONHOME are not ours
    config.install_signal_handlers = 0; // never steal the host process's handlers

    PyStatus status = PyConfig_SetString (&config, &config.home, (const wchar_t*) runtimeHome);
    if (PyStatus_Exception (status)) {
        SetError (errorUtf8, errorSize, FormatPyStatus ("PyConfig_SetString(home)", status));
        PyConfig_Clear (&config);
        return EVPPY_ERROR;
    }
    status = PyConfig_SetString (&config, &config.program_name, L"EvP");
    if (PyStatus_Exception (status)) {
        SetError (errorUtf8, errorSize, FormatPyStatus ("PyConfig_SetString(program_name)", status));
        PyConfig_Clear (&config);
        return EVPPY_ERROR;
    }

    status = Py_InitializeFromConfig (&config);
    PyConfig_Clear (&config);
    if (PyStatus_Exception (status)) {
        SetError (errorUtf8, errorSize, FormatPyStatus ("Py_InitializeFromConfig", status));
        return EVPPY_ERROR;
    }

    if (PyRun_SimpleString (redirectSource) != 0) {
        SetError (errorUtf8, errorSize, "Failed to install the sys.stdout/sys.stderr redirect.");
        return EVPPY_ERROR;
    }

    runnerGlobals = PyDict_New ();
    if (runnerGlobals == nullptr) {
        SetError (errorUtf8, errorSize, "PyDict_New failed for the runner namespace.");
        return EVPPY_ERROR;
    }
    PyDict_SetItemString (runnerGlobals, "__builtins__", PyEval_GetBuiltins ());

    PyObject* const result = PyRun_String (runnerSource, Py_file_input, runnerGlobals, runnerGlobals);
    if (result == nullptr) {
        PyErr_Print ();
        FlushStreams ();
        SetError (errorUtf8, errorSize, "Failed to define the script runner (traceback in the report window).");
        return EVPPY_ERROR;
    }
    Py_DECREF (result);

    // Py_InitializeFromConfig leaves the GIL HELD by the calling thread. Release
    // it so Zone B's worker can acquire it — without this, PyGILState_Ensure on
    // any other thread blocks forever against the initializing thread.
    // From here on, EVERY entry into Python must bracket itself with
    // PyGILState_Ensure/Release, whichever thread it comes from.
    PyEval_SaveThread ();

    initialized = true;
    return EVPPY_OK;
}

extern "C" __declspec (dllexport) void EvpPy_SetApiCall (EvpPy_ApiCallFn apiCall, EvpPy_FreeFn freeResult)
{
    apiCallFn = apiCall;
    freeFn = freeResult;
}

extern "C" __declspec (dllexport) void EvpPy_SetBufferApi (EvpPy_AcquireBufferFn acquire, EvpPy_ReleaseBufferFn release)
{
    acquireBufferFn = acquire;
    releaseBufferFn = release;
}

extern "C" __declspec (dllexport) int EvpPy_AddSysPathFront (const uint16_t* dir, char* errorUtf8, int errorSize)
{
    if (!initialized) {
        SetError (errorUtf8, errorSize, "EvpPy_AddSysPathFront: the interpreter is not initialized.");
        return EVPPY_ERROR;
    }

    const PyGILState_STATE gil = PyGILState_Ensure ();
    int result_code = EVPPY_OK;

    PyObject* const path = PySys_GetObject ("path"); // borrowed
    PyObject* const entry = PyUnicode_FromWideChar ((const wchar_t*) dir, -1);
    if (path == nullptr || entry == nullptr) {
        PyErr_Clear ();
        SetError (errorUtf8, errorSize, "EvpPy_AddSysPathFront: could not reach sys.path.");
        result_code = EVPPY_ERROR;
    }
    else if (PyList_Insert (path, 0, entry) != 0) { // index 0: precedence over site-packages
        PyErr_Clear ();
        SetError (errorUtf8, errorSize, "EvpPy_AddSysPathFront: sys.path.insert failed.");
        result_code = EVPPY_ERROR;
    }

    Py_XDECREF (entry);
    PyGILState_Release (gil);
    return result_code;
}

extern "C" __declspec (dllexport) int EvpPy_RunCommand (const uint16_t* scriptPath, const char* moduleName,
                                                        const char* paramsJson, const char* actionName,
                                                        const char* menuRegion, int watchArmed, char* errorUtf8,
                                                        int errorSize)
{
    if (!initialized) {
        SetError (errorUtf8, errorSize, "EvpPy_RunCommand: the interpreter is not initialized.");
        return EVPPY_ERROR;
    }

    const PyGILState_STATE gil = PyGILState_Ensure ();
    int result_code = EVPPY_OK;

    PyObject* const runner = PyDict_GetItemString (runnerGlobals, "_evp_run_command"); // borrowed
    PyObject* const path = PyUnicode_FromWideChar ((const wchar_t*) scriptPath, -1);

    if (runner == nullptr || path == nullptr) {
        PyErr_Clear ();
        SetError (errorUtf8, errorSize, "EvpPy_RunCommand: the runner is unavailable.");
        result_code = EVPPY_ERROR;
    }
    else {
        PyObject* const result = PyObject_CallFunction (runner, "Ossssi", path, moduleName, paramsJson,
                                                        actionName == nullptr ? "" : actionName,
                                                        menuRegion == nullptr ? "" : menuRegion, watchArmed);
        if (result == nullptr) {
            PyErr_Print (); // traceback -> sys.stderr -> report callback -> log
            SetError (errorUtf8, errorSize, "the command raised an exception (traceback in the log)");
            result_code = EVPPY_ERROR;
        }
        else {
            // E9 — the runner's cancel sentinel (see _evp_run_command). A string
            // compare, not an exception: the boundary is C, and a Python exception
            // type cannot cross it. run()'s ordinary return value is ignored, so
            // nothing legitimate is being shadowed here.
            if (PyUnicode_Check (result) && PyUnicode_CompareWithASCIIString (result, "__evp_cancelled__") == 0)
                result_code = EVPPY_CANCELLED;
            Py_DECREF (result);
        }
    }

    Py_XDECREF (path);
    FlushStreams ();
    PyGILState_Release (gil);
    return result_code;
}

extern "C" __declspec (dllexport) void EvpPy_FreeString (char* text)
{
    free (text);
}

extern "C" __declspec (dllexport) int EvpPy_ScanCommands (const uint16_t* root, char** jsonOut, char* errorUtf8,
                                                          int errorSize)
{
    if (!initialized) {
        SetError (errorUtf8, errorSize, "EvpPy_ScanCommands: the interpreter is not initialized.");
        return EVPPY_ERROR;
    }
    if (jsonOut == nullptr)
        return EVPPY_ERROR;

    const PyGILState_STATE gil = PyGILState_Ensure ();
    int result_code = EVPPY_ERROR;

    // evp._scanner.scan_root(root) -> dict, then json.dumps it. The scanner only
    // ever ast.parse()s; nothing from the scripts root is imported or executed.
    PyObject* scanner = PyImport_ImportModule ("evp._scanner");
    PyObject* jsonMod = PyImport_ImportModule ("json");
    PyObject* rootStr = PyUnicode_FromWideChar ((const wchar_t*) root, -1);
    PyObject* scanned = nullptr;
    PyObject* dumped = nullptr;

    if (scanner == nullptr || jsonMod == nullptr || rootStr == nullptr) {
        SetError (errorUtf8, errorSize, "EvpPy_ScanCommands: could not import evp._scanner / json.");
    }
    else {
        scanned = PyObject_CallMethod (scanner, "scan_root", "O", rootStr);
        if (scanned == nullptr) {
            PyErr_Print ();
            FlushStreams ();
            SetError (errorUtf8, errorSize, "EvpPy_ScanCommands: scan_root raised (traceback in the log).");
        }
        else {
            dumped = PyObject_CallMethod (jsonMod, "dumps", "O", scanned);
            if (dumped == nullptr) {
                PyErr_Print ();
                FlushStreams ();
                SetError (errorUtf8, errorSize, "EvpPy_ScanCommands: the scan result is not JSON-serializable.");
            }
            else {
                const char* const utf8 = PyUnicode_AsUTF8 (dumped);
                if (utf8 == nullptr) {
                    PyErr_Clear ();
                    SetError (errorUtf8, errorSize, "EvpPy_ScanCommands: could not encode the scan result.");
                }
                else {
                    const size_t length = strlen (utf8);
                    char* const buffer = (char*) malloc (length + 1);
                    if (buffer == nullptr) {
                        SetError (errorUtf8, errorSize, "EvpPy_ScanCommands: out of memory.");
                    }
                    else {
                        memcpy (buffer, utf8, length + 1);
                        *jsonOut = buffer; // freed via EvpPy_FreeString
                        result_code = EVPPY_OK;
                    }
                }
            }
        }
    }

    Py_XDECREF (dumped);
    Py_XDECREF (scanned);
    Py_XDECREF (rootStr);
    Py_XDECREF (jsonMod);
    Py_XDECREF (scanner);
    PyGILState_Release (gil);
    return result_code;
}

// One node-graph script node's body. Drives evp._graphscript, for the same
// reason EvpPy_ScanCommands drives evp._scanner: the interesting logic - a fresh
// namespace, captured output, an enforced time budget, reading the declared
// outputs back - is a dozen lines of Python and several hundred of error-prone C
// API calls, and it is far easier to see that the Python is right.
//
// ⚠️ THE SOURCE IS PASSED IN, NOT READ HERE. The graph runtime has already read
// the file, hashed it and parsed its header. Reading it again would let the file
// change between the two reads, so the node would report ports it did not run.
extern "C" __declspec (dllexport) int EvpPy_RunGraphScript (const char* sourceUtf8, const uint16_t* displayPath,
                                                            const char* inputsJson, const char* outputsJson,
                                                            const char* importRootsJson, int timeBudgetMs,
                                                            char** resultJsonOut, char* errorUtf8, int errorSize)
{
    if (!initialized) {
        SetError (errorUtf8, errorSize, "EvpPy_RunGraphScript: the interpreter is not initialized.");
        return EVPPY_ERROR;
    }
    if (resultJsonOut == nullptr || sourceUtf8 == nullptr)
        return EVPPY_ERROR;
    *resultJsonOut = nullptr;

    const PyGILState_STATE gil = PyGILState_Ensure ();
    int result_code = EVPPY_ERROR;

    PyObject* module = PyImport_ImportModule ("evp._graphscript");
    PyObject* jsonMod = PyImport_ImportModule ("json");
    PyObject* source = nullptr;
    PyObject* path = nullptr;
    PyObject* inputs = nullptr;
    PyObject* outputs = nullptr;
    PyObject* importRoots = nullptr;
    PyObject* produced = nullptr;
    PyObject* dumped = nullptr;

    if (module == nullptr || jsonMod == nullptr) {
        PyErr_Print ();
        SetError (errorUtf8, errorSize, "EvpPy_RunGraphScript: could not import evp._graphscript / json.");
    }
    else {
        source = PyUnicode_FromString (sourceUtf8);
        path = displayPath == nullptr ? PyUnicode_FromString ("")
                                      : PyUnicode_FromWideChar ((const wchar_t*) displayPath, -1);
        inputs = PyObject_CallMethod (jsonMod, "loads", "s", inputsJson == nullptr ? "{}" : inputsJson);
        outputs = PyObject_CallMethod (jsonMod, "loads", "s", outputsJson == nullptr ? "[]" : outputsJson);
        // An absent list is an empty one, not an error: a node whose workspace
        // would not resolve still runs, it simply cannot import its neighbours.
        importRoots = PyObject_CallMethod (jsonMod, "loads", "s", importRootsJson == nullptr ? "[]" : importRootsJson);

        if (source == nullptr || path == nullptr || inputs == nullptr || outputs == nullptr || importRoots == nullptr) {
            PyErr_Clear ();
            SetError (errorUtf8, errorSize, "EvpPy_RunGraphScript: could not decode the request.");
        }
        else {
            produced = PyObject_CallMethod (module, "run", "OOOOOd", source, path, inputs, outputs, importRoots,
                                            (double) timeBudgetMs);
            if (produced == nullptr) {
                // run() is written never to raise, so reaching here means the
                // helper itself is broken rather than the user's script. Reported
                // as such, so nobody spends an afternoon debugging their script.
                PyErr_Print ();
                FlushStreams ();
                SetError (errorUtf8, errorSize,
                          "EvpPy_RunGraphScript: the script runner raised (traceback in the log).");
            }
            else {
                dumped = PyObject_CallMethod (jsonMod, "dumps", "O", produced);
                if (dumped == nullptr) {
                    // A script that assigned something exotic to an output port -
                    // a numpy array, a class instance. Cleared rather than
                    // printed, and reported as the node's own failure, because it
                    // IS the user's mistake and it names what to fix.
                    PyErr_Clear ();
                    SetError (errorUtf8, errorSize,
                              "the script produced a value that cannot leave Python; return numbers, text, "
                              "points or lists of them");
                }
                else {
                    const char* const utf8 = PyUnicode_AsUTF8 (dumped);
                    if (utf8 == nullptr) {
                        PyErr_Clear ();
                        SetError (errorUtf8, errorSize, "EvpPy_RunGraphScript: could not encode the result.");
                    }
                    else {
                        const size_t length = strlen (utf8);
                        char* const buffer = (char*) malloc (length + 1);
                        if (buffer == nullptr) {
                            SetError (errorUtf8, errorSize, "EvpPy_RunGraphScript: out of memory.");
                        }
                        else {
                            memcpy (buffer, utf8, length + 1);
                            *resultJsonOut = buffer; // freed via EvpPy_FreeString
                            result_code = EVPPY_OK;
                        }
                    }
                }
            }
        }
    }

    Py_XDECREF (dumped);
    Py_XDECREF (produced);
    Py_XDECREF (importRoots);
    Py_XDECREF (outputs);
    Py_XDECREF (inputs);
    Py_XDECREF (path);
    Py_XDECREF (source);
    Py_XDECREF (jsonMod);
    Py_XDECREF (module);
    FlushStreams ();
    PyGILState_Release (gil);
    return result_code;
}

extern "C" __declspec (dllexport) int EvpPy_RunScriptFile (const uint16_t* scriptPath, const char* moduleName,
                                                           char* errorUtf8, int errorSize)
{
    if (!initialized) {
        SetError (errorUtf8, errorSize, "EvpPy_RunScriptFile: the interpreter is not initialized.");
        return EVPPY_ERROR;
    }

    // Callable from ANY thread — Zone B's worker, or the main thread. The GIL is
    // released while idle (see EvpPy_Initialize), so acquire it here and hold it
    // for exactly the duration of the script.
    //
    // Note the interaction with the gate: the report callback fires from inside
    // this bracket, i.e. WITH the GIL held, and marshals to Archicad's main
    // thread. That cannot deadlock, because the main thread never asks for the
    // GIL — no Python lives there. If that ever changes, this comment is the
    // first thing to revisit.
    const PyGILState_STATE gil = PyGILState_Ensure ();

    int result_code = EVPPY_OK;

    PyObject* const runner = PyDict_GetItemString (runnerGlobals, "_evp_run"); // borrowed
    if (runner == nullptr) {
        SetError (errorUtf8, errorSize, "_evp_run is missing from the runner namespace.");
        result_code = EVPPY_ERROR;
    }
    else {
        PyObject* const path = PyUnicode_FromWideChar ((const wchar_t*) scriptPath, -1);
        if (path == nullptr) {
            PyErr_Clear ();
            SetError (errorUtf8, errorSize, "Could not convert the script path.");
            result_code = EVPPY_ERROR;
        }
        else {
            PyObject* const result = PyObject_CallFunction (runner, "Os", path, moduleName);
            Py_DECREF (path);

            if (result == nullptr) {
                PyErr_Print (); // traceback -> sys.stderr -> report callback
                SetError (errorUtf8, errorSize, "The script raised an exception (traceback in the console).");
                result_code = EVPPY_ERROR;
            }
            else {
                Py_DECREF (result);
            }
        }
    }

    FlushStreams ();
    PyGILState_Release (gil);
    return result_code;
}
