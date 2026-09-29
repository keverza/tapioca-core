// ArchViz/StorySliceOverlayContent: the storey slices as an overlay layer. A label
// outside its slice, a fill at the wrong height or a storey the filter should have
// dropped is a picture, not an error, so each rule is pinned here.

#include "ArchViz/StorySliceOverlayContent.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace so = geomsrv::archviz::storysliceoverlay;
namespace ss = geomsrv::archviz::storeyslices;
namespace layers = geomsrv::archviz::overlaylayers;
using geomsrv::archviz::SliceChain;

namespace {

SliceChain Square (double x0, double y0, double x1, double y1, bool clockwise = false)
{
    SliceChain chain;
    chain.closed = true;
    chain.xy = clockwise ? std::vector<double> { x0, y0, x0, y1, x1, y1, x1, y0 }
                         : std::vector<double> { x0, y0, x1, y0, x1, y1, x0, y1 };
    return chain;
}

ss::Snapshot TwoStoreys ()
{
    ss::Snapshot snapshot;
    ss::Storey ground;
    ground.index = 0;
    ground.level = 0.0;
    ground.name = "Ground";
    ground.chains = { Square (0, 0, 10, 10) };
    ground.areaM2 = 100.0;
    ss::Storey first = ground;
    first.index = 1;
    first.level = 3.0;
    first.name = "First";
    first.chains = { Square (0, 0, 10, 5) };
    first.areaM2 = 50.0;
    snapshot.storeys = { ground, first };
    snapshot.generation = 7;
    return snapshot;
}

} // namespace

TEST (StorySliceOverlay, TheLabelSitsInsideTheLowerLeftCorner)
{
    const SliceChain square = Square (0, 0, 10, 10);
    double x = -1.0, y = -1.0;
    ASSERT_TRUE (so::CornerAnchor ({ square }, square, 1.0, x, y));
    EXPECT_NEAR (x, 0.70710678, 1e-6);
    EXPECT_NEAR (y, 0.70710678, 1e-6);
    // Wound the other way, the same corner.
    const SliceChain clockwise = Square (0, 0, 10, 10, true);
    ASSERT_TRUE (so::CornerAnchor ({ clockwise }, clockwise, 1.0, x, y));
    EXPECT_NEAR (x, 0.70710678, 1e-6);
    EXPECT_NEAR (y, 0.70710678, 1e-6);
}

// ⚠️ A HOLE IS OUTSIDE. A light well in the lower-left corner pushes the label to the
// next corner rather than into the well.
TEST (StorySliceOverlay, TheLabelAvoidsAHoleAtTheCorner)
{
    const SliceChain outer = Square (0, 0, 10, 10);
    const SliceChain hole = Square (0.2, 0.2, 2, 2, true);
    double x = 0.0, y = 0.0;
    ASSERT_TRUE (so::CornerAnchor ({ outer, hole }, outer, 1.0, x, y));
    EXPECT_GT (x + y, 5.0);
}

// The lowest-left vertex of a polygon is on its hull, so it is always convex; an L's
// label goes to its outer corner, never into the notch at (5, 5).
TEST (StorySliceOverlay, AnLsLabelSitsInItsOuterCorner)
{
    SliceChain l;
    l.closed = true;
    l.xy = { 0, 0, 10, 0, 10, 5, 5, 5, 5, 10, 0, 10 };
    double x = 0.0, y = 0.0;
    ASSERT_TRUE (so::CornerAnchor ({ l }, l, 0.5, x, y));
    EXPECT_NEAR (x, 0.35355339, 1e-6);
    EXPECT_NEAR (y, 0.35355339, 1e-6);
}

