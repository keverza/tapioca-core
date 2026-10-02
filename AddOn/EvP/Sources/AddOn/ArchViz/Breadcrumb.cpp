// ArchViz/Breadcrumb -- see the header for why the file is held while it stands.

#include "ArchViz/Breadcrumb.hpp"

#include "ArchViz/LogClaim.hpp" // WriterPath

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace geomsrv {
namespace archviz {
namespace breadcrumb {

namespace {

// More than any mode's name; a breadcrumb longer than this is read in part.
constexpr DWORD kMaxContents = 4096;

bool WriteAll (HANDLE file, const std::string& contents)
{
    DWORD written = 0;
    return ::WriteFile (file, contents.data (), DWORD (contents.size ()), &written, nullptr) != 0 &&
           written == contents.size ();
}

// Refused because another process has the file open -- or is deleting it, which
// Windows answers with "access denied" until its last handle closes.
bool HeldByAnother (DWORD error)
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED;
}

} // namespace

std::vector<std::wstring> PathsOf (const std::wstring& base)
{
    std::vector<std::wstring> paths;
    for (unsigned n = 1; n <= kMaxHolders; ++n)
        paths.push_back (logclaim::WriterPath (base, n));
    return paths;
}

Held Hold (const std::wstring& base, const std::string& contents)
{
    for (const std::wstring& path : PathsOf (base)) {
        // ⚠️ SHARING ONLY READS: while this handle is open another process can read what
        // the breadcrumb says, but not take it, delete it or mistake it for a crash.
        HANDLE file = ::CreateFileW (path.c_str (), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (HeldByAnother (::GetLastError ()))
                continue;
            return Held {};
        }
        Held held;
        held.file = file;
        held.path = path;
        if (!WriteAll (file, contents)) {
            Release (held);
            return Held {};
        }
        return held;
    }
    return Held {};
}

bool Rewrite (const Held& held, const std::string& contents)
{
    if (held.file == nullptr)
        return false;
    HANDLE file = HANDLE (held.file);
    LARGE_INTEGER start = {};
    return ::SetFilePointerEx (file, start, nullptr, FILE_BEGIN) != 0 && WriteAll (file, contents) &&
           ::SetEndOfFile (file) != 0;
}

void Release (Held& held)
{
    if (held.file == nullptr)
        return;
    HANDLE file = HANDLE (held.file);
    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile = TRUE;
    const bool marked =
        ::SetFileInformationByHandle (file, FileDispositionInfo, &disposition, sizeof (disposition)) != 0;
    ::CloseHandle (file);
    if (!marked)
        ::DeleteFileW (held.path.c_str ());
    held = Held {};
}

Found Sweep (const std::vector<std::wstring>& paths)
{
    Found found;
    for (const std::wstring& path : paths) {
        // ⚠️ THE QUESTION AND THE DELETE ARE ONE OPEN. Sharing only reads, it is refused
        // while a running process holds the file for writing; granted, the file goes when
        // this handle closes.
        HANDLE file = ::CreateFileW (path.c_str (), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError ();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;
            if (HeldByAnother (error)) {
                ++found.held;
                continue;
            }
            found.left.push_back (std::string ()); // there, and unreadable: still a session's
            ::DeleteFileW (path.c_str ());
            continue;
        }
        std::string contents (kMaxContents, '\0');
        DWORD read = 0;
        if (::ReadFile (file, contents.data (), kMaxContents, &read, nullptr) == 0)
            read = 0;
        contents.resize (read);
        ::CloseHandle (file);
        found.left.push_back (contents);
    }
    return found;
}

} // namespace breadcrumb
} // namespace archviz
} // namespace geomsrv
