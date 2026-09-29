#ifndef EVP_ARCHVIZ_OVERLAYANNOTATIONCONTENT_HPP
#define EVP_ARCHVIZ_OVERLAYANNOTATIONCONTENT_HPP

// ArchViz/OverlayAnnotationContent -- the Watch trace's selected frame (evp.watch,
// Annotation/DrawList.hpp) as an overlay layer, so the annotations a script publishes
// are drawn over Archicad's own views and not only inside the viewer palette.
//
// ⚠️ THE ANNOTATION LAYER'S OWN GEOMETRY, THE OVERLAYS' PROJECTION. A dimension is
// resolved by Annotation/DimensionGeometry exactly as the viewer resolves it -- its
// plane, its side, its offset -- and an angle's arc is sampled by the same helper; what
// differs is only where the pixels come from: each overlay projects the model points at
// its own Present (OverlayScene.hpp). The viewer's screen-space fitting (text moved
// outside a short dimension, hover-only dimensions) is not repeated: an overlay has no
// per-frame CPU camera to fit against, so a dimension too short to hold its text hides
// the text instead (OverlayScene's kHideShortSpan).
//
// Pure -- tests/cpp builds it.

#include "Annotation/DrawList.hpp"
#include "ArchViz/OverlayLayers.hpp"

namespace geomsrv {
namespace archviz {
namespace overlayannotations {

constexpr const char* kLayerName = "tapioca.watch";

struct Built {
    overlaylayers::Layer layer;
    uint32_t primitives = 0; // in the frame
    uint32_t drawn = 0;      // turned into something drawn
};

// How the annotations are drawn -- Tapioca.OverlayAnnotations' `style`. Each role keeps
// the viewer's colour (annotation::RoleColour) unless `roleRgba` names another.
struct Style {
    float lineWidthPixels = 2.0f;    // polylines and arrows
    float contextWidthPixels = 1.0f; // the Context role's, drawn lighter
    float pointSizePixels = 8.0f;    // a point's marker in the plan
    float textSizePixels = 12.0f;    // labels, and a dimension's or angle's text
    uint32_t haloRgba = 0x000000B0u;
    float haloPixels = overlaylayers::kAutoHalo; // as a text's
    // Where the building is in front: labels and dimensions fade, the rest is drawn
    // over it -- the viewer's FadeWhenOccluded, which `occlusion` replaces for all.
    overlaylayers::Behind occlusion = overlaylayers::Behind::Layer;
    uint32_t roleRgba[6] = {}; // by annotation::SemanticRole; alpha 0: the role's own
    std::string font;          // a font file; empty is the bundled font
};

// Points as markers, polylines as polylines, arrows with their heads, dimensions and
// angles through the annotation geometry, labels as text -- each in its role's colour.
// Element primitives name a GUID and draw nothing here.
Built BuildLayer (const annotation::Frame& frame, const Style& style = Style ());

} // namespace overlayannotations
} // namespace archviz
} // namespace geomsrv

#endif
