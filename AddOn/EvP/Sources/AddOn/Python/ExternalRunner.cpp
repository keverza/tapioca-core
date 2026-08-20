#include "APIEnvir.h" // Win32Interface only — no ACAPinc.h here, on purpose
#include "ExternalRunner.hpp"
#include "PathUtils.hpp"
#include "RunCancel.hpp" // E9 — the running command's cancel token

#include <string>
#include <vector>

// See PathUtils.cpp for the /Zc:wchar_t- situation. Short version: GS::uchar_t and
// wchar_t are the same type under this flag, so UniString::ToUStr() is already a
// valid LPCWSTR, and <fstream>-style wide-path C++ I/O is unavailable — raw Win32.

namespace evp {

namespace {

// E9 — the exit code that means "this run was cancelled", used both as the code we
// TerminateProcess with and the one _evp_external_main.py exits with when run()
// raises evp.Cancelled, so a kill and a cooperative stop are indistinguishable to
// the caller. Kept under 256 so it survives POSIX-style low-byte truncation of
// sys.exit() unchanged; 0xE9 is a nod to the roadmap item. Change it here and in
// _evp_external_main.py together.
constexpr DWORD ExternalCancelExitCode = 0xE9;

// How long a cancelled subprocess gets to stop ITSELF before it is killed.
//
// Measured, not guessed: CancelProbeExternal's first run showed the kill beating
// the child's own checkpoint every time, so the cooperative path never executed —
// the transcript ended mid-tick with no "STOPPED" line and no NOW LOOK block. A
// killed process runs no `finally`, so a command holding a half-written DXF would
// leave it half-written. The grace window lets a well-behaved command notice the
// cancel on its next poll and unwind properly; only one that will not stop gets
// TerminateProcess. Sized above the ~1s poll cadence a checkpoint loop typically
// has, and small enough that Stop still feels immediate on a wedged child.
constexpr int64_t ExternalCancelGraceMs = 2000;

// GetTickCount64, not <chrono>: this file is deliberately raw Win32 (see the
// header), and a monotonic millisecond counter is all the grace window needs.
int64_t NowMs ()
{
    return (int64_t) GetTickCount64 ();
}

// A child-inheritable pipe. The parent end is marked NON-inheritable: if the child
// inherited its own read end of stdout, the pipe would never report EOF and the
// drain below would block forever after the child exits.
struct Pipe {
    HANDLE read = nullptr;
    HANDLE write = nullptr;

    bool Create (bool parentKeepsRead)
    {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof (sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe (&read, &write, &sa, 0))
            return false;
        return SetHandleInformation (parentKeepsRead ? read : write, HANDLE_FLAG_INHERIT, 0) != 0;
    }

    ~Pipe ()
    {
        if (read != nullptr)
            CloseHandle (read);
        if (write != nullptr)
            CloseHandle (write);
    }

