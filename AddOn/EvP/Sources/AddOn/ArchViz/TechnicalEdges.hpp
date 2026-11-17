#ifndef EVP_ARCHVIZ_TECHNICALEDGES_HPP
#define EVP_ARCHVIZ_TECHNICALEDGES_HPP

// ArchViz/TechnicalEdges -- the model-wide technical / hidden-line edges (issue #14 P2).
//
// The edges that DEFINE the shape -- borders and creases sharper than an edge angle -- drawn over
// the whole model at a screen-space width, with the hidden ones optionally shown faint. It is the
// selection's edge pass (SelectionEdges.hpp, #118) applied to every element: the same face-aware
// wire pipeline, the same two depth-split passes, the same per-triangle mask.
//
// ⚠️ NEVER THE TRIANGULATION. The wireframe render mode draws every polygon boundary Archicad
// reports, which on a mesh, a terrain or a column is its tessellation. These edges are a subset of
// those (BuildCreaseEdgeMasks keeps only edges the wireframe would draw), filtered by angle.
//
// ⚠️ THE EDGE ANGLE IS A CHOICE OF PRE-BUILT TIERS, NOT A FREE SLIDER. Each tier's mask is packed
// into its own 3 bits of the triangle's wire mask when the packet is built, off the frame loop; the
// HUD's angle only picks which bits the hull shader reads (`edgeParams.y`). A free angle would mean
// re-running the weld and neighbour search for the whole model on every slider move.
//
// PURE: no Diligent, no ACAPI -- tested offline (test_technicaledges.cpp).

#include "ArchViz/SelectionEdges.hpp"
#include "ArchViz/SelectionHighlightStyle.hpp"

#include <cstddef>
#include <cstdint>

namespace geomsrv {
namespace archviz {

// The crease angles packed above the wireframe's own bits, in PACKING order. Tier 0 is the
// selection's angle, at kSelectionEdgeShift exactly as before, so the selection is unchanged.
constexpr float kCreaseTierDegrees[] = { kSelectionCreaseDegrees, 10.0f, 20.0f, 30.0f, 60.0f };
constexpr size_t kCreaseTierCount = sizeof (kCreaseTierDegrees) / sizeof (kCreaseTierDegrees[0]);
static_assert (kSelectionEdgeShift + 3 * kCreaseTierCount <= 32, "the crease tiers fit the 32-bit wire mask");

// Where tier `tier`'s mask sits: (mask >> CreaseTierShift (tier)) & 7.
constexpr uint32_t CreaseTierShift (size_t tier)
{
    return kSelectionEdgeShift + 3u * uint32_t (tier);
}

// The tier whose angle is nearest `degrees`.
size_t NearestCreaseTier (float degrees);

// The HUD's choices, ascending, for its combo; each is one of kCreaseTierDegrees.
constexpr float kTechnicalEdgeAngles[] = { 10.0f, 20.0f, 30.0f, 40.0f, 60.0f };
constexpr size_t kTechnicalEdgeAngleCount = sizeof (kTechnicalEdgeAngles) / sizeof (kTechnicalEdgeAngles[0]);

// What the frame loop hands the scene.
struct TechnicalEdgeStyle {
    bool enabled = false;
    // LINEAR rgba of the visible edges; hidden edges use the same colour at `hiddenAlpha`.
    float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float pixels = 1.25f;
    bool showHidden = false;
    float hiddenAlpha = 0.3f;
    float hiddenPixels = 1.0f;
    uint32_t maskShift = CreaseTierShift (0);
};

// The HUD stores the colour as sRGB 0xRRGGBBAA; the flat line shader writes linear.
TechnicalEdgeStyle TechnicalEdgeStyleFromHud (bool enabled, uint32_t srgbRgba, float pixels, bool showHidden,
                                              float angleDegrees);

} // namespace archviz
} // namespace geomsrv

#endif
