#ifndef EVP_ARCHVIZ_INSTRUCTIONBANNER_HPP
#define EVP_ARCHVIZ_INSTRUCTIONBANNER_HPP

// ArchViz/InstructionBanner — one line of text across the top of the overlay's
// HUD, with an optional live countdown (PLAT-RE111).
//
// ⚠️ IT IS THE ONLY CHANNEL THAT REACHES THE USER WHILE THEY NAVIGATE.
// Archicad's DG palette does not repaint during a navigation drag, so the
// palette status line a command writes to is frozen for the whole gesture --
// which is exactly the interval a measurement run needs to talk during, and the
// log file is read long afterwards. The overlay renders every frame regardless.
// Reported from a live run: with the wheel button held for a pan, the palette
// line stops updating and the user cannot see what to do next.
//
// ⚠️ IT HOLDS A DEADLINE, NOT A COUNT, and clears itself when that deadline
// passes. The countdown therefore ticks in real time while the setting thread
// sleeps through the gesture it just announced, and a command that dies mid-run
// cannot strand an instruction over the drawing.
//
// ⚠️ ITS OWN FILE, NOT DiligentViewportSupport's, ONLY BECAUSE OF AN INCLUDE
// CYCLE: the viewport HOLDS one, and Support's header already includes the
// viewport's. Nothing else about it wants to be separate.

#include "ArchViz/DiligentHud.hpp"   // HudState

#include <chrono>
#include <mutex>
#include <string>

namespace geomsrv {
namespace archviz {

class InstructionBanner {
public:
    // Any thread. Empty text hides it; a negative `seconds` means no countdown.
    void Set (const std::string& text, double seconds);
    // Render thread, once per frame, before the HUD is drawn.
    void PublishTo (HudState& hud);

private:
    std::mutex mutex_;
    std::string text_;
    std::chrono::steady_clock::time_point deadline_ {};
    bool hasDeadline_ = false;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
