#include "APIEnvir.h" // Win32Interface only — no ACAPinc.h here, on purpose
#include "PathUtils.hpp"

#include <string>
#include <vector>

// ============================================================================
// The /Zc:wchar_t- situation, stated once so nobody re-derives it:
//
//   * The SDK forces /Zc:wchar_t-, which makes `wchar_t` a typedef for
//     `unsigned short` rather than a distinct builtin type.
//   * GS::uchar_t is `UInt16` (GSRoot/Definitions.hpp) — THE SAME TYPE. So
//     GS::UniString::ToUStr() already hands us a valid `const wchar_t*` for
//     Win32. No conversion, no copy. (Verified offline with the real flags.)
//   * The same flag forbids <fstream> with wide paths here: basic_filebuf
//     bottoms out in the exported, C++-mangled std::_Fiopen(const wchar_t*),
//     which under this flag names a symbol the prebuilt CRT does not export.
//     Hence raw CreateFileW below rather than std::ofstream.
//   * `a + b` on UniString yields the lazy GS::UniString::Concatenation, which
//     has NO ToUStr() — always materialize into a UniString first.
// ============================================================================

namespace evp {

bool ReadEnv (const wchar_t* name, GS::UniString& value)
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
    value = GS::UniString (buffer.data ());
    return true;
}

bool PathExists (const GS::UniString& path)
{
    return GetFileAttributesW ((LPCWSTR) path.ToUStr ().Get ()) != INVALID_FILE_ATTRIBUTES;
}

bool CreateDirectoryChain (const GS::UniString& dir)
{
    if (PathExists (dir))
        return true;

    const UIndex separator = dir.FindLast ('\\');
    if (separator != MaxUIndex && separator > 2) {
        if (!CreateDirectoryChain (dir.GetSubstring (0, separator)))
            return false;
    }
    return CreateDirectoryW ((LPCWSTR) dir.ToUStr ().Get (), nullptr) != 0 || GetLastError () == ERROR_ALREADY_EXISTS;
}

bool WriteTextFile (const GS::UniString& path, const char* utf8, GS::UniString& error)
{
    const HANDLE file = CreateFileW ((LPCWSTR) path.ToUStr ().Get (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Could not create " + path;
        return false;
    }
    DWORD written = 0;
    const DWORD length = (DWORD) strlen (utf8);
    const bool ok = WriteFile (file, utf8, length, &written, nullptr) != 0 && written == length;
    CloseHandle (file);
    if (!ok)
        error = "Could not write " + path;
    return ok;
}

bool WriteTextFile (const GS::UniString& path, const GS::UniString& text, GS::UniString& error)
{
    const auto utf8 = text.ToCStr (0, MaxUSize, CC_UTF8);
    return WriteTextFile (path, utf8.Get (), error);
}

bool ReadTextFile (const GS::UniString& path, GS::UniString& text)
{
    const HANDLE file = CreateFileW ((LPCWSTR) path.ToUStr ().Get (), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const DWORD size = GetFileSize (file, nullptr);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) { // a settings file, not a payload
        CloseHandle (file);
        return false;
    }

    std::vector<char> buffer ((size_t) size + 1, '\0');
    DWORD read = 0;
    const bool ok = ReadFile (file, buffer.data (), size, &read, nullptr) != 0;
    CloseHandle (file);
    if (!ok)
        return false;

    buffer[read] = '\0';
    text = GS::UniString (buffer.data (), CC_UTF8);
    return true;
}

// Size-triggered, single-generation log rotation. Cap matches evp.paths
// (LOG_CAP_BYTES = 5 MiB) so the Python-side probe logs and the shared host logs
// (commands.log, scan.log) stay bounded the same way. When path is at/over the
// cap, rename it to "<name>.1<ext>" (discarding any older .1) BEFORE the next line
// is appended — never truncate mid-file, a partial line hides evidence.
static const DWORD kLogCapBytes = 5u * 1024u * 1024u;

static void RotateIfOversized (const GS::UniString& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW ((LPCWSTR) path.ToUStr ().Get (), GetFileExInfoStandard, &info) == 0)
        return; // does not exist yet — nothing to rotate
    if (info.nFileSizeHigh == 0 && info.nFileSizeLow < kLogCapBytes)
        return;

    // Insert ".1" before the extension: "...\commands.log" -> "...\commands.1.log".
    const UIndex dot = path.FindLast ('.');
    const UIndex sep = path.FindLast ('\\');
    GS::UniString backup;
    if (dot != MaxUIndex && (sep == MaxUIndex || dot > sep))
        backup = path.GetSubstring (0, dot) + GS::UniString (".1") + path.GetSubstring (dot, path.GetLength () - dot);
    else
        backup = path + GS::UniString (".1");

    DeleteFileW ((LPCWSTR) backup.ToUStr ().Get ()); // discard the older generation (ok if absent)
    MoveFileW ((LPCWSTR) path.ToUStr ().Get (), (LPCWSTR) backup.ToUStr ().Get ());
}

bool AppendTextLine (const GS::UniString& path, const GS::UniString& line)
{
    RotateIfOversized (path);

    // FILE_SHARE_READ so the log can be tailed while a run is in flight (or
    // read after a force-quit).
    const HANDLE file = CreateFileW ((LPCWSTR) path.ToUStr ().Get (), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const GS::UniString terminated (line + GS::UniString ("\r\n"));
    const auto utf8 = terminated.ToCStr (0, MaxUSize, CC_UTF8);
    DWORD written = 0;
    const bool ok = WriteFile (file, utf8.Get (), (DWORD) strlen (utf8.Get ()), &written, nullptr) != 0;
    FlushFileBuffers (file);
    CloseHandle (file);
    return ok;
}

GS::UniString EvpDataDir ()
{
    GS::UniString localAppData;
    if (!ReadEnv (L"LOCALAPPDATA", localAppData))
        return GS::UniString ();
    const GS::UniString tapiocaDir (localAppData + GS::UniString ("\\Tapioca"));
    const GS::UniString legacyDir (localAppData + GS::UniString ("\\EvP"));
    if (!PathExists (tapiocaDir) && PathExists (legacyDir)) {
        // Same volume in normal use; COPY_ALLOWED retains the user's state even
        // if Windows resolves the locations differently. Best effort: startup
        // must still work if an antivirus or a stale process holds a file open.
        MoveFileExW ((LPCWSTR) legacyDir.ToUStr ().Get (), (LPCWSTR) tapiocaDir.ToUStr ().Get (),
                     MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
    }
    return tapiocaDir;
}

GS::UniString ScanLogPath ()
{
    return EvpDataDir () + GS::UniString ("\\logs\\scan.log");
}

void StartupTrace (const GS::UniString& message)
{
    const GS::UniString dataDir = EvpDataDir ();
    if (dataDir.IsEmpty ())
        return; // no %LOCALAPPDATA%: nothing to write to, and
                // failing here would help nobody
    const GS::UniString logs (dataDir + GS::UniString ("\\logs"));
    if (!CreateDirectoryChain (logs))
        return;
    AppendTextLine (logs + GS::UniString ("\\startup.log"), GS::UniString ("  . ") + message);
}

} // namespace evp
