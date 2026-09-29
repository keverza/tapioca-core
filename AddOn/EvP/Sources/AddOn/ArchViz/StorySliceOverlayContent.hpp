#ifndef EVP_ARCHVIZ_STORYSLICEOVERLAYCONTENT_HPP
#define EVP_ARCHVIZ_STORYSLICEOVERLAYCONTENT_HPP

// ArchViz/StorySliceOverlayContent -- slices as an overlay layer: for each slice, its
// contours at its height, a light translucent fill, the outline's hidden part dashed,
// and one small label with the slice's area inside a corner of its contour.
//
// Two sources make slices, and this file draws either: the floors of the massing slabs
// (SlabSlices.hpp), and the storey cuts of the whole model from the extraction
// (StorySliceSnapshot.hpp). It only decides what is drawn of them, and is pure --
// tests/cpp builds it -- because every mistake here is a picture: a label outside its
// slice, a fill at the wrong height, a storey the filter should have dropped.
//
// The layer is the add-on's own (`tapioca.storeySlices`); Tapioca.OverlayStorySlices
// switches it (ArchViz/StorySliceOverlay.hpp).

#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/StorySliceSnapshot.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace storysliceoverlay {

constexpr const char* kLayerName = "tapioca.storeySlices";

struct Controls {
    std::vector<int> storeys; // Archicad storey numbers; empty is every storey
    overlaylayers::Views views = overlaylayers::Views::ThreeD;
    uint32_t outlineRgba = 0x3C3C3CFFu;
    float outlineWidthPixels = 2.0f;
    // Where the building is in front: dashed, the drafting convention for hidden edges.
    overlaylayers::Behind outlineBehind = overlaylayers::Behind::Dash;
    uint32_t fillRgba = 0xC8C8C84Du; // light, translucent grey; alpha 0 is no fill
    overlaylayers::Behind fillBehind = overlaylayers::Behind::Fade;
    bool label = true;
    float labelSizePixels = 11.0f;
    uint32_t labelRgba = 0x202020FFu;
    uint32_t labelHaloRgba = 0xFFFFFFD0u;
    uint32_t decimals = 1;
    bool labelName = false;  // the slice's name before the area
    double liftMetres = 0.0; // each slice drawn this far above its cut
};

// One slice to draw: a cut's contours at its height, its area and the name its label
// may carry.
struct Slice {
    std::vector<SliceChain> chains; // x y metres
    double z = 0.0;                 // the cut, world metres
    double areaM2 = 0.0;
    std::string name; // "Ground", "A-01 F3"
    int storey = 0;   // the Archicad storey the cut lies in: what the filter keys on
};

// The model's storey cuts as slices, each at its storey's level.
std::vector<Slice> FromStoreys (const storeyslices::Snapshot& snapshot);

// A point just inside a convex corner of `chain`: the lowest-left one (smallest
// x + y) whose inward bisector lands inside every contour of the slice, `inset`
// metres along it. False when no corner qualifies; x and y are untouched then.
bool CornerAnchor (const std::vector<SliceChain>& contours, const SliceChain& chain, double inset, double& x,
                   double& y);

// "245.7 m²", or "Level 2  245.7 m²".
std::string AreaText (double areaM2, uint32_t decimals, const std::string& name, bool withName);

struct Built {
    overlaylayers::Layer layer;
    uint32_t slices = 0; // slices drawn
    double areaM2 = 0.0; // their areas summed
};

// The layer for these slices under these controls; empty of primitives when no slice
// passes the filter.
Built BuildLayer (const std::vector<Slice>& slices, const Controls& controls);

} // namespace storysliceoverlay
} // namespace archviz
} // namespace geomsrv

#endif
