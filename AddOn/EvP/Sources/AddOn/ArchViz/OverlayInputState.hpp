#ifndef EVP_ARCHVIZ_OVERLAYINPUTSTATE_HPP
#define EVP_ARCHVIZ_OVERLAYINPUTSTATE_HPP

// ⚠️ BOUND BY OVERLAY-INVARIANTS.md. What the HUD's input layer (OverlayInput.cpp) and the
// cursor over the HUD (OverlayInputCursor.cpp) share: each view's target, the one gesture
// latch and the counters. Not an interface: only those two files include it.
#include "ArchViz/OverlayInput.hpp"

#include <string>
#include <vector>

namespace geomsrv::archviz::overlayinput::state {

struct Target {
    HWND canvas = nullptr;
    HudOwner owner;
    HitMap map;
    // The pointer, in the canvas's client pixels, and whether it is over the canvas.
    bool inside = false;
    float x = 0.0f;
    float y = 0.0f;
    bool wasOver = false; // over the HUD at the last message: leaving it redraws the hover
    bool navigating = false;
    uint64_t navigationRevision = 0;
    std::vector<overlayhud::Input::Button> buttons;
    std::vector<overlayhud::Input::EditAction> editActions;
    std::vector<overlayhud::Input::Key> keys;
    std::u16string characters;
    float wheel = 0.0f; // notches turned over a page that scrolls, since the last layout took them
    bool refreshPending = false;
    bool timerArmed = false;
    uint64_t lastRedrawMs = 0;
    // The canvas belongs to this thread (the hook sees its messages only then), and its
    // WM_SETCURSOR is answered by the subclass below.
    bool ownThread = false;
    bool subclassed = false;
};

extern Target targets[2];
extern Router router;
extern Stats stats;

View ViewOf (const Target& target);
Target* TargetFor (HWND window);

// The cursor over the HUD (OverlayInputCursor.cpp).
// The HUD owns what is under the pointer now: the canvas itself is there -- not a palette
// floating over it -- and a region of the last layout, or a gesture the HUD holds.
bool OwnsPointer (const Target& target);
// Shows the HUD's cursor when the pointer is its; false when Archicad's stands.
bool ShowCursor (const Target& target);
// Puts the HUD's cursor back on a locked view it owns if Archicad replaced it; counted.
void ReassertCursor (const Target& target);
// The canvas's subclass, which answers WM_SETCURSOR and swallows Archicad's context menu.
void Subclass (Target& target, View view);
void Unsubclass (Target& target);
// The subclass procedures belong to comctl32: resolved again at the next attach.
void ForgetSubclassing ();

} // namespace geomsrv::archviz::overlayinput::state
#endif
