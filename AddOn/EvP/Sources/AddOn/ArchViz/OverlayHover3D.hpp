#ifndef EVP_ARCHVIZ_OVERLAYHOVER3D_HPP
#define EVP_ARCHVIZ_OVERLAYHOVER3D_HPP

// ArchViz/OverlayHover3D -- hover mode in the 3D view (the user's stage 3, HANDOFF-OverlayHud
// D19): what is under the pointer there, read for the 3D HUD's layout.
//
// ⚠️ BOUND BY OVERLAY-INVARIANTS.md: it reads the camera the census decoded
// (`freshness::LatestCamera`) and projects through the matrix the overlay's shaders draw
// with (`cameralayout::ViewProjection`, finding 1 and 2); it adds nothing on the render
// thread, decides nothing about the camera, and never reads the plan's transform (§12).
//
// MAIN THREAD, from the 3D HUD's refresh.

#include "ArchViz/OverlayHud.hpp"

namespace geomsrv {
namespace archviz {
namespace overlayhover3d {

// Hover mode is on: the input hook then lays the 3D HUD out at every move over the view.
// Called from the hook: an atomic.
bool Hovering ();

// With hover mode on, what the pointer of `input` is on -- the nearest item of the shown
// layers drawn in 3D, its readout and its tint -- into `input.hover`, `picks` set. Nothing
// while the mode is off. A pointer it cannot read (no camera decoded yet, a camera that
// does not decode as one) reads "nothing", and the reason is said once when it changes.
void Fill (overlayhud::Input& input);

// While the dock's lock is on, the camera as `input.project` -- model metres onto the view through
// the matrix the overlay's shaders draw with (findings 1 and 2) -- for the floor plan picked on the
// view. Nothing when no camera decodes yet.
void Projection (overlayhud::Input& input);

} // namespace overlayhover3d
} // namespace archviz
} // namespace geomsrv

#endif
