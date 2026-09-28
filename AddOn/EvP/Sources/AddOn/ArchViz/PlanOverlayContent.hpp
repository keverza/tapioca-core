#ifndef EVP_ARCHVIZ_PLANOVERLAYCONTENT_HPP
#define EVP_ARCHVIZ_PLANOVERLAYCONTENT_HPP

// ArchViz/PlanOverlayContent -- the floor-plan overlay's geometry: Archicad's own 2D
// outlines, as line segments the Present-side layer draws through the transform read
// at the plan's Present.
//
// ⚠️ 2D DRAWING, NEVER A CUT OF THE 3D MESH (OVERLAY-INVARIANTS.md §12). The rings are
// what the plan itself draws -- walls' connection polygons from
// `ACAPI_Element_GetRelations`, trimmed at junctions -- handed in by the content
// reader the add-on registers.
//
// ⚠️ DOUBLE UNTIL THE LAST STEP, AND THEN TWO FLOATS, NOT ONE. A georeferenced project
// sits hundreds of kilometres from its origin, where a float keeps a few centimetres;
// even relative to the content's own centre a float keeps only micrometres at the
// far end of a large site, which extreme close zoom (§14) magnifies into pixels. Every
// coordinate is therefore relative to the content's centre and split into the float
// nearest it and the float remainder the first one dropped; the layer subtracts the
// view's anchor from each half separately, so a point near the anchor keeps double's
// precision on the GPU without the GPU doing double arithmetic.
//
// Pure arithmetic: no ACAPI, no D3D. Tested in tests/cpp/test_planoverlaycontent.cpp.

#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace plancontent {

// One segment, in model metres relative to Content's origin: the high halves, then the
// low halves. This IS the layer's per-instance layout -- two float4s -- so the order
// is a contract with the shader, not a style.
struct Segment {
    float x0, y0, x1, y1;         // the float nearest each coordinate
    float x0Lo, y0Lo, x1Lo, y1Lo; // what that float dropped
};
static_assert (sizeof (Segment) == 32, "the plan layer reads a segment as two float4s");

struct Content {
    double originX = 0.0; // model metres: the centre of every ring point
    double originY = 0.0;
    std::vector<Segment> segments;
    uint32_t rings = 0;
};

// `value` as the float nearest it plus the float remainder: hi + lo carries ~48 bits.
void Split (double value, float& hi, float& lo);

// Closed rings of model-metre points (x, y, x, y, ...), with one signed arc angle per
// vertex for the edge that starts there (0 = straight), as the plan's wall outlines
// carry them. `arcSign` is SetPlanAnchors' knob for the repo's two readings of an
// arc's direction (+1 is the DevKit's: a positive angle puts the arc on the chord's
// right, so it turns counter-clockwise); `arcChordMetres` bounds each chord of a
// tessellated arc.
Content BuildContent (const std::vector<std::vector<double>>& rings, const std::vector<std::vector<double>>& arcs,
                      double arcSign, double arcChordMetres);

// One edge's point list in double: its start and its arc's interior, never its end.
// ⚠️ THE SAME READING AS `TessellateEdge` (PlanAnchorRibbon.hpp), in double: the same
// centre, the same direction and the same step count. The test pins the two together,
// so the plan overlay and the anchor ribbon cannot come to disagree about which way
// an arc bulges.
void TessellateEdgeDouble (double x0, double y0, double x1, double y1, double arcAngle, double arcSign,
                           double arcChordMetres, std::vector<double>& outXY);

// ---- the frame's projection ----------------------------------------------------

// Model metres -> the back buffer's PHYSICAL pixels, as read at the plan's Present:
//     px = xx * mx + xy * my + ox,   py = yx * mx + yy * my + oy
struct PixelTransform {
    double xx = 0.0, xy = 0.0, yx = 0.0, yy = 0.0, ox = 0.0, oy = 0.0;
};

// The constants the layer's shader reads, in its cbuffer's order. The projection is
// ANCHORED at the model point under the buffer's centre: the shader measures each
// point from the anchor, high half from high half and low half from low half, so what
// is on screen is a small number in float however far it sits from the origin.
struct ViewConstants {
    float linear[4] = {}; // physical pixels per metre: xx, xy, yx, yy
    float view[4] = {};   // the anchor relative to the content origin: hi x, hi y, lo x, lo y
    float screen[4] = {}; // the anchor's physical pixel x, y; then 2 / width, 2 / height
    float colour[4] = {}; // straight alpha
    float stroke[4] = {}; // half the stroke's width in physical pixels, then unused
};
static_assert (sizeof (ViewConstants) == 80, "the plan layer's cbuffer is five float4s");

// Fill `linear`, `view` and `screen` for one frame. False when the transform does not
// invert (there is no anchor) or the buffer has no size; `out` is then untouched.
bool MakeViewConstants (const PixelTransform& transform, double originX, double originY, uint32_t width,
                        uint32_t height, ViewConstants& out);

// What the shader computes for one point, in float, step for step -- so a test can
// hold the GPU's arithmetic to the double projection without a GPU.
void ShaderPixel (const ViewConstants& constants, float hiX, float hiY, float loX, float loY, float& px, float& py);

} // namespace plancontent
} // namespace archviz
} // namespace geomsrv

#endif
