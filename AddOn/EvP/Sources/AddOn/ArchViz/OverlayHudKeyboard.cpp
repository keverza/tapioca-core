#include "ArchViz/OverlayHudEngine.hpp"

namespace geomsrv::archviz::overlayhud {
void Engine::Impl::KeyboardInput (const Input& input)
{
    auto& io = ImGui::GetIO ();
    // The virtual editor's panel fields need the same queued keys as its canvas shortcuts;
    // VirtualEditActions keeps those shortcuts out while ImGui owns a text field.
    const bool active =
        (store->roofEditor && store->editLocked && store->shown && LayerShown (*store, roofslope::kLayer)) ||
        (store->virtualEditor && store->editLocked && store->shown && LayerShown (*store, editlayers::kLayer)) ||
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
        if (const auto mapped = hudshell::TextKey (event.code); mapped != ImGuiKey_None)
            io.AddKeyEvent (mapped, event.down);
    }
    for (char16_t c : input.characters)
        io.AddInputCharacterUTF16 (c);
}
} // namespace geomsrv::archviz::overlayhud
