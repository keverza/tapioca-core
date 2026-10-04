#ifndef EVP_ARCHVIZ_SURFACESWITCH_HPP
#define EVP_ARCHVIZ_SURFACESWITCH_HPP

// ArchViz/SurfaceSwitch -- the overlay or the separate viewer, never both (the user,
// 2026-10-03: "Overlay and viewer should not be possible to open both at same time. Its either
// overlay or viewer.").
//
// ⚠️ ONE OR THE OTHER, WHOEVER ASKS. The HUD's dock is a switch -- the overlay's circle at its
// top, the viewer's at its bottom -- and the menu items are the other way in: starting an
// overlay closes the viewer (`BeforeOverlayStarts`), opening the viewer turns the overlays off
// (`BeforeViewerOpens`). From the floor plan the viewer opens over the plan, from the 3D window
// over the model; from the viewer the overlay of the view in front starts.
//
// ⚠️ A SWITCH ASKED FROM A HUD HAPPENS AFTER THE HUD HAS RETURNED. The overlay's HUD is laid out
// inside the input layer's message, and stopping the overlay there would take the input layer
// away under its own feet; the viewer's HUD draws on the render thread, which may neither call
// ACAPI nor stop itself. So a request is posted to a message-only window of the main thread
// and performed from the top of its message loop -- the shape the input layer and CameraWake
// already have.
//
// ⚠️ THE WINDOW IS MADE ON THE MAIN THREAD AND OUTLIVES NOTHING: `Arm` before anything may ask
// from another thread, `Shutdown` at the unload -- a window whose procedure lives in an unloaded
// DLL is Windows calling into freed code.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace surfaceswitch {

enum class Surface : uint8_t { Overlay = 1, Viewer = 2 };

// MAIN THREAD. Make the window requests are posted to. Idempotent.
void Arm ();
// MAIN THREAD, at the unload: the window and its class go.
void Shutdown ();

// Any thread: switch to `to` from the main thread's message loop. A request before `Arm` --
// or after `Shutdown` -- is dropped and counted.
void Request (Surface to);

// MAIN THREAD. The exclusion, for the menu items and the verbs: an overlay about to start
// closes the viewer; the viewer about to open turns both overlays off.
void BeforeOverlayStarts ();
void BeforeViewerOpens ();
// MAIN THREAD. Whether the separate viewer is open: the Diligent viewport running in its palette.
bool ViewerOpen ();

struct Stats {
    uint64_t requested = 0;
    uint64_t performed = 0;
    uint64_t dropped = 0; // asked with no window to post to
};
Stats GetStats ();

} // namespace surfaceswitch
} // namespace archviz
} // namespace geomsrv

#endif
