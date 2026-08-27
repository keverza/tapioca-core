#include "Preview/GhPreviewSegmentView.hpp"

#include <windows.h>

namespace evp::preview {

namespace {

std::string DescribeLastError (DWORD code)
{
    // The numeric code, always. "Access is denied" alone has sent people
    // reinstalling Rhino before now; the number is what a support question can
    // be answered from.
    LPWSTR text = nullptr;
    const DWORD length =
        FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        nullptr, code, 0, (LPWSTR) &text, 0, nullptr);

    std::string message = "Windows error " + std::to_string ((unsigned long) code);
    if (length > 0 && text != nullptr) {
        std::string narrow;
        narrow.reserve (length);
        for (DWORD index = 0; index < length; ++index) {
            const wchar_t character = text[index];
            if (character == L'\r' || character == L'\n')
                continue;
            narrow.push_back (character < 128 ? (char) character : '?');
        }
        while (!narrow.empty () && narrow.back () == ' ')
            narrow.pop_back ();
        if (!narrow.empty ())
            message += " (" + narrow + ")";
    }
    if (text != nullptr)
        LocalFree (text);
    return message;
}

std::wstring Widen (const std::string& text)
{
    // AcceptablePreviewSegmentName has already restricted this to letters,
    // digits, '.', '_' and '-', so the widening is exact rather than a
    // code-page guess.
    return std::wstring (text.begin (), text.end ());
}

} // namespace

GhPreviewSegmentView::~GhPreviewSegmentView ()
{
    Close ();
}

bool GhPreviewSegmentView::Open (const std::string& name, uint32_t declaredBytes, std::string& error)
{
    Close ();

    if (declaredBytes == 0u) {
        error = "A preview batch asked for a segment of no bytes.";
        return false;
    }

    // ⚠️ NAME CHECKED BEFORE THE OPEN, BY THE CALLER. GhPreviewIngest runs
    // AcceptablePreviewSegmentName first; this is the second gate rather than
    // the first, so that a future caller cannot reach OpenFileMappingW with a
    // name the worker chose freely.
    std::string nameError;
    if (!AcceptablePreviewSegmentName (name, nameError)) {
        error = nameError;
        return false;
    }

    const std::wstring wide = Widen (name);
    HANDLE opened = OpenFileMappingW (FILE_MAP_READ, FALSE, wide.c_str ());
    if (opened == nullptr) {
        error = "The preview batch's shared memory \"" + name +
                "\" could not be opened: " + DescribeLastError (GetLastError ()) +
                ". The worker creates it and holds it until the batch is acknowledged, so this usually means the "
                "worker died mid-batch.";
        return false;
    }

    const void* mapped = MapViewOfFile (opened, FILE_MAP_READ, 0, 0, 0);
    if (mapped == nullptr) {
        error = "The preview batch's shared memory \"" + name +
                "\" was opened but could not be mapped: " + DescribeLastError (GetLastError ()) + ".";
        CloseHandle (opened);
        return false;
    }

    // ⚠️ WHAT WAS MAPPED, NOT WHAT WAS CLAIMED. Every offset in the batch was
    // validated against `declaredBytes`; if the section object is smaller than
    // that, each of those validated offsets is an over-read waiting to happen.
    // MapViewOfFile with a zero size maps the whole section, and VirtualQuery is
    // how big that turned out to be.
    MEMORY_BASIC_INFORMATION info {};
    const SIZE_T queried = VirtualQuery (mapped, &info, sizeof (info));
    const std::size_t mappedBytes = queried == 0 ? 0 : (std::size_t) info.RegionSize;
    if (mappedBytes < (std::size_t) declaredBytes) {
        error = "The preview batch declared a " + std::to_string (declaredBytes) + "-byte segment and \"" + name +
                "\" holds " + std::to_string (mappedBytes) + ".";
        UnmapViewOfFile (mapped);
        CloseHandle (opened);
        return false;
    }

    mapping = opened;
    view = (const uint8_t*) mapped;
    // The DECLARED size, not the mapped one: a section object is rounded up to a
    // page, and the bytes past the declaration are not the worker's batch. Every
    // range check downstream is against this.
    bytes = (std::size_t) declaredBytes;
    return true;
}

const uint8_t* GhPreviewSegmentView::Data () const
{
    return view;
}

std::size_t GhPreviewSegmentView::Size () const
{
    return bytes;
}

void GhPreviewSegmentView::Close ()
{
    if (view != nullptr) {
        UnmapViewOfFile (view);
        view = nullptr;
    }
    if (mapping != nullptr) {
        CloseHandle ((HANDLE) mapping);
        mapping = nullptr;
    }
    bytes = 0;
}

} // namespace evp::preview
