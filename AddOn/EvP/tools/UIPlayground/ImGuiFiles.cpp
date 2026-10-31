// Diligent's user config disables stdio callbacks. Keep those callbacks inside the
// shared ImGui too; neither the host nor a reloadable module supplies file ownership.
#include <imgui.h>
#include <cstdio>

ImFileHandle ImFileOpen (const char* name, const char* mode)
{
    return std::fopen (name, mode);
}
bool ImFileClose (ImFileHandle file)
{
    return std::fclose (static_cast<FILE*> (file)) == 0;
}
ImU64 ImFileGetSize (ImFileHandle file)
{
    auto* stream = static_cast<FILE*> (file);
    const auto position = _ftelli64 (stream);
    if (position < 0 || _fseeki64 (stream, 0, SEEK_END) != 0)
        return ImU64 (-1);
    const auto size = _ftelli64 (stream);
    _fseeki64 (stream, position, SEEK_SET);
    return ImU64 (size);
}
ImU64 ImFileRead (void* data, ImU64 size, ImU64 count, ImFileHandle file)
{
    return std::fread (data, size_t (size), size_t (count), static_cast<FILE*> (file));
}
ImU64 ImFileWrite (const void* data, ImU64 size, ImU64 count, ImFileHandle file)
{
    return std::fwrite (data, size_t (size), size_t (count), static_cast<FILE*> (file));
}
