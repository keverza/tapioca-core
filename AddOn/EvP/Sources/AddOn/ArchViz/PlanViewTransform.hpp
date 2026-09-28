#ifndef EVP_ARCHVIZ_PLANVIEWTRANSFORM_HPP
#define EVP_ARCHVIZ_PLANVIEWTRANSFORM_HPP

// ArchViz/PlanViewTransform -- the floor plan's model-to-pixel map, read from ACAPI.
//
// ⚠️ READ AT THE PLAN'S PRESENT, AND THAT IS THE WHOLE DESIGN (OVERLAY-INVARIANTS.md
// finding 14). Archicad applies a pan or a zoom, draws and presents in one pass of its
// paint path on the main thread; every read taken before that Present is a frame
// stale, and the read taken inside it is the frame's transform (measured: p95 0.50 px
// against the frame's pixels). §11 permits this ACAPI call in the plan's Present
// detour alone: the plan canvas's chain, ACAPI's own thread, outside any ACAPI call of
// ours -- which the CALLER checks, on every call.
//
// Three corners of the canvas, in LOGICAL pixels, fix the affine map whatever the
// zoom, rotation or display scaling; a repeat of the first says whether the view
// moved while it was being asked. The fit is PlanTransformMath's, the one the earlier
// plan overlay measured with -- one definition of the measurement.
//
// MAIN THREAD ONLY: ACAPI.

#include <cstdint>

namespace geomsrv {
namespace archviz {

// model metres -> the canvas's LOGICAL pixels:
//     x = xx * mx + xy * my + ox,   y = yx * mx + yy * my + oy
struct PlanViewTransform {
    bool valid = false;
    bool torn = false; // the view moved between the samples
    int32_t error = 0; // ACAPI's error when a sample was refused
    double xx = 0.0, xy = 0.0, yx = 0.0, yy = 0.0, ox = 0.0, oy = 0.0;
};

// Sample the canvas at (0,0), (logicalWidth,0), (0,logicalHeight) and (0,0) again.
PlanViewTransform ReadPlanViewTransform (uint32_t logicalWidth, uint32_t logicalHeight);

} // namespace archviz
} // namespace geomsrv

#endif
