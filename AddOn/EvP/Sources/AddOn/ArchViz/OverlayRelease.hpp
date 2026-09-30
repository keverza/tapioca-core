#ifndef EVP_ARCHVIZ_OVERLAYRELEASE_HPP
#define EVP_ARCHVIZ_OVERLAYRELEASE_HPP

// ArchViz/OverlayRelease -- what an overlay the user turned off gives back.
//
// ⚠️ OFF IS OFF (the user, 2026-09-30: an overlay unchecked in the menu is cleared
// completely and takes no resources). A renderer's stop already gives back its hooks and
// its device objects (OVERLAY-INVARIANTS.md §8); these are what the overlay kept beside
// them to start again fast, and each is made again on the next start. A view change
// stops a renderer too and gives back none of this: the overlay is still wanted there.
//
// MAIN THREAD, every one, and only after the renderer it belongs to has stopped.

namespace geomsrv {
namespace archviz {
namespace overlayrelease {

// The model the 3D overlay occludes against, and the extraction still building it --
// unless the portable viewport is up, which draws the same model and owns the
// extraction then. ⚠️ BLOCKS for as long as the extraction takes to stop: at worst a
// slice's timeout (ExtractionThread.hpp), so never from a project event, which must not
// wait on the main thread.
void Model ();

// What the 3D overlay published -- its scene, its HUD, its layers' geometry -- and its
// HUD's engine.
void ThreeD ();

// The plan overlay's HUD engine; its stop gave back everything else.
void Plan ();

// Both overlays off: the layers, the storey slices, the Watch trace's annotations, what
// the user did to the HUD, the laid-out layers and the text engines.
void Shared ();

} // namespace overlayrelease
} // namespace archviz
} // namespace geomsrv

#endif
