// ArchViz/LogClaim -- see the header for why a second writer is sent to a file of its own.

#include "ArchViz/LogClaim.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace geomsrv {
namespace archviz {
namespace logclaim {

namespace {

// Where the extension starts, or npos: a dot after the last separator.
size_t ExtensionAt (const std::wstring& path)
{
    const size_t dot = path.find_last_of (L'.');
    const size_t sep = path.find_last_of (L"\\/");
    return dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep) ? dot : std::wstring::npos;
}

std::string Utf8 (const std::wstring& wide)
{
    if (wide.empty ())
        return std::string ();
    const int size = ::WideCharToMultiByte (CP_UTF8, 0, wide.data (), int (wide.size ()), nullptr, 0, nullptr, nullptr);
    std::string text (size_t (size > 0 ? size : 0), '\0');
    if (size > 0)
        ::WideCharToMultiByte (CP_UTF8, 0, wide.data (), int (wide.size ()), text.data (), size, nullptr, nullptr);
    return text;
}

// The file's name, for a line that points at it from beside it.
std::string NameOf (const std::wstring& path)
{
    const size_t sep = path.find_last_of (L"\\/");
    return Utf8 (sep == std::wstring::npos ? path : path.substr (sep + 1));
}

// ⚠️ A WRITER'S OPEN SHARES WRITES, so another process can leave its pointer line.
HANDLE OpenForAppending (const std::wstring& path)
{
    return ::CreateFileW (path.c_str (), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool Append (HANDLE file, const std::string& text)
{
    DWORD written = 0;
    return ::WriteFile (file, text.data (), DWORD (text.size ()), &written, nullptr) != 0 && written == text.size ();
}

} // namespace

std::wstring WriterPath (const std::wstring& base, unsigned n)
{
    if (n <= 1)
        return base;
    const size_t ext = ExtensionAt (base);
    const std::wstring suffix = L"-" + std::to_wstring (n);
    return ext == std::wstring::npos ? base + suffix : base.substr (0, ext) + suffix + base.substr (ext);
}

std::wstring BackupPath (const std::wstring& path)
{
    const size_t ext = ExtensionAt (path);
    return ext == std::wstring::npos ? path + L".1" : path.substr (0, ext) + L".1" + path.substr (ext);
}

Claim Take (const std::wstring& base, const std::string& stamp)
{
    Claim claim;
    std::vector<std::wstring> taken; // written by another process, passed over
    for (unsigned n = 1; n <= kMaxWriters; ++n) {
        const std::wstring path = WriterPath (base, n);
        // ⚠️ THE QUESTION: an open that shares only reads is refused while another
        // process holds the file for writing. Refused for any other reason, no file in
        // this folder can be written either, and the search stops where it would have.
        HANDLE ask = ::CreateFileW (path.c_str (), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ask == INVALID_HANDLE_VALUE) {
            if (::GetLastError () != ERROR_SHARING_VIOLATION)
                return claim;
            taken.push_back (path);
            continue;
        }
        ::CloseHandle (ask);
        HANDLE file = OpenForAppending (path);
        if (file == INVALID_HANDLE_VALUE) {
            if (::GetLastError () != ERROR_SHARING_VIOLATION)
                return claim;
            taken.push_back (path); // taken between the question and the open
            continue;
        }
        claim.file = file;
        claim.path = path;
        claim.writer = n;
        break;
    }
    if (claim.file == nullptr)
        return claim; // every file written by another: nothing to write to, nowhere to say it

    if (!taken.empty ()) {
        const std::string pid = std::to_string (::GetCurrentProcessId ());
        const std::string pointer = stamp + "  LOG          another process with the add-on (pid " + pid +
                                    ") writes its lines to " + NameOf (claim.path) + "\r\n";
        std::string names;
        for (const std::wstring& path : taken) {
            HANDLE other = OpenForAppending (path);
            if (other != INVALID_HANDLE_VALUE) {
                Append (other, pointer);
                ::CloseHandle (other);
            }
            names += (names.empty () ? "" : ", ") + NameOf (path);
        }
        Append (HANDLE (claim.file), stamp + "  LOG          " + names + (taken.size () == 1 ? " is" : " are") +
                                         " written by another process with the add-on; this one (pid " + pid +
                                         ") writes here\r\n");
    }
    LARGE_INTEGER size = {};
    claim.bytes = ::GetFileSizeEx (HANDLE (claim.file), &size) != 0 ? uint64_t (size.QuadPart) : 0;
    return claim;
}

} // namespace logclaim
} // namespace archviz
} // namespace geomsrv