TEST (StorySliceOverlay, EachStoreyIsItsFillOutlineAndAreaAtItsLevel)
{
    const so::Controls controls;
    const so::Built built = so::BuildLayer (so::FromStoreys (TwoStoreys ()), controls);
    const layers::Layer& layer = built.layer;
    EXPECT_EQ (layer.name, so::kLayerName);
    EXPECT_TRUE (layers::Reserved (layer.name));
    EXPECT_EQ (layers::Validate (layer), "");
    EXPECT_EQ (built.slices, 2u);
    EXPECT_DOUBLE_EQ (built.areaM2, 150.0);
    ASSERT_EQ (layer.meshes.size (), 2u);
    ASSERT_EQ (layer.polylines.size (), 2u);
    ASSERT_EQ (layer.texts.size (), 2u);
    // The fill: light translucent grey, faint behind the building, drawn by the guest.
    EXPECT_EQ (layer.meshes[0].rgba, 0xC8C8C84Du);
    EXPECT_TRUE (layers::DrawnByGuest (layer.meshes[0]));
    EXPECT_EQ (layer.meshes[0].style.behind, layers::Behind::Fade);
    for (size_t i = 2; i < layer.meshes[1].points.size (); i += 3)
        EXPECT_DOUBLE_EQ (layer.meshes[1].points[i], 3.0);
    // The outline: its hidden part dashed.
    EXPECT_EQ (layer.polylines[1].behind, layers::Behind::Dash);
    EXPECT_TRUE (layer.polylines[1].closed);
    EXPECT_DOUBLE_EQ (layer.polylines[1].points[2], 3.0);
    // One small label per slice, at its storey's height, never hidden.
    EXPECT_EQ (layer.texts[0].text, "100.0 m\xC2\xB2");
    EXPECT_EQ (layer.texts[1].text, "50.0 m\xC2\xB2");
    EXPECT_DOUBLE_EQ (layer.texts[1].at[2], 3.0);
    EXPECT_EQ (layer.texts[1].behind, layers::Behind::Show);
    // Lying on the slice, inside it, sized to it.
    EXPECT_TRUE (layer.texts[1].planar);
    EXPECT_DOUBLE_EQ (layer.texts[1].normal[2], 1.0);
    EXPECT_GT (layer.texts[1].sizeMetres, 0.1);
    EXPECT_LT (layer.texts[1].sizeMetres, 1.0);
    EXPECT_GT (layer.texts[1].at[0], 0.0);
    EXPECT_GT (layer.texts[1].at[1], 0.0);
    EXPECT_LT (layer.texts[1].at[1], 5.0);

    // Or facing the view, as before.
    so::Controls screen;
    screen.labelOnSlice = false;
    const so::Built facing = so::BuildLayer (so::FromStoreys (TwoStoreys ()), screen);
    ASSERT_EQ (facing.layer.texts.size (), 2u);
    EXPECT_FALSE (facing.layer.texts[1].planar);
    EXPECT_FLOAT_EQ (facing.layer.texts[1].sizePixels, 11.0f);
}

// ⚠️ THE TEXT RISES INTO THE SLICE, whichever way its contour winds: along the edge
// whose left side is the slice, from just inside the lowest-left corner.
TEST (StorySliceOverlay, ALabelOnASliceRunsAlongAnEdgeIntoIt)
{
    for (const bool clockwise : { false, true }) {
        const SliceChain square = Square (0, 0, 10, 10, clockwise);
        so::SlicePlacement place;
        ASSERT_TRUE (so::PlaceOnSlice ({ square }, square, 0.5, place)) << clockwise;
        EXPECT_NEAR (place.x, 0.5, 1e-9);
        EXPECT_NEAR (place.y, 0.5, 1e-9);
        EXPECT_NEAR (place.dx, 1.0, 1e-9); // along +x, rising to +y: read from above
        EXPECT_NEAR (place.dy, 0.0, 1e-9);
        EXPECT_NEAR (place.room, 9.0, 1e-9);
    }
}

TEST (StorySliceOverlay, ALabelIsSizedToItsSliceAndItsEdge)
{
    EXPECT_DOUBLE_EQ (so::LabelSizeMetres (0.8, 400.0, 1.0, "anything at all"), 0.8); // asked for
    EXPECT_NEAR (so::LabelSizeMetres (0.0, 400.0, 100.0, "412.5 m\xC2\xB2"), 0.7, 1e-9);
    // Too long for its edge: smaller, until it fits.
    const double fitted = so::LabelSizeMetres (0.0, 400.0, 2.0, "A-01 F3  412.5 m\xC2\xB2");
    EXPECT_LT (fitted, 0.7);
    EXPECT_LE (16.0 * 0.55 * fitted, 0.9 * 2.0 + 1e-9);
}

TEST (StorySliceOverlay, TheControlsFilterLiftAndSilence)
{
    so::Controls controls;
    controls.storeys = { 1 };
    controls.liftMetres = 0.25;
    controls.fillRgba = 0xFFFFFF00u; // alpha 0: no fill
    controls.label = false;
    controls.labelName = true;
    const so::Built built = so::BuildLayer (so::FromStoreys (TwoStoreys ()), controls);
    EXPECT_EQ (built.slices, 1u);
    EXPECT_TRUE (built.layer.meshes.empty ());
    EXPECT_TRUE (built.layer.texts.empty ());
    ASSERT_EQ (built.layer.polylines.size (), 1u);
    EXPECT_DOUBLE_EQ (built.layer.polylines[0].points[2], 3.25);

    controls.label = true;
    const so::Built named = so::BuildLayer (so::FromStoreys (TwoStoreys ()), controls);
    ASSERT_EQ (named.layer.texts.size (), 1u);
    EXPECT_EQ (named.layer.texts[0].text, "First  50.0 m\xC2\xB2");
}

TEST (StorySliceOverlay, AreasReadWithTheirDecimals)
{
    EXPECT_EQ (so::AreaText (245.66, 1, "", false), "245.7 m\xC2\xB2");
    EXPECT_EQ (so::AreaText (245.66, 0, "L2", true), "L2  246 m\xC2\xB2");
}
