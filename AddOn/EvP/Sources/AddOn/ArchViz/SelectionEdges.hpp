#ifndef EVP_ARCHVIZ_SELECTIONEDGES_HPP
#define EVP_ARCHVIZ_SELECTIONEDGES_HPP

// ArchViz/SelectionEdges -- which of an element's edges the SELECTION draws (issue #118).
//
// ⚠️ NOT THE WIREFRAME'S EDGES. The wireframe draws every edge Archicad calls visible, which is
// every polygon boundary -- right for a wall, wrong for a mesh, a terrain, a railing or a column,
// whose surface Archicad builds from many small polygons. Selected, a 64-sided column was 64 lines
// and a terrain was its whole triangulation (the user, live). The selection draws only the edges
// that DEFINE the shape:
//
//   a CREASE  -- the faces either side meet at more than `creaseDegrees`; coplanar and gently
//                curved neighbours merge, so a cylinder keeps its cap circles and loses its
//                facets, and a terrain keeps its ridges and loses its triangles;
//   a BORDER  -- an edge with one face (an open panel's outline), or more than two;
//
// and, of those, only edges the wireframe would draw at all, so a flat face's triangulation can
// never come back through this door. A smooth surface's silhouette is the outline hull's job.
//
// Computed on the CPU when the packet is built, off the frame loop, and packed into bits 3..5 of
// the same per-triangle mask the wireframe reads from bits 0..2 -- so it costs no buffer, no
// upload and no binding of its own. Pure, so it is tested offline (test_selectionedges.cpp).

#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {

// Where the selection's bits sit in a packed wire mask: (mask >> kSelectionEdgeShift) & 7.
constexpr uint32_t kSelectionEdgeShift = 3;

// One 3-bit mask per triangle, in BuildTriangleWireEdgeMask's edge order: bit 0 = (v1,v2),
// bit 1 = (v2,v0), bit 2 = (v0,v1).
//
// `positions` is xyz per vertex; `indices` three per triangle; `wireMasks` one per triangle (only
// bits 0..2 are read; an empty vector means every edge is drawable).
//
// ⚠️ NEIGHBOURS ARE FOUND BY POSITION, NOT BY INDEX. The upload welds corners that share a
// position AND a normal, so across a hard edge -- exactly the edges this is looking for -- the
// two faces hold different vertex indices for the same corner. Matching by index would see every
// hard edge as a border, and draw it for the wrong reason.
std::vector<uint8_t> BuildSelectionEdgeMasks (const std::vector<float>& positions, const std::vector<uint32_t>& indices,
                                              const std::vector<uint32_t>& wireMasks, float creaseDegrees);

} // namespace archviz
} // namespace geomsrv

#endif
