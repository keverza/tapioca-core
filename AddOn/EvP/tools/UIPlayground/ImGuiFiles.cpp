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

// ---- The screenshot's open sections (Host.cpp FullScreenshot) -------------------------------------
// ImGui's item hooks (IMGUI_ENABLE_TEST_ENGINE, the Playground's ImGui only) see every openable item
// as it is submitted. While PlaygroundExpandAll is on, a closed one -- a CollapsingHeader or a tree
// node -- is set open in its window's storage, so the next frame draws it open; turned off, each is
// closed again, so a screenshot leaves the panel as the user had it.
#include <imgui_internal.h>
#include <utility>
#include <vector>

namespace {
bool g_expandAll = false;
std::vector<std::pair<ImGuiStorage*, ImGuiID>> g_expanded;
} // namespace

void PlaygroundExpandAll (bool on)
{
    ImGuiContext& g = *GImGui;
    g_expandAll = on;
    g.TestEngineHookItems = on;
    if (on)
        return;
    for (const auto& [storage, id] : g_expanded)
        storage->SetInt (id, 0);
    g_expanded.clear ();
}

size_t PlaygroundExpandedCount ()
{
    return g_expanded.size ();
}

void ImGuiTestEngineHook_ItemAdd (ImGuiContext*, ImGuiID, const ImRect&, const ImGuiLastItemData*)
{
}

void ImGuiTestEngineHook_ItemInfo (ImGuiContext* ctx, ImGuiID id, const char*, ImGuiItemStatusFlags flags)
{
    if (!g_expandAll || !(flags & ImGuiItemStatusFlags_Openable) || (flags & ImGuiItemStatusFlags_Opened) ||
        !ctx->CurrentWindow)
        return;
    ImGuiStorage* storage = ctx->CurrentWindow->DC.StateStorage;
    storage->SetInt (id, 1);
    g_expanded.emplace_back (storage, id);
}

void ImGuiTestEngineHook_Log (ImGuiContext*, const char*, ...)
{
}

const char* ImGuiTestEngine_FindItemDebugLabel (ImGuiContext*, ImGuiID)
{
    return nullptr;
}
