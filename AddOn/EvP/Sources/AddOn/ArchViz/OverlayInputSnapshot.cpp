// ⚠️ BOUND BY OVERLAY-INVARIANTS.md (§15, §16). Called from the posted layout message and the
// heartbeat, on the main thread, outside the hook; it reads Win32 state and calls no ACAPI.
// ArchViz/OverlayInputSnapshot -- what a view's HUD is told for its layout: the view's size, the
// pointer, the buttons and keys the input layer took for it, and whether Archicad navigates.
// Cut out of OverlayInput.cpp (the hooks and the routing) at the size cap; no behaviour of its own.

#include "ArchViz/OverlayInputEvents.hpp"
#include "ArchViz/OverlayInputState.hpp"

#include <utility>

namespace geomsrv::archviz::overlayinput::state {

using events::PhysicalButtons;

overlayhud::Input InputOf (Target& target, bool take)
{
    overlayhud::Input input;
    RECT client = {};
    if (target.canvas != nullptr && ::GetClientRect (target.canvas, &client)) {
        input.width = float (client.right - client.left);
        input.height = float (client.bottom - client.top);
    }
    input.pointer = target.inside;
    input.x = target.x;
    input.y = target.y;
    // Coalesced layout messages can sit behind queued moves. Sample the current
    // cursor for hover, but preserve event coordinates when delivering a click.
    POINT screen = {};
    if (target.buttons.empty () && target.canvas != nullptr && ::GetCursorPos (&screen)) {
        const HWND under = ::WindowFromPoint (screen);
        POINT point = screen;
        if (::ScreenToClient (target.canvas, &point)) {
            input.pointer = (under == target.canvas || ::IsChild (target.canvas, under)) && point.x >= 0 &&
                            point.y >= 0 && point.x < input.width && point.y < input.height;
            input.x = float (point.x);
            input.y = float (point.y);
        }
    }
    // Locking routes canvas interactions to us, but does not turn the canvas into a panel.
    input.overHud = input.pointer && target.map.Hit (input.x, input.y, input.width, input.height) >= 0;
    input.hasButtonState = true;
    input.buttonsDown = PhysicalButtons ();
    // ⚠️ AND THROUGH ITS SETTLE (#145): the editor withholds its picks while this is set, so a
    // path under the pointer is not highlighted -- and no redraw asked for -- as Archicad ends.
    input.navigating = target.navigating || NavigatingNatively (target);
    input.navigationRevision = target.navigationRevision;
    input.viewIdentity = uint64_t (reinterpret_cast<uintptr_t> (target.canvas));
    // Read as the layout is: it follows the press that asked for it, the key held through both.
    input.shift = ::GetKeyState (VK_SHIFT) < 0;
    input.ctrl = ::GetKeyState (VK_CONTROL) < 0;
    if (take) {
        input.buttons = std::move (target.buttons);
        input.editActions = std::move (target.editActions);
        input.keys = std::move (target.keys);
        input.characters = std::move (target.characters);
        target.keys.clear (), target.characters.clear ();
        target.editActions.clear ();
        target.buttons.clear ();
        input.wheel = target.wheel;
        target.wheel = 0.0f;
    }
    return input;
}

} // namespace geomsrv::archviz::overlayinput::state
