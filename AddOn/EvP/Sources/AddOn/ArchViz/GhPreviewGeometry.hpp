#ifndef EVP_ARCHVIZ_GHPREVIEWGEOMETRY_HPP
#define EVP_ARCHVIZ_GHPREVIEWGEOMETRY_HPP

// ArchViz/GhPreviewGeometry — a Grasshopper preview snapshot turned into
// drawables.
//
// `GhPreviewCache` answers WHAT THE DEFINITION MEANS: identified primitives with
// world-space arrays and flags. This file answers WHAT IS DRAWN — the flat-shaded
// mesh triangles and the constant-width ribbon for the curves — and
// `GhPreviewLayer` owns nothing but the buffers and the draws. Same split as
// StorySliceGeometry/StorySliceLayer and PlanAnchorRibbon/PlanAnchorLayer, for
// the same reason: every way of getting this wrong produces a PICTURE rather
// than an error, and a picture is what an offline test can still pin if the
// geometry is pure.
//
// Pure: no Diligent, no DevKit, no ACAPI, no Win32.
//
// ⚠️ THIS FILE DOES NOT KNOW WHERE THE CAMERA IS, AND MUST NOT LEARN. Orbit,
// zoom, pan and the view matrix belong to the Diligent viewport, which already
// owns them for every other layer; preview is handed the same `viewProj` every
// frame and pushes its ribbon out in CLIP space, where a pixel has a size. A
// preview that tracked the camera itself would be a second description of one
// camera, and every disagreement between two such descriptions in this renderer
// has surfaced as a picking or projection fault.
//
// ⚠️ THE RIBBON IS SCREEN-WIDTH, NOT WORLD-WIDTH — the same design as
// PlanAnchorRibbon and StorySliceGeometry. A preview curve is read against the
// building at every zoom; a fixed world width is a fat tube up close and
// invisible from across the site. Each vertex therefore carries its segment's
// OTHER endpoint, so the vertex shader can project both, measure the direction on
// SCREEN, and push perpendicular to that.
//
// ⚠️ AND IT IS BUILT ONE PIXEL WIDER THAN IT DRAWS, carrying `side`, for the
// reason StorySliceVertex states: this target has NO MSAA, so the antialiasing
// has to be analytic. Without it a near-horizontal curve staircases, and on a
// preview that reads as bad geometry from the definition rather than as a
// missing multisample.
//
// ⚠️ TEXT IS COLLECTED, NOT DRAWN. `BillboardText` and `WorldText` come out as
// labels with a world anchor and a string, and nothing here rasterises them:
// this renderer has no text capability yet, and inventing a private one inside
// the preview layer would be the second one to throw away when a real one lands.
// A label the layer cannot draw is REPORTED (see `labelCount`) rather than
// dropped silently, because "my text panel shows nothing" and "Tapioca cannot
// draw text yet" are the same symptom and different problems.

#include "Preview/GhPreviewCache.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

// ⚠️ THIS STRUCT AND kGhPreviewMeshVS's INPUT LAYOUT ARE ONE CONTRACT (see
// GhPreviewLayer.cpp). A mismatch does not fail; it reads the wrong bytes and
// draws confidently wrong geometry.
struct GhPreviewMeshVertex {
    float x, y, z;    // world metres, Z-up, as the definition produced them
    float nx, ny, nz; // unit, world space; the flat-shading normal
    uint32_t rgba;    // resolved from the primitive's flags, per vertex
};

// ⚠️ SO ARE THIS ONE AND kGhPreviewLineVS's.
struct GhPreviewLineVertex {
    float x, y, z;    // this corner's centreline point
    float ox, oy, oz; // the segment's OTHER endpoint, for the screen direction
    // Which side of the centreline this corner is pushed to: -1 or +1. See the
    // header — this is the analytic antialiasing, not a decoration.
    float side;
    uint32_t rgba;

    // ⚠️ THERE IS NO "WHICH END" FIELD, AND THERE MUST NOT BE ONE. Because every
    // corner names the OTHER end of its own segment, the direction to the other
    // end always points inward, so the square cap is always a push against it.
    // A separate end flag would be a second statement of the same fact, and the
    // two could disagree — at which point one end of every line grows a spur.
};

