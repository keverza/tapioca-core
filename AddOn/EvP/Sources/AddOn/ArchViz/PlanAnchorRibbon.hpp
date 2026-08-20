#ifndef EVP_ARCHVIZ_PLANANCHORRIBBON_HPP
#define EVP_ARCHVIZ_PLANANCHORRIBBON_HPP

// ArchViz/PlanAnchorRibbon — a 2D plan outline turned into drawable triangles.
//
// PLAT-RE65's anchors: the wall outlines Archicad itself draws, laid over the
// floor plan so the analysis layer's register can be CHECKED rather than
// trusted. This file is the geometry half and it is PURE — no Diligent, no
// ACAPI, no state — so the offline test suite covers it (tests/cpp).
//
// ⚠️ THE WIDTH IS IN PIXELS, NOT METRES, AND THAT IS THE WHOLE DESIGN. An
// anchor line exists to be compared against a line Archicad drew, and Archicad's
// plan linework is a constant screen weight at every zoom. A ribbon built at a
// fixed WORLD width would be a fat band when zoomed in and invisible when zoomed
// out — useless at exactly the zoom levels where a register error is worth
// seeing. So each vertex carries the two DIRECTIONS it may be pushed in, and the
// vertex shader does the pushing in clip space, where a pixel has a size.
//
// Each segment becomes a quad with SQUARE CAPS — extended half a width along its
// own tangent at both ends. That is not cosmetic: without it a 90-degree corner
// leaves a triangular notch half a line-width across, which on an instrument
// whose whole job is "do these two lines coincide" reads as a gap in the wall.
// The overlapping caps fill it, and overlap is invisible in a flat colour.
//
// ⚠️ ARCS: THE SIGN CONVENTION IS NOT SETTLED, so it is a PARAMETER here rather
// than a constant. The repo contradicts itself — CommandUtils' polygon walk says
// a positive `arcAngle` bulges to the RIGHT of the begIndex->endIndex direction,
// while evp.elements.polygon_area adds a positive arc's segment area to a
// counterclockwise shoelace, which is a bulge to the LEFT. Both cannot be true.
// A curved wall drawn the wrong way is a plausible-looking picture, so this
// takes `arcSign` and the command exposes it: the user turns it until a curved
// wall's anchor sits on Archicad's own arc, exactly the way the sun override
// settled the azimuth convention. Straight edges are unaffected either way.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {

// ⚠️ THIS STRUCT AND kPlanAnchorVS's input layout ARE ONE CONTRACT (see
// PlanAnchorLayer.cpp). A mismatch does not fail; it reads the wrong bytes and
// draws confidently wrong lines.
struct PlanAnchorVertex {
    float x, y, z;        // world metres, Z-up, exactly as extraction emits
    float nx, ny;         // push direction ACROSS the line, unit, model space
    float tx, ty;         // push direction ALONG it (the square cap), unit
};

// One closed or open outline -> triangles. Appends; does not clear `out`, so a
// whole storey's worth of walls accumulates into one buffer and one draw call.
//
// `xy` is {x,y,x,y,…} with `pointCount` points and NO closing repeat (that is
// what GetWallPlanOutlines returns). `arcs` is one signed angle per point for
// the edge LEAVING it, or null for an all-straight outline; a curved edge is
// tessellated into chords no longer than `arcChordMetres`.
//
// Returns how many vertices were appended. A ring with fewer than 2 points
// appends nothing — a single point has no direction to be perpendicular to, and
// guessing one draws a line that is not in the model.
size_t BuildAnchorRibbon (const float* xy, size_t pointCount,
                          const float* arcs, bool closed, float z,
                          float arcSign, float arcChordMetres,
                          std::vector<PlanAnchorVertex>& out);

// A WHOLE SET of outlines -> one ribbon, which is what the viewer is handed.
//
// `outlines[i]` is {x,y,x,y,…}; `arcs[i]` is one signed angle per point, and may
// be shorter than `outlines` or individually empty for an all-straight set.
//
// ⚠️ AN ARC ARRAY SHORTER THAN ITS POINT LIST IS DROPPED, NOT PADDED. It is one
// angle per point (the edge LEAVING it), so a short one would be read past its
// end for the last edges — and a wire that disagrees with itself about a length
// is not evidence about the last edge's curvature, it is evidence that the two
// sides disagree. Straight edges are the safe reading.
size_t BuildAnchorRibbonSet (const std::vector<std::vector<float>>& outlines,
                             const std::vector<std::vector<float>>& arcs,
                             float z, float arcSign, float arcChordMetres,
                             std::vector<PlanAnchorVertex>& out);

// The tessellated point list for one edge, arcs included — exposed because it
// is the part with a convention in it, and a test that can only see triangles
// cannot say which way an arc bulged.
//
// Appends the START point and every intermediate point, but NOT the end point:
// the caller chains edges, and a shared point emitted twice makes a degenerate
// segment whose direction is undefined.
void TessellateEdge (float x0, float y0, float x1, float y1,
                     float arcAngle, float arcSign, float arcChordMetres,
                     std::vector<float>& outXY);

}   // namespace archviz
}   // namespace geomsrv

#endif
