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
    // GetTickCount64 at the last navigation event passed to Archicad; 0: none this session.
    uint64_t lastNavigationMs = 0;
    // The locked view's HUD owned the pointer at the last message the thread took, or as it
    // went idle (`NotePointerClaims`): what the pointer shim answers from.
    bool claims = false;
    bool leaveArmed = false; // the timer that tells Archicad the pointer left, after that settle
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

// What the view's HUD is told for its layout (OverlayInputSnapshot.cpp); `take` hands over the
// presses, keys and wheel collected since the last call.
overlayhud::Input InputOf (Target& target, bool take);

// Archicad is navigating this view: its middle button is held, read live, or the last event of
// a navigation passed to it is younger than `kNavigationSettleMs` (OverlayHitMap.hpp), or it is
// in a pointer mode of its own (below).
bool NavigatingNatively (const Target& target);

// A pointer mode of Archicad's on a locked view (`HostMode`, OverlayHitMap.hpp), read live from
// the cursor (OverlayInputHostMode.cpp).
HostMode HostPointerMode (const Target& target);
// A cursor that replaced the HUD's on the locked view: true when it is a navigation cursor
// Archicad set with nothing held, which is then left standing.
bool OfferHostCursor (const Target& target, HCURSOR foreign);
// ⚠️ AFTER A NAVIGATION ARCHICAD BELIEVES THE POINTER RESTS WHERE THE GESTURE ENDED (the user,
// 2026-11-14, #145: the locked view still highlights Archicad's elements and raises its info
// tags). The locked view eats every hover move, so the last position Archicad was told is the
// middle release's or the wheel step's, and it pre-selects the element there. `ArmHostLeave`
// after each navigation event passed on a locked view; when the settle has run out
// `HostLeaveTimer` tells the canvas the pointer left -- as `SetHitMap` does at the moment of
// locking -- and returns true for a timer that was its own. `DisarmHostLeave` with the session.
void ArmHostLeave (Target& target, HWND inputWindow);
bool HostLeaveTimer (HWND inputWindow, WPARAM timer);
void DisarmHostLeave (Target& target, HWND inputWindow);
// Where Archicad is told the pointer is (OverlayInputPointerShim.cpp): its libraries' imports of
// GetCursorPos and GetMessagePos answered by ours, with the message hook and removed with it.
void InstallPointerShim ();
void RemovePointerShim ();
// Each locked view's claim on the pointer, read now: at every message taken and at idle.
void NotePointerClaims ();
// A module's import slot for `function` of `library`, or null; and a compare-and-swap of it.
void** ImportSlot (HMODULE module, const char* library, const char* function);
bool SwapImport (void** slot, void* expected, void* replacement);
// Session state (§8): what was learned of Archicad's cursors, and the mode.
void ForgetHostMode ();

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
