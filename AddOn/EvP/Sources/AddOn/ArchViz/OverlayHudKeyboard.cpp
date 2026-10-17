#include "ArchViz/OverlayHudEngine.hpp"

namespace geomsrv::archviz::overlayhud {
namespace {
ImGuiKey Key (uint32_t code)
{
    if (code >= 'A' && code <= 'Z')
        return ImGuiKey (ImGuiKey_A + code - 'A');
    if (code >= '0' && code <= '9')
        return ImGuiKey (ImGuiKey_0 + code - '0');
    if (code >= 0x70 && code <= 0x87)
        return ImGuiKey (ImGuiKey_F1 + code - 0x70);
    switch (code) {
        case 0x08:
            return ImGuiKey_Backspace;
        case 0x09:
            return ImGuiKey_Tab;
        case 0x0D:
            return ImGuiKey_Enter;
        case 0x1B:
            return ImGuiKey_Escape;
        case 0x20:
            return ImGuiKey_Space;
        case 0x21:
            return ImGuiKey_PageUp;
        case 0x22:
            return ImGuiKey_PageDown;
        case 0x23:
            return ImGuiKey_End;
        case 0x24:
            return ImGuiKey_Home;
        case 0x25:
            return ImGuiKey_LeftArrow;
        case 0x26:
            return ImGuiKey_UpArrow;
        case 0x27:
            return ImGuiKey_RightArrow;
        case 0x28:
            return ImGuiKey_DownArrow;
        case 0x2D:
            return ImGuiKey_Insert;
        case 0x2E:
            return ImGuiKey_Delete;
        default:
            return ImGuiKey_None;
    }
}
} // namespace
void Engine::Impl::KeyboardInput (const Input& input)
{
    auto& io = ImGui::GetIO ();
    const bool active =
        (store->roofEditor && store->editLocked && store->shown && LayerShown (*store, roofslope::kLayer)) ||
        CriteriaTextActive (*store);
    if (roofKeysActive && !active)
        io.ClearInputKeys ();
    roofKeysActive = active;
    if (!active)
        return;
    for (const auto& event : input.keys) {
        io.AddKeyEvent (ImGuiMod_Ctrl, event.ctrl);
        io.AddKeyEvent (ImGuiMod_Shift, event.shift);
        io.AddKeyEvent (ImGuiMod_Alt, event.alt);
        if (const auto mapped = Key (event.code); mapped != ImGuiKey_None)
            io.AddKeyEvent (mapped, event.down);
    }
    for (char16_t c : input.characters)
        io.AddInputCharacterUTF16 (c);
}
} // namespace geomsrv::archviz::overlayhud
