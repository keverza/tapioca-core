#ifndef EVP_ARCHVIZ_OVERLAYHUDEVENTS_HPP
#define EVP_ARCHVIZ_OVERLAYHUDEVENTS_HPP

// ArchViz/OverlayHudEvents -- what the user changed on the overlays' HUD, for Python: a
// panel docked or opened, a section folded, the text size, a control's new value. The HUD
// engines (OverlayHud.hpp) report each change as they lay out; a caller asks for those
// after the last it saw (`Tapioca.OverlayHudEvents`).
//
// ⚠️ A SEQUENCE, NOT A CALLBACK. The change happens inside Archicad's message dispatch on
// the main thread, where a script cannot run; Python polls from its own thread. Each
// event has a number one above the last; a caller keeps the last it saw and asks for the
// ones after it -- the shape of `Tapioca.GraphGetEvents`.
//
// ⚠️ A RING OF `kCapacity`. A caller that fell behind by more is told so (`gap`) rather
// than handed a tail that looks complete: it reads the state again (`Tapioca.OverlayHud`)
// instead of stitching.
//
// ⚠️ ANY THREAD, ONE MUTEX. Pushed on the main thread after the HUD's ImGui lock is let
// go, read on the server's thread without the main thread (the verb is gate-free). No
// hook ever touches it.
//
// Pure: no Win32, tests/cpp builds it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhudevents {

constexpr size_t kCapacity = 512;

struct Event {
    uint64_t seq = 0;    // one above the event before it
    uint64_t timeMs = 0; // wall clock, milliseconds since 1970 UTC: what time.time() says x 1000
    std::string view;    // "3d" or "plan": where the user did it
    std::string kind;    // "hud" (opened, closed), "panel" (its tab), "section", "fontScale", a control's
    std::string layer;   // the panel's layer; empty for the HUD's own (the text size)
    int32_t panel = -1;  // its place among that layer's panels
    std::string title;   // the panel's title
    std::string id;      // the control: a section's title, a control's id
    int32_t item = -1;   // its place among the panel's items
    double value = 0.0;  // what it is now: 1 open, 0 closed; the scale; the value
    std::string text;    // what that says: "open", "closed", "110 %", a tab's title, an option
    bool final = true;   // false while a slider is still being dragged
};

// `event` numbered and stamped, at the ring's end. Returns its number.
uint64_t Push (Event event);

struct Tail {
    uint64_t lastSeq = 0;      // the newest event's number; 0 while there is none
    bool gap = false;          // events after `since` fell off the ring before this read
    std::vector<Event> events; // after `since`, oldest first, at most the number asked
};
Tail Since (uint64_t since, size_t max);

uint64_t LastSeq ();

// The project whose layers the events named closed (§8): the events go, the numbers go on.
// A caller that had seen them all reads nothing new; one behind reads a gap.
void Clear ();

} // namespace overlayhudevents
} // namespace archviz
} // namespace geomsrv

#endif
