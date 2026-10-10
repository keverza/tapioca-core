#ifndef EVP_ARCHVIZ_OVERLAYHITMAP_HPP
#define EVP_ARCHVIZ_OVERLAYHITMAP_HPP

// ArchViz/OverlayHitMap -- where the overlays' HUD is on the view, and whose a mouse
// gesture is: the HUD's or Archicad's (OverlayInput.hpp takes the messages).
//
// ⚠️ THE AREA UNDER THE HUD IS NOT CLICK-THROUGH (the user, 2026-09-29). A click meant
// to toggle something in a panel must never also select the element behind it. So a
// press over a panel or a legend is the HUD's, and so is a bare move there -- Archicad
// would otherwise pre-select and put its info tags under the panel.
//
// ⚠️ THE GESTURE BELONGS TO WHERE IT BEGAN. A press over the HUD is the HUD's until
// its release, wherever the pointer goes; a press anywhere else is Archicad's until its
// release, even across the HUD -- a wall drawn over a panel still draws. A gesture
// whose release was never seen (the button came up outside every window) ends at the
// first move with no button held.
//
// ⚠️ NAVIGATION IS NEVER TAKEN. The wheel and the middle button pan and zoom over the
// HUD as anywhere else -- but for the wheel over a panel whose page scrolls (`Region::scrolls`;
// the user, 2026-10-03: a vertical scroll bar when a page is taller than the view): there it
// scrolls the page, as over any scrolling list, and the view does not zoom under it.
//
// Pure: no Win32, tests/cpp builds it.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayinput {

// The two overlays' views; each has its own HUD, its own layout state and its own input.
enum class View : uint8_t { ThreeD = 0, Plan = 1 };

// A popup is the whole view while a dropdown's list is open: the click that closes it is
// the HUD's too, as a list closing on a click outside it eats that click.
enum class RegionKind : uint8_t { Panel = 0, Legend = 1, Dock = 2, Popup = 3 };

// A rectangle of the view that is the HUD's, anchored as its drawing is: `fraction` of
// the view, then `rect` (left, top, right, bottom) in pixels from there -- physical
// pixels, or logical ones the view's DPI scale multiplies (`logical`: a legend's, as
// the vertex shader scales its offsets).
struct Region {
    RegionKind kind = RegionKind::Panel;
    std::string layer; // the layer it is drawn for
    uint32_t item = 0; // which of that layer's panels or legends
    float fraction[2] = { 0.0f, 0.0f };
    float rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool logical = false;
    // A legend's colour bar inside it, the same way: what the HUD hovers for a value.
    float bar[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // A panel whose page is taller than it: the wheel over it scrolls the page.
    bool scrolls = false;
};

struct HitMap {
    std::vector<Region> regions; // a later one is drawn over an earlier one
    float dpiScale = 1.0f;
    // At the last layout the pointer was on something it can press: a hand over the HUD,
    // not an arrow.
    bool hand = false;
    // The cursor the HUD asked for there (`Cursor`): a canvas that is one pressable item says
    // its own (HudShell `OwnCursor`); otherwise a hand or an arrow as `hand` says.
    uint8_t cursor = 0;
    // ⚠️ LOCKED, THE VIEW IS THE HUD'S (the user, 2026-10-10: edit the floor plan on the view, not
    // Archicad's elements): a press anywhere on it is the HUD's, and a move with nothing held;
    // the wheel and the middle button still navigate (`Router`).
    bool locked = false;

    // The index of the topmost region holding the view point (x, y) of a view `width`
    // by `height` pixels; -1 for none.
    int Hit (float x, float y, float width, float height) const;
    // The point is the HUD's: on a region of it, or anywhere while the view is locked.
    bool Owns (float x, float y, float width, float height) const
    {
        return locked || Hit (x, y, width, height) >= 0;
    }
};

// ⚠️ THE POINTER SAYS WHAT A PRESS WILL DO (the user, 2026-10-10: over the plan canvas it kept
// switching between a hand and an arrow): an arrow, a hand on what can be pressed, a move cross
// on what drags, sizing arrows across a wall.
enum class Cursor : uint8_t { Arrow = 0, Hand = 1, Move = 2, SizeWE = 3, SizeNS = 4 };

enum class Button : uint8_t { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4 };
enum class EventKind : uint8_t { Move = 0, Press = 1, Release = 2, Wheel = 3 };

// The buttons held, one bit per `Button`, as Windows reports them WITH the event: a
// press includes its button, a release no longer does.
constexpr uint32_t Bit (Button button)
{
    return 1u << uint32_t (button);
}

struct Event {
    EventKind kind = EventKind::Move;
    Button button = Button::Left; // a press's or a release's
    uint32_t held = 0;
    float wheel = 0.0f; // the wheel's notches, positive turned away from the user; 0 sideways
};

enum class Route : uint8_t { Pass = 0, Take = 1 };
enum class Owner : uint8_t { None = 0, Hud = 1, Host = 2 };

class Router {
  public:
    // A message being removed from the queue: its route, the latch moved by it. `overScroll`:
    // the pointer is over a region that scrolls (`Region::scrolls`).
    Route Decide (const Event& event, bool overHud, bool overScroll = false);
    // A message only peeked at: the route it would get, the latch untouched -- it
    // comes back to be removed.
    Route Preview (const Event& event, bool overHud, bool overScroll = false) const;
    Owner GetOwner () const
    {
        return owner_;
    }
    void Reset ()
    {
        owner_ = Owner::None;
    }

  private:
    static Route Verdict (const Event& event, bool overHud, bool overScroll, Owner& owner);
    Owner owner_ = Owner::None;
};

} // namespace overlayinput
} // namespace archviz
} // namespace geomsrv

#endif