    void CloseWrite ()
    {
        if (write != nullptr) {
            CloseHandle (write);
            write = nullptr;
        }
    }
    void CloseRead ()
    {
        if (read != nullptr) {
            CloseHandle (read);
            read = nullptr;
        }
    }
};

// The child's environment: ours, plus EvP's own variables. A block of
// "NAME=VALUE\0NAME=VALUE\0\0".
//
// EVP_ACTION rides here rather than in the params on stdin for the same reason
// the embedded runner takes it as its own argument: stdin carries the USER's
// values, and nothing else should ever have to be filtered back out of them.
std::vector<wchar_t> BuildEnvironment (const GS::UniString& endpoint, const GS::UniString& commandDir,
                                       const GS::UniString& packageDir, const GS::UniString& action)
{
    std::vector<wchar_t> block;

    const wchar_t* const parent = GetEnvironmentStringsW ();
    if (parent != nullptr) {
        const wchar_t* cursor = parent;
        while (*cursor != L'\0') {
            const size_t length = wcslen (cursor);
            // Drop any inherited copies of the names we are about to set, so ours
            // are unambiguous rather than duplicated.
            const bool ours =
                _wcsnicmp (cursor, L"EVP_ENDPOINT=", 13) == 0 || _wcsnicmp (cursor, L"EVP_COMMAND_DIR=", 16) == 0 ||
                _wcsnicmp (cursor, L"EVP_ACTION=", 11) == 0 || _wcsnicmp (cursor, L"PYTHONPATH=", 11) == 0;
            if (!ours)
                block.insert (block.end (), cursor, cursor + length + 1);
            cursor += length + 1;
        }
        FreeEnvironmentStringsW ((LPWCH) parent);
    }

    auto append = [&block] (const GS::UniString& entry) {
        const wchar_t* const text = (const wchar_t*) entry.ToUStr ().Get ();
        block.insert (block.end (), text, text + wcslen (text) + 1);
    };

    append (GS::UniString ("EVP_ENDPOINT=") + endpoint);
    append (GS::UniString ("EVP_COMMAND_DIR=") + commandDir);
    // Always set, empty for an ordinary run - so an inherited stale value can
    // never turn a Run into an export.
    append (GS::UniString ("EVP_ACTION=") + action);
    // PYTHONPATH carries ONLY the package dir. The command folder is appended to
    // sys.path by the entry script instead, so it lands AFTER this one and can
    // never shadow `evp` itself.
    append (GS::UniString ("PYTHONPATH=") + packageDir);

    block.push_back (L'\0'); // terminate the block
    return block;
}

} // namespace

bool RunCommandExternal (const GS::UniString& folder, const GS::UniString& paramsJson, const GS::UniString& action,
                         unsigned short port, const GS::UniString& runtimeHome, const GS::UniString& packageDir,
                         uint64_t runGeneration, GS::UniString& output, bool& cancelled, GS::UniString& error)
{
    cancelled = false;

    const GS::UniString exe (runtimeHome + GS::UniString ("\\python.exe"));
    if (!PathExists (exe)) {
        // Name the path. "python.exe not found" with no path is exactly the
        // information-free failure this project bans.
        error = "No python.exe at " + exe +
                ". The embedded runtime resolved to a "
                "folder holding python312.dll but no interpreter executable, so "
                "runtime=\"external\" cannot run. (A full CPython install has one; "
                "the embeddable distribution the P4 bootstrap installs also does.)";
        return false;
    }

    const GS::UniString entry (packageDir + GS::UniString ("\\_evp_external_main.py"));
    if (!PathExists (entry)) {
        error = "No _evp_external_main.py at " + entry + " — the evp package staging is incomplete.";
        return false;
    }

    Pipe in, out;
    if (!in.Create (false) || !out.Create (true)) {
        error = "Could not create pipes for the external runtime.";
        return false;
    }

    // -u: unbuffered, so the console sees output as it happens rather than in one
    // lump when the process exits (or not at all, if it crashes).
    // -s: no user site-packages. Plain python.exe would otherwise add the user's
    // %APPDATA%\Roaming\Python\...\site-packages ahead of the runtime's own, letting
    // a stray user install shadow the managed env — the embedded zone already has
    // user-site off (isolated config), so this keeps the two zones identical.
    GS::UniString commandLine (GS::UniString ("\"") + exe + "\" -s -u \"" + entry + "\"");
    std::vector<wchar_t> mutableCommandLine;
    {
        const wchar_t* const text = (const wchar_t*) commandLine.ToUStr ().Get ();
        mutableCommandLine.assign (text, text + wcslen (text) + 1); // CreateProcessW may write to it
    }

    std::vector<wchar_t> environment =
        BuildEnvironment (GS::UniString::Printf ("http://127.0.0.1:%d", (int) port), folder, packageDir, action);

    STARTUPINFOW startup = {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in.read;
    startup.hStdOutput = out.write;
    startup.hStdError = out.write; // one stream: interleaving is the true order

    PROCESS_INFORMATION process = {};
    const BOOL started = CreateProcessW ((LPCWSTR) exe.ToUStr ().Get (), mutableCommandLine.data (), nullptr, nullptr,
                                         TRUE, // inherit the pipe handles
                                         CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data (),
                                         (LPCWSTR) folder.ToUStr ().Get (), &startup, &process);

    if (!started) {
        error =
            GS::UniString::Printf ("Could not start %T (Win32 error %u).", exe.ToPrintf (), (unsigned) GetLastError ());
        return false;
    }

    // The parent must drop the child's ends, or the reads below never see EOF.
    in.CloseRead ();
    out.CloseWrite ();

    // Params over stdin, then EOF — the entry script reads until EOF.
    {
        const auto utf8 = paramsJson.ToCStr (0, MaxUSize, CC_UTF8);
        const DWORD length = (DWORD) strlen (utf8.Get ());
        DWORD written = 0;
        WriteFile (in.write, utf8.Get (), length, &written, nullptr);
        in.CloseWrite (); // the EOF the child is waiting for
    }

    // Drain until EOF. This is also what waits for the process: reading to EOF and
    // THEN waiting can't deadlock on a full pipe buffer, whereas waiting first can.
    //
    // E9 — PeekNamedPipe + a short sleep instead of a blocking ReadFile. A blocking
    // read cannot be interrupted, which is what made the old
    // WaitForSingleObject(..., INFINITE) below unstoppable: a wedged child held this
    // worker forever with no way for Stop, a panel close or timeout_s to reach it.
    // Peeking costs one syscall per 20ms of idle and keeps the drain exactly as it
    // was whenever there IS data. (PeekNamedPipe works on the anonymous pipes
    // CreatePipe hands out; it returns 0 when the write end is gone, which is EOF.)
    std::string transcript;
    bool exited = false;
    bool killed = false;
    int64_t cancelledAtMs = 0; // when the cancel was first seen; 0 == not yet
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe (out.read, nullptr, 0, nullptr, &available, nullptr) == 0)
            break; // write end closed: EOF

        if (available > 0) {
            char buffer[4096];
            DWORD read = 0;
            const DWORD want = (available < sizeof (buffer)) ? available : (DWORD) sizeof (buffer);
            if (ReadFile (out.read, buffer, want, &read, nullptr) == 0 || read == 0)
                break;
            transcript.append (buffer, read);
            continue; // keep reading while data is flowing
        }

        // Nothing buffered. If the process is already gone AND the pipe stayed
        // empty across a full pass, everything it wrote has been collected.
        if (exited)
            break;
        if (WaitForSingleObject (process.hProcess, 0) == WAIT_OBJECT_0) {
            exited = true;
            continue; // one more pass to sweep the pipe
        }

        if (RunCancel::Get ().IsCancelled (runGeneration)) {
            cancelled = true;
            if (cancelledAtMs == 0) {
                // First notice. Do NOT kill yet — the child polls the same token
                // over the HTTP bus and may be about to exit cleanly on its own,
                // running its finally blocks on the way out. Start the clock.
                cancelledAtMs = NowMs ();
            }
            else if (!killed && (NowMs () - cancelledAtMs) > ExternalCancelGraceMs) {
                // It had its chance. Zone C's whole reason for existing is that a
                // subprocess CAN be killed — this is where that promise is kept.
                // The loop continues either way, so the transcript written before
                // the kill is still drained.
                killed = true;
                TerminateProcess (process.hProcess, ExternalCancelExitCode);
            }
            Sleep (20);
            continue;
        }

        Sleep (20); // idle: ~50 wakeups/s on a quiet child, none while it talks
    }
    out.CloseRead ();

