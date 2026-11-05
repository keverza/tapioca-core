#ifndef EVP_ARCHVIZ_OVERLAYINPUTKEYBOARD_HPP
#define EVP_ARCHVIZ_OVERLAYINPUTKEYBOARD_HPP
#include "ArchViz/OverlayHud.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace geomsrv::archviz::overlayinput::keyboard {
using Sink = bool (*) (HWND window, const overlayhud::Input::Key& key, const std::u16string& text, bool take);
// Thread-local and scoped by Sink. Runs before host accelerator translation,
// unlike a canvas-only WM_KEYDOWN rewrite. No focus steal, ACAPI or ImGui here.
bool Install (Sink sink);
void Stop ();
bool Installed ();
void Message (MSG& message, bool take);
// ⚠️ A MODIFIER'S AUTO-REPEAT ASKS FOR NO LAYOUT (2026-11-05 13:46-13:48): Shift+middle orbits
// the view, and Shift held repeats ~30 times a second; over the locked view each repeat cost a
// ~9 ms layout on the main thread while Archicad navigated (NAVWATCH: 94 in 5 s). It carries no
// new state -- the HUD reads a key's press, never its repeat -- so it is still taken and queued;
// only the layout is not asked for.
inline bool IsModifierRepeat (const overlayhud::Input::Key& key)
{
    const uint32_t c = key.code;
    const bool modifier = c == VK_SHIFT || c == VK_LSHIFT || c == VK_RSHIFT || c == VK_CONTROL || c == VK_LCONTROL ||
                          c == VK_RCONTROL || c == VK_MENU || c == VK_LMENU || c == VK_RMENU;
    return modifier && key.down && key.repeat;
}
} // namespace geomsrv::archviz::overlayinput::keyboard
#endif
