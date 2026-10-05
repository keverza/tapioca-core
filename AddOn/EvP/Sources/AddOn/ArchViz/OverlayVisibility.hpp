#ifndef EVP_ARCHVIZ_OVERLAYVISIBILITY_HPP
#define EVP_ARCHVIZ_OVERLAYVISIBILITY_HPP

// ArchViz/OverlayVisibility -- whether the overlays' content is drawn at all: the dock's
// circle (OverlayHud.hpp), shown or hidden (the user, 2026-09-30: toggle the whole overlay
// and its HUD without destroying them).
//
// ⚠️ READ AT PRESENT, SO AN ATOMIC. The composers read it inside Archicad's Present, where
// OVERLAY-INVARIANTS.md §11 allows no lock: hidden, they draw nothing but the HUD -- which
// is then only the dock's tab, the way back -- and render no occluder for it. Nothing is
// released or rebuilt: shown again, the next Present draws what was there.
//
// Set on the main thread by the controller from the HUD's state (FollowHudState), and
// reset with it: a project closing, an overlay turned off, the unload.

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace overlayvisibility {

inline std::atomic<bool>& ContentFlag ()
{
    static std::atomic<bool> shown { true };
    return shown;
}

inline bool ContentShown ()
{
    return ContentFlag ().load (std::memory_order_acquire);
}

inline void SetContentShown (bool shown)
{
    ContentFlag ().store (shown, std::memory_order_release);
}

// Existing-model reference edges only. Keep occluder geometry and analysis layers
// active when the reference is hidden, in both Present paths.
inline std::atomic<bool>& WireframeFlag ()
{
    static std::atomic<bool> shown { true };
    return shown;
}

inline bool WireframeShown ()
{
    return WireframeFlag ().load (std::memory_order_acquire);
}

inline void SetWireframeShown (bool shown)
{
    WireframeFlag ().store (shown, std::memory_order_release);
}

// ⚠️ HOVER MODE (OverlayHud.hpp `HoverMode`), READ BY THE INPUT HOOK: on, a move anywhere
// on the view asks for the HUD's layout -- what is under the pointer -- and is still
// Archicad's. Off, only moves over the HUD do. Same source, same follower.
inline std::atomic<bool>& HoverFlag ()
{
    static std::atomic<bool> on { false };
    return on;
}

inline bool Hovering ()
{
    return HoverFlag ().load (std::memory_order_acquire);
}

inline void SetHovering (bool on)
{
    HoverFlag ().store (on, std::memory_order_release);
}

} // namespace overlayvisibility
} // namespace archviz
} // namespace geomsrv

#endif
