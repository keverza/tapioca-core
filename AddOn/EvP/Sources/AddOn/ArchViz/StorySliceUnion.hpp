#ifndef EVP_ARCHVIZ_STORYSLICEUNION_HPP
#define EVP_ARCHVIZ_STORYSLICEUNION_HPP

// ArchViz/StorySliceUnion — 2D boolean UNION of one storey's slice regions.
//
// A C++ port of `private/Commands/StorySliceOverlay/sliceunion.py`, which is the
// ORIGINAL and whose `test_sliceunion.py` is the canonical Python suite. The
// tolerances, the tie-breaks and the classification rule are the same numbers
// deliberately, so the viewer and the Python overlay draw the same outline for
// the same storey. Changing one without the other is how they drift.
//
// ⚠️ THE UNION IS THE WHOLE POINT, not a tidy-up. `SliceMesh` gives one filled
// region PER ELEMENT at the cut plane, and adjacent walls overlap and butt into
// each other. Drawn raw, a storey is a mess of doubled and crossing lines that
// reads as a tessellation bug. The union keeps only the outline of the combined
// solid and drops every edge interior to it.
//
// Method — planar arrangement + boundary classification. No CGAL, no shapely:
//   1. Quantise vertices, build the edge soup of all closed rings.
//   2. Split every edge at all intersections with every other edge (proper
//      crossings, T-junctions, and collinear-overlap endpoints).
//   3. Dedup coincident sub-edges.
//   4. Classify: probe a point just off each side. "Inside the union" = inside
//      >= 1 input ring (even-odd). Keep the sub-edge iff exactly ONE side is
//      inside — that is the union boundary.
//
// ⚠️ ONE STOREY AT A TIME, AND ONLY WHEN EVERY ELEMENT HAS BEEN CUT. The union
// is over the whole storey's regions, so it cannot be computed per element as
// the extractor walks them — a per-element union is just that element's own
// outline. The caller accumulates loops through the batch and unions once at the
// end.
//
// ⚠️ PURE — no Diligent, no DevKit, no ACAPI — so `tests/cpp` compiles the real
// source. It is O(E^2) to split; the AABB rejects in the .cpp are what keep that
// affordable on a real storey rather than on a test box.

#include "Geometry/SliceEngine.hpp" // geomsrv::Polyline

#include <cstddef>
#include <vector>

namespace geomsrv {
namespace archviz {

// One edge of the union boundary, or one span of an open cross-section that
// could not bound a region and was passed through. Both are PAIRS: the consumer
// draws line segments, and an N-point polyline fed to a segment list draws every
// other edge — which looks like a dash pattern and so reads as a styling choice
// rather than as a bug.
struct UnionSegment {
    double x0 = 0.0, y0 = 0.0;
    double x1 = 0.0, y1 = 0.0;
};

// Boolean union of one storey's closed slice regions -> boundary segments.
//
// `loops` are `SliceMesh` outputs for every element cut at this storey: a closed
// ring repeats its first point (that is what `closed` means and what this tests
// for). Open polylines cannot bound a region and pass through unchanged, split
// into consecutive pairs. The z coordinate is not used in the arithmetic — a
// storey is one plane — and is the caller's to reattach.
std::vector<UnionSegment> UnionLoops (const std::vector<Polyline>& loops);

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_STORYSLICEUNION_HPP