    // Bounded, not INFINITE: by here the child has closed its pipe or been killed,
    // so this only collects the exit code. A stuck handle must not re-wedge us.
    WaitForSingleObject (process.hProcess, 5000);

    DWORD exitCode = 0;
    GetExitCodeProcess (process.hProcess, &exitCode);
    CloseHandle (process.hThread);
    CloseHandle (process.hProcess);

    output = GS::UniString (transcript.c_str (), CC_UTF8);

    if (cancelled || exitCode == ExternalCancelExitCode) {
        // A cancel is a clean outcome, not a failure — no error text, and the
        // caller reports "cancelled". Also catches the child exiting with this
        // code by itself: _evp_external_main.py uses it when run() raises
        // evp.Cancelled, so a cooperative stop and a kill report identically.
        cancelled = true;
        return true;
    }

    if (exitCode != 0) {
        error = GS::UniString::Printf ("The external runtime exited with code %u.", (unsigned) exitCode);
        return false;
    }
    return true;
}

// ---- E7 environment manager (evp/_env.py) ---------------------------------
//
// Reconciles a command's `requires` (op="ensure") or rebuilds the whole managed
// env (op="reset"). Runs the runtime's python.exe against evp/_env.py as a
// STANDALONE FILE — _env.py imports only the stdlib, so this needs no PYTHONPATH
// and runs -s -E (fully hermetic, even the reset path works with site-packages
// torn down). stdout carries the single JSON result line; stderr carries pip's
// progress. Drained on SEPARATE pipes so pip noise never corrupts the JSON.
bool RunEnvManager (const GS::UniString& op, const GS::Array<GS::UniString>& requirements,
                    const GS::UniString& runtimeHome, const GS::UniString& packageDir, GS::UniString& resultJson,
                    GS::UniString& progress, GS::UniString& error)
{
    const GS::UniString exe (runtimeHome + GS::UniString ("\\python.exe"));
    if (!PathExists (exe)) {
        error = "No python.exe at " + exe +
                ". Run Install-Runtime.ps1 to populate "
                "%LOCALAPPDATA%\\EvP\\runtime before commands can install dependencies.";
        return false;
    }
    const GS::UniString script (packageDir + GS::UniString ("\\evp\\_env.py"));
    if (!PathExists (script)) {
        error = "No _env.py at " + script + " — the evp package staging is incomplete.";
        return false;
    }

    // python.exe -s -E "<pkg>\evp\_env.py" <op> --require r1 --require r2 ...
    GS::UniString commandLine (GS::UniString ("\"") + exe + "\" -s -E \"" + script + "\" " + op);
    for (const GS::UniString& r : requirements)
        commandLine += GS::UniString (" --require \"") + r + "\"";

    std::vector<wchar_t> mutableCommandLine;
    {
        const wchar_t* const text = (const wchar_t*) commandLine.ToUStr ().Get ();
        mutableCommandLine.assign (text, text + wcslen (text) + 1);
    }

    Pipe out, err;
    if (!out.Create (true) || !err.Create (true)) {
        error = "Could not create pipes for the environment manager.";
        return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = out.write;
    startup.hStdError = err.write; // separate: JSON on stdout must stay clean

    PROCESS_INFORMATION process = {};
    const BOOL started =
        CreateProcessW ((LPCWSTR) exe.ToUStr ().Get (), mutableCommandLine.data (), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr /* -E ignores env anyway */,
                        (LPCWSTR) runtimeHome.ToUStr ().Get (), &startup, &process);

    if (!started) {
        error =
            GS::UniString::Printf ("Could not start %T (Win32 error %u).", exe.ToPrintf (), (unsigned) GetLastError ());
        return false;
    }

    out.CloseWrite ();
    err.CloseWrite ();

    // Drain stderr FIRST to EOF, then stdout: _env.py flushes stderr (pip) before it
    // writes the final JSON line to stdout, and both pipes have ample buffer for the
    // volumes here (a pip log + a one-line result), so sequential draining cannot
    // deadlock. Keeping them ordered avoids interleaving a partial JSON read.
    auto drain = [] (HANDLE h) {
        std::string acc;
        for (;;) {
            char buffer[4096];
            DWORD read = 0;
            if (ReadFile (h, buffer, sizeof (buffer), &read, nullptr) == 0 || read == 0)
                break;
            acc.append (buffer, read);
        }
        return acc;
    };
    const std::string errText = drain (err.read);
    const std::string outText = drain (out.read);
    out.CloseRead ();
    err.CloseRead ();

    WaitForSingleObject (process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess (process.hProcess, &exitCode);
    CloseHandle (process.hThread);
    CloseHandle (process.hProcess);

    resultJson = GS::UniString (outText.c_str (), CC_UTF8);
    progress = GS::UniString (errText.c_str (), CC_UTF8);

    if (exitCode != 0) {
        // _env.py still prints a JSON {ok:false,error} on failure; surface a short
        // reason here and leave the detail (pip log) for the caller to log.
        error = "the environment manager reported a failure (see the command log)";
        return false;
    }
    return true;
}

} // namespace evp
