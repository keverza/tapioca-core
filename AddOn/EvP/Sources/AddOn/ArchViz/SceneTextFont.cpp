// ArchViz/SceneTextFont -- see the header. Moved verbatim from SceneTextLayer.cpp,
// where it was file-local, so the overlays' text engine reads the same font.

#include "ArchViz/SceneTextFont.hpp"

#include <windows.h>

#include <cstring>

namespace geomsrv::archviz {

namespace {

constexpr int kSceneTextFontResourceId = 32581;

} // namespace

bool LoadBundledSceneTextFont (std::vector<uint8_t>& bytes, std::string& error)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR> (&LoadBundledSceneTextFont), &module)) {
        error = "could not resolve the add-on module for the bundled text font";
        return false;
    }
    const HRSRC resource = FindResourceW (module, MAKEINTRESOURCEW (kSceneTextFontResourceId), L"DATA");
    const DWORD resourceSize = resource != nullptr ? SizeofResource (module, resource) : 0;
    const HGLOBAL loaded = resource != nullptr ? LoadResource (module, resource) : nullptr;
    const void* data = loaded != nullptr ? LockResource (loaded) : nullptr;
    if (data == nullptr || resourceSize < sizeof (uint32_t)) {
        error = "bundled Noto Sans resource 32581 is missing or unreadable";
        return false;
    }
    uint32_t payloadSize = 0;
    std::memcpy (&payloadSize, data, sizeof (payloadSize));
    if (payloadSize == 0 || payloadSize > resourceSize - sizeof (payloadSize)) {
        error = "bundled Noto Sans resource has an invalid payload size";
        return false;
    }
    const uint8_t* payload = static_cast<const uint8_t*> (data) + sizeof (payloadSize);
    bytes.assign (payload, payload + payloadSize);
    return true;
}

} // namespace geomsrv::archviz
