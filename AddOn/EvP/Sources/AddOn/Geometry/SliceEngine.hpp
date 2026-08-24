#ifndef GEOMETRYSERVER_SLICEENGINE_HPP
#define GEOMETRYSERVER_SLICEENGINE_HPP

#include "Mesh.hpp"

#include <vector>
#include <string>
#include <cstdint>

// Horizontal plane cut (z = const) of a snapshot -> per-element polylines.
// Pure C++ over the immutable snapshot; runs on HTTP worker threads.
//
// Robustness notes (these matter in practice):
//  * Triangles are classified by the SIGNED distance of each vertex to the plane,
//    with |d| < eps snapped to 0 and zeros counted as "above". That tie-break
//    makes slicing EXACTLY at a slab/roof face well-defined: fully-coplanar
//    triangles are skipped, and the element's side faces still emit the face
//    boundary. Without it, cutting at z = floor level is degenerate.
//  * Segments are welded into chains on a quantised grid (`weld` tolerance).
//    Archicad tessellation has T-junctions and duplicated coincident faces, so
//    some chains legitimately fail to close: those are returned with
//    closed=false rather than dropped, because a silently dropped chain is a
//    hole in the caller's outline that they will not notice.
//  * A wall with an opening at this z yields an outer loop AND an inner loop.
//    Both are returned; classifying outer-vs-hole is left to the caller
//    (even-odd fill handles it without needing orientation).
namespace geomsrv {

struct Polyline {
    std::vector<double> pts;      // xyz interleaved (z is constant = the cut plane)
    bool                closed = false;

    size_t PointCount () const { return pts.size () / 3; }
};

struct ElementSlice {
    std::string           guid;
    int32_t               elemType = 0;
    std::vector<Polyline> loops;
};

struct SliceResult {
    double                    zUsed = 0.0;    // the plane actually cut (see `nudged`)
    bool                      nudged = false; // z was tangent to geometry; lifted slightly
    std::vector<ElementSlice> elements;
};

// Cut ONE element's triangle soup at z -> its polylines, welded and chained.
//
// ⚠️ THIS IS THE WHOLE ALGORITHM; `SliceZ` below is a loop over it plus the
// snapshot's filtering and tangency lift. It is exposed because the ArchViz
// viewer needs the same cut and must NOT pay for a second model read to get it:
// the extraction thread already walks every element's world-space doubles on its
// way to the GPU, so it slices there and shares this core rather than acquiring
// a `Snapshot` the viewer otherwise never builds. Two copies of a plane cut is
// exactly the drift the offline suite could not catch — one would be tested and
// the other would be the one drawing.
//
// ⚠️ DOUBLES, AND THAT IS NOT INCIDENTAL. A survey-placed model sits at
// coordinates in the millions, where float32 has ~10 cm of resolution and the
// 1e-6 m weld tolerance below becomes meaningless — every endpoint welds to
// every other, or none do. Callers holding float geometry must slice BEFORE
// they narrow it.
//
// `vertices` is xyz interleaved; `triangles` is 3 vertex indices per triangle;
// `weld` is the endpoint-merge tolerance in metres. No tangency lift is applied
// here — the caller decides, because tangency is a property of the whole set of
// elements being cut, not of one of them.
std::vector<Polyline> SliceMesh (const double* vertices, size_t vertexCount,
                                 const uint32_t* triangles, size_t triangleCount,
                                 double z, double weld = 1e-6);

// Does the plane land exactly on a vertex of this element (within `eps`)? A
// tangent element has no cross-section, so a caller cutting at storey level —
// where every wall starts — lifts the plane a micron. Split out of `SliceZ` so
// the ArchViz path can make the same decision over its own element set.
bool IsTangentToPlane (const double* vertices, size_t vertexCount, double z);

// Cut the snapshot at z. If `types` is non-empty, only those ModelerAPI element
// types are cut; if `guids` is non-empty, only those elements. `weld` is the
// endpoint-merge tolerance in meters.
//
// TANGENCY: a plane exactly at an element's base is tangent — the solid lies
// entirely "above" it and would yield NO cross-section. That is exactly what
// happens when you slice at floor level (walls start there), so by default we
// detect tangency and lift the plane by 1 micron. The effect is the intuitive
// one: slicing at floor level excludes the floor slab (you are above it) and
// includes the walls rising from it. `zUsed`/`nudged` report what happened; pass
// nudge=false to cut exactly at z.
SliceResult SliceZ (const Snapshot& snap, double z,
                    const std::vector<int32_t>& types,
                    const std::vector<std::string>& guids,
                    double weld = 1e-6,
                    bool nudge = true);

} // namespace geomsrv

#endif
