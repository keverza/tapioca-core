#ifndef EVP_ARCHVIZ_OVERLAYANNOTATIONS_HPP
#define EVP_ARCHVIZ_OVERLAYANNOTATIONS_HPP

// ArchViz/OverlayAnnotations -- the Watch trace's annotations (evp.watch) on the
// overlays in Archicad's views: the selected frame of the retained trace, redrawn as
// the add-on's own layer (`tapioca.watch`, OverlayAnnotationContent.hpp) whenever the
// selection or the trace changes. Switched by Tapioca.OverlayAnnotations.
//
// ⚠️ ON BY DEFAULT, AND STARTED BY THE OVERLAYS. The annotations used to be drawn only
// inside the viewer palette's frame loop; the overlays moved to Archicad's own Present
// and nothing there read the trace. `EnsureStarted` is called whenever an overlay is
// turned on, so a script's annotations appear wherever an overlay is -- unless a caller
// switched them off, which is remembered until the add-on unloads.
//
// MAIN THREAD. A 250 ms timer while on; `Shutdown` kills it without ACAPI.

#include "ArchViz/OverlayAnnotationContent.hpp"

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayannotations {

struct State {
    bool enabled = false;
    bool haveFrame = false; // a trace frame is selected
    uint32_t primitives = 0;
    uint32_t drawn = 0;
    std::string frame; // "node n, frame f"
};

State Apply (bool enabled);

// How the annotations are drawn from now on; what is shown is redrawn at once.
void SetStyle (const Style& style);
State Describe ();

// An overlay was turned on: start following the trace unless a caller said off.
void EnsureStarted ();

// The project closed: the layer and what it showed go (§8).
void OnProjectClosed ();
void Shutdown ();

} // namespace overlayannotations
} // namespace archviz
} // namespace geomsrv

#endif
