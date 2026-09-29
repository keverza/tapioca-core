// ArchViz/OverlayFonts -- see the header.

#include "ArchViz/OverlayFonts.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace geomsrv {
namespace archviz {
namespace overlayfonts {

namespace {

// A font file is at most this: the largest CJK collections are about 30 MB.
constexpr std::streamoff kMaxFontBytes = 64ll * 1024 * 1024;

std::string Lower (std::string text)
{
    std::transform (text.begin (), text.end (), text.begin (),
                    [] (unsigned char c) { return char (std::tolower (c)); });
    return text;
}

std::string Trimmed (const std::string& text)
{
    const size_t first = text.find_first_not_of (" \t");
    const size_t last = text.find_last_not_of (" \t");
    return first == std::string::npos ? std::string () : text.substr (first, last - first + 1);
}

std::wstring Wide (const std::string& utf8)
{
    if (utf8.empty ())
        return std::wstring ();
    const int length = ::MultiByteToWideChar (CP_UTF8, 0, utf8.data (), int (utf8.size ()), nullptr, 0);
    std::wstring out (size_t (length > 0 ? length : 0), L'\0');
    if (length > 0)
        ::MultiByteToWideChar (CP_UTF8, 0, utf8.data (), int (utf8.size ()), out.data (), length);
    return out;
}

std::string Utf8 (const std::wstring& wide)
{
    if (wide.empty ())
        return std::string ();
    const int length =
        ::WideCharToMultiByte (CP_UTF8, 0, wide.data (), int (wide.size ()), nullptr, 0, nullptr, nullptr);
    std::string out (size_t (length > 0 ? length : 0), '\0');
    if (length > 0)
        ::WideCharToMultiByte (CP_UTF8, 0, wide.data (), int (wide.size ()), out.data (), length, nullptr, nullptr);
    return out;
}

bool IsFile (const std::string& path)
{
    const DWORD attributes = ::GetFileAttributesW (Wide (path).c_str ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string FontsFolder ()
{
    wchar_t windows[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW (windows, MAX_PATH);
    return length > 0 && length < MAX_PATH ? Utf8 (windows) + "\\Fonts" : std::string ("C:\\Windows\\Fonts");
}

// The file an installed face `requested` is in, from one hive's font list.
bool Find (HKEY hive, const std::string& requested, std::string& path)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW (hive, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return false;
    bool found = false;
    for (DWORD index = 0; !found; ++index) {
        wchar_t name[512] = {};
        wchar_t data[MAX_PATH * 2] = {};
        DWORD nameLength = DWORD (std::size (name)), dataBytes = DWORD (sizeof (data) - sizeof (wchar_t)), type = 0;
        const LONG status =
            ::RegEnumValueW (key, index, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*> (data), &dataBytes);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            continue;
        if (!Lists (Utf8 (name), requested))
            continue;
        std::string file = Utf8 (data);
        // A bare file name lives in the Windows fonts folder; a user's own font is a
        // full path.
        if (file.find ('\\') == std::string::npos && file.find ('/') == std::string::npos)
            file = FontsFolder () + "\\" + file;
        if (IsFile (file)) {
            path = file;
            found = true;
        }
    }
    ::RegCloseKey (key);
    return found;
}

} // namespace

bool Lists (const std::string& valueName, const std::string& requested)
{
    const std::string wanted = Lower (Trimmed (requested));
    if (wanted.empty ())
        return false;
    // "Name (TrueType)": the part before the last parenthesis is the faces.
    std::string faces = valueName;
    const size_t open = faces.rfind (" (");
    if (open != std::string::npos && !faces.empty () && faces.back () == ')')
        faces = faces.substr (0, open);
    size_t begin = 0;
    while (begin <= faces.size ()) {
        const size_t amp = faces.find (" & ", begin);
        const std::string face = faces.substr (begin, amp == std::string::npos ? std::string::npos : amp - begin);
        if (Lower (Trimmed (face)) == wanted)
            return true;
        if (amp == std::string::npos)
            break;
        begin = amp + 3;
    }
    return false;
}

bool Resolve (const std::string& name, std::string& path, std::string& error)
{
    const std::string requested = Trimmed (name);
    if (requested.empty ()) {
        error = "a font is an installed family's name or a font file's path";
        return false;
    }
    if (IsFile (requested)) {
        path = requested;
        return true;
    }
    if (Find (HKEY_CURRENT_USER, requested, path) || Find (HKEY_LOCAL_MACHINE, requested, path))
        return true;
    error = "no font \"" + requested +
            "\" is installed -- name it as Windows lists it (\"Arial\", \"Segoe UI Semibold\") or give a .ttf, "
            ".otf or .ttc path";
    return false;
}

bool Read (const std::string& path, std::vector<uint8_t>& bytes, std::string& error)
{
    std::ifstream in (Wide (path), std::ios::binary | std::ios::ate);
    if (!in) {
        error = "the font file \"" + path + "\" could not be opened";
        return false;
    }
    const std::streamoff size = in.tellg ();
    if (size <= 0 || size > kMaxFontBytes) {
        error = "the font file \"" + path + "\" is empty or larger than 64 MB";
        return false;
    }
    bytes.resize (size_t (size));
    in.seekg (0);
    if (!in.read (reinterpret_cast<char*> (bytes.data ()), size)) {
        error = "the font file \"" + path + "\" could not be read";
        return false;
    }
    return true;
}

} // namespace overlayfonts
} // namespace archviz
} // namespace geomsrv
