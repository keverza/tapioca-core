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
// ⚠️ SHIFT IS ARCHICAD'S TOO (2026-11-14 10:36-10:52, #145). Shift+middle orbits the view, and
// over the locked view the HUD took every key, Shift's press and release included: Archicad saw
// its orbit modifier go down and up only through the mouse. With the redraws held (no redraw of
// ours within 500 ms of 15 of 19 jumps) the camera still jumped 30-106 deg, 109-312 ms after a
// middle release -- and each of those followed a Shift key-up the HUD had taken by 78-453 ms.
// So Shift is queued for the HUD as before AND left to Archicad: `QueueKey` answers "not taken".
// Only Shift: Ctrl and Alt alone change Archicad's tool or open its menu bar, and nothing
// measured names them.
inline bool PassesToHost (const overlayhud::Input::Key& key)
{
    return key.code == VK_SHIFT || key.code == VK_LSHIFT || key.code == VK_RSHIFT;
}
inline bool IsModifierRepeat (const overlayhud::Input::Key& key)
{
    const uint32_t c = key.code;
    const bool modifier = c == VK_SHIFT || c == VK_LSHIFT || c == VK_RSHIFT || c == VK_CONTROL || c == VK_LCONTROL ||
                          c == VK_RCONTROL || c == VK_MENU || c == VK_LMENU || c == VK_RMENU;
    return modifier && key.down && key.repeat;
}
} // namespace geomsrv::archviz::overlayinput::keyboard
#endif