// A piece of text the definition asked for, with nowhere to be drawn yet.
//
// Deliberately carries MEANING and not a quad: the anchor, the string and
// whether it faces the camera. Whoever adds text rendering builds the quads at
// the camera, exactly as TraceAnnotationLayer already does for its own markers,
// and needs nothing from this file changed.
struct GhPreviewLabel {
    float x, y, z;
    uint32_t rgba = 0xFFFFFFFFu;
    // True for BillboardText (always faces the camera), false for WorldText
    // (lies in the plane the definition put it in).
    bool billboard = true;
    std::string text;
};

// One depth behaviour's worth of drawables. Two of these exist because the x-ray
// flag is a different PIPELINE, not a different colour: a primitive marked x-ray
// must be visible INSIDE the wall it is being compared against, and that is a
// depth-test-off variant rather than anything the geometry can express.
struct GhPreviewBucket {
    std::vector<GhPreviewMeshVertex> meshVertices;
    std::vector<uint32_t> meshIndices;
    // A triangle list, six vertices per segment. Not indexed: adjacent segments
    // share no vertex once each is pushed to its own side and capped.
    std::vector<GhPreviewLineVertex> lineVertices;

    bool Empty () const
    {
        return meshIndices.empty () && lineVertices.empty ();
    }
};

struct GhPreviewDrawables {
    GhPreviewBucket depthTested;
    GhPreviewBucket xray;
    std::vector<GhPreviewLabel> labels;

    // Primitives of a kind this build does not produce geometry for yet
    // (PointMarker, PlaneGizmo, Arrow3D, PointCloud, BillboardSprite).
    // COUNTED, not dropped quietly: a definition whose markers never appear
    // should say "Tapioca does not draw those yet", not nothing at all.
    size_t deferredKinds = 0;
    // True when a ceiling below cut the build short. The viewport reports it;
    // a preview silently missing its last ten thousand triangles is a picture of
    // a building that does not exist.
    bool truncated = false;

    size_t LabelCount () const
    {
        return labels.size ();
    }
};

// What a preview primitive looks like. Colours are RGBA8 in 0xRRGGBBAA order,
// matching every other layer here.
struct GhPreviewStyle {
    uint32_t rgba = 0x4CA64CFFu;            // ordinary preview geometry
    uint32_t selectedRgba = 0x2ECC40FFu;    // selected on the Grasshopper canvas
    uint32_t highlightedRgba = 0xFFD400FFu; // highlighted (a watch, a warning)
    float lineWidthPixels = 2.0f;
};

// Ceilings, applied while building rather than after.
//
// ⚠️ CHECKED AS IT GOES, NOT AT THE END. The point of a ceiling on this path is
// that a definition previewing millions of primitives must not be able to make
// Archicad allocate them; building the lot and then trimming would have already
// paid the cost the ceiling exists to refuse.
struct GhPreviewLimits {
    size_t maxMeshVertices = 4u * 1000u * 1000u;
    size_t maxLineVertices = 2u * 1000u * 1000u;
};

// Snapshot -> drawables, for ONE drawing surface.
//
// `surface` selects which entries are read: the 3D layer asks for `Model3D` and
// the plan overlay for `FloorPlan`, and `Both` primitives answer yes to either --
// which is the whole reason `Both` is a value rather than two messages.
//
// ⚠️ ORDERED BY primitiveId, NOT BY MAP ORDER. The snapshot's primitives come out
// of an unordered map, so the order differs between runs and can differ between
// frames; with any transparency in the style that is a flicker nobody could
// trace back to a hash seed. Sorting also makes a test able to say what the
// buffer contains.
//
// Invisible primitives are skipped entirely rather than drawn transparent:
// toggling a component's preview off must cost nothing to draw, and an alpha-zero
// triangle still costs a fragment.
GhPreviewDrawables BuildGhPreviewDrawables (const evp::preview::GhPreviewSnapshot& snapshot,
                                            evp::preview::PreviewSurface surface, const GhPreviewStyle& style,
                                            const GhPreviewLimits& limits);

// The colour one primitive's flags resolve to. Selected wins over highlighted,
// which wins over ordinary: selection is the thing the user is doing right now.
uint32_t GhPreviewColour (uint8_t flags, const GhPreviewStyle& style);

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_GHPREVIEWGEOMETRY_HPP
