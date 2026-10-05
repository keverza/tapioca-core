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
    std::vector<float> outlineDashMetres;    // its pattern where it is visible, in metres; empty is solid
    overlaylayers::HiddenLine outlineHidden; // its colour, width and pattern where the building hides it
    // Where the building is in front: dashed, the drafting convention for hidden edges.
    overlaylayers::Behind outlineBehind = overlaylayers::Behind::Dash;
    uint32_t fillRgba = 0xC8C8C84Du; // light, translucent grey; alpha 0 is no fill
    // Slices coloured: `fillColors` one per floor level in turn, from the lowest -- slices
    // at one height share theirs, across slabs -- or else `fillColormap` over the slices'
    // heights (its own min and max, in metres, when it has them); `fillRgba` when neither.
    // `fillOpacity` multiplies whichever it is.
    std::vector<uint32_t> fillColors;
    overlaylayers::Colormap fillColormap; // no stops: none
    float fillOpacity = 1.0f;
    overlaylayers::Behind fillBehind = overlaylayers::Behind::Fade;
    bool label = true;
    float labelSizePixels = 11.0f;
    uint32_t labelRgba = 0x202020FFu;
    uint32_t labelHaloRgba = 0xFFFFFFD0u;
    float labelHaloPixels = overlaylayers::kAutoHalo; // as a text's
    std::string labelFont;                            // a font file; empty is the bundled font
    uint32_t decimals = 1;
    bool labelName = false; // the slice's name before the area
    // ⚠️ ON THE SLICE BY DEFAULT: the label lies on the cut plane at its height, along
    // an edge of the contour, sized to the slice -- what a slice looks like is a plate,
    // and a label facing the camera floats in front of it (the user, 2026-09-29).
    // False puts it back on the screen, `labelSizePixels` high.
    bool labelOnSlice = true;
    double labelSizeMetres = 0.0;      // 0: fitted to the slice
    float labelMinProjectedPixels = 0; // hide until its world-size em is readable at this zoom
    double liftMetres = 0.0;           // each slice drawn this far above its cut
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

// The fill colour of the slice at `z`, the `level`th of `levels` distinct heights from the
// lowest, the lowest `low` and the highest `high` (Controls says which rule applies).
uint32_t FillColour (const Controls& controls, size_t level, double z, double low, double high);

// The model's storey cuts as slices, each at its storey's level.
std::vector<Slice> FromStoreys (const storeyslices::Snapshot& snapshot);

// A point just inside a convex corner of `chain`: the lowest-left one (smallest
// x + y) whose inward bisector lands inside every contour of the slice, `inset`
// metres along it. False when no corner qualifies; x and y are untouched then.
bool CornerAnchor (const std::vector<SliceChain>& contours, const SliceChain& chain, double inset, double& x,
                   double& y);

// Where a label lying on a slice starts: just inside the lowest-left convex corner of
// `chain`, `inset` from both its edges, its baseline along the edge that keeps the slice
// on the text's upper side (seen from above), with `room` metres along that edge.
struct SlicePlacement {
    double x = 0.0, y = 0.0;
    double dx = 1.0, dy = 0.0;
    double room = 0.0;
};
bool PlaceOnSlice (const std::vector<SliceChain>& contours, const SliceChain& chain, double inset, SlicePlacement& out);

// The label's height in metres: `wanted` when given, otherwise from the slice's area;
// then no longer than `room` holds `text`.
double LabelSizeMetres (double wanted, double areaM2, double room, const std::string& text);

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
