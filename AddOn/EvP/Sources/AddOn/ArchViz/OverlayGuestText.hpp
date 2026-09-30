#ifndef EVP_ARCHVIZ_OVERLAYGUESTTEXT_HPP
#define EVP_ARCHVIZ_OVERLAYGUESTTEXT_HPP

// ArchViz/OverlayGuestText -- the overlays' one text engine (OverlayText.hpp), made with
// the bundled font on first use: both overlays lay out with it, so a glyph rasterised
// for the plan is not rasterised again for 3D -- and a HUD engine (OverlayHud.hpp) per
// view.
//
// ⚠️ ONE HUD ENGINE PER VIEW, NOT ONE FOR BOTH. An ImGui context holds what the pointer
// hovers and presses. The 3D HUD is laid out on every layer change even while the plan
// is in front; in one context that layout would take the plan's hover and a press in
// progress away from it. What the user DID -- the panel closed, a section folded -- is one
// state both engines keep.
//
// MAIN THREAD ONLY -- the engine is.

#include "ArchViz/OverlayHitMap.hpp" // overlayinput::View

#include <cstddef>
#include <memory>
#include <string>

namespace geomsrv {
namespace archviz {

namespace overlaytext {
class Engine;
}
namespace overlayhud {
class Engine;
struct State;
} // namespace overlayhud

namespace guesttext {

// The engine, or null when the bundled font could not be read or its seed atlas not
// built -- said once in archviz.log. The first call pays for the seed atlas.
overlaytext::Engine* Engine ();

// The engine for the font file at `path` (overlayfonts::Resolve made it), made on first
// use with a small seed; the bundled font's for an empty path. Null when the file
// cannot be read or `kMaxFonts` are already made -- said once per path.
constexpr size_t kMaxFonts = 8;
overlaytext::Engine* EngineFor (const std::string& path);

// The view's HUD engine, or null when it could not start -- said once. The two keep
// what the user does to the panels in one state: the panel closed in 3D is closed in plan.
overlayhud::Engine* Hud (overlayinput::View view);

// What the user did to the panels, both views' (OverlayHud.hpp `State`); made on first use.
std::shared_ptr<overlayhud::State> HudState ();

// What the user did to the panels forgotten, and the events that said so: the project
// whose layers they were closed (§8).
void ForgetHudState ();

// ⚠️ LET GO WHEN THE OVERLAY IS TURNED OFF (OverlayRelease.hpp), each made again on its
// next use. The view's HUD engine -- its ImGui context and its atlas: that view's overlay
// is off. The text engines -- the bundled font's and the callers' fonts: both are off,
// and nothing laid out with them is kept (overlayscene::ForgetDrafts first).
void ReleaseHud (overlayinput::View view);
void ReleaseText ();

} // namespace guesttext
} // namespace archviz
} // namespace geomsrv

#endif
