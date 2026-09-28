// ArchViz/OverlayLayers: arbitrary geometry on the overlays -- the store, and what the
// 2D and 3D overlays are handed to draw.

#include "ArchViz/OverlayLayers.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace ol = geomsrv::archviz::overlaylayers;

namespace {

ol::Layer Square (const char* name, double east = 0.0, double north = 0.0)
{
    ol::Layer layer;
    layer.name = name;
    ol::Polyline outline;
    outline.points = { east, north, 0.0, east + 1.0, north, 0.0, east + 1.0, north + 1.0, 0.0, east, north + 1.0, 0.0 };
    outline.closed = true;
    outline.rgba = 0x11223344u;
    outline.widthPixels = 3.0f;
    layer.polylines.push_back (outline);
    return layer;
}

double X0 (const ol::Prepared2D& p, const ol::StrokeInstance& s)
{
    return p.originX + double (s.segment.x0) + double (s.segment.x0Lo);
}
double Y0 (const ol::Prepared2D& p, const ol::StrokeInstance& s)
{
    return p.originY + double (s.segment.y0) + double (s.segment.y0Lo);
}
double X1 (const ol::Prepared2D& p, const ol::StrokeInstance& s)
{
    return p.originX + double (s.segment.x1) + double (s.segment.x1Lo);
}

struct Fresh {
    Fresh ()
    {
        ol::ClearAll ();
    }
    ~Fresh ()
    {
        ol::ClearAll ();
    }
};

} // namespace

TEST (OverlayLayers, ASetLayerReplacesItsNamesakeAndKeepsItsPlaceInTheDrawOrder)
{
    Fresh fresh;
    const uint64_t first = ol::Set (Square ("a"));
    ol::Set (Square ("b"));
    const uint64_t third = ol::Set (Square ("a", 5.0));
    EXPECT_GT (third, first);
    const auto layers = ol::Layers ();
    ASSERT_EQ (layers.size (), 2u);
    EXPECT_EQ (layers[0]->name, "a");
    EXPECT_DOUBLE_EQ (layers[0]->polylines[0].points[0], 5.0);
    EXPECT_EQ (layers[1]->name, "b");
}

TEST (OverlayLayers, ClearingMovesTheGenerationOnlyWhenSomethingWent)
{
    Fresh fresh;
    ol::Set (Square ("a"));
    const uint64_t before = ol::Generation ();
    EXPECT_FALSE (ol::Clear ("missing"));
    EXPECT_EQ (ol::Generation (), before);
    EXPECT_TRUE (ol::Clear ("a"));
    EXPECT_GT (ol::Generation (), before);
    EXPECT_TRUE (ol::Layers ().empty ());
}

TEST (OverlayLayers, ValidationNamesTheFirstProblemItFinds)
{
    ol::Layer layer = Square ("ok");
    EXPECT_TRUE (ol::Validate (layer).empty ());
    layer.name.clear ();
    EXPECT_NE (ol::Validate (layer).find ("name"), std::string::npos);

    ol::Layer uneven = Square ("x");
    uneven.polylines[0].points.pop_back ();
    EXPECT_NE (ol::Validate (uneven).find ("polyline 0"), std::string::npos);

    ol::Layer mesh;
    mesh.name = "m";
    ol::Mesh triangle;
    triangle.points = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    triangle.indices = { 0, 1, 3 };
    mesh.meshes.push_back (triangle);
    EXPECT_NE (ol::Validate (mesh).find ("index 3"), std::string::npos);

    ol::Layer nan = Square ("n");
    nan.polylines[0].points[4] = std::nan ("");
    EXPECT_FALSE (ol::Validate (nan).empty ());
}

TEST (OverlayLayers, ColoursAreWrittenRrggbbaaAndReadRedFirst)
{
    EXPECT_EQ (ol::ToUnorm (0x11223344u), 0x44332211u);
    EXPECT_EQ (ol::ToUnorm (0xFF000080u), 0x800000FFu);
}

TEST (OverlayLayers, TheTwoDStrokesCloseTheRingAndCarryTheirStyle)
{
    const auto layer = std::make_shared<const ol::Layer> (Square ("s"));
    const ol::Prepared2D prepared = ol::Prepare2D ({ layer });
    ASSERT_EQ (prepared.strokes.size (), 4u);
    EXPECT_DOUBLE_EQ (prepared.originX, 0.5);
    EXPECT_DOUBLE_EQ (prepared.originY, 0.5);
    const ol::StrokeInstance& closing = prepared.strokes.back ();
    EXPECT_NEAR (X1 (prepared, closing), 0.0, 1e-12);
    EXPECT_EQ (closing.rgba, 0x44332211u);
    EXPECT_FLOAT_EQ (closing.widthPixels, 3.0f);
}

// ⚠️ THE 2D LAYERS KEEP A GEOREFERENCED PROJECT'S MICROMETRES, as the wall outlines do.
TEST (OverlayLayers, FarFromTheOriginTheTwoDHalvesReconstructDoubles)
{
    const double east = 512345.678, north = 6543210.123;
    const auto layer = std::make_shared<const ol::Layer> (Square ("far", east, north));
    const ol::Prepared2D prepared = ol::Prepare2D ({ layer });
    ASSERT_EQ (prepared.strokes.size (), 4u);
    EXPECT_NEAR (X0 (prepared, prepared.strokes[0]), east, 1e-9);
    EXPECT_NEAR (Y0 (prepared, prepared.strokes[0]), north, 1e-9);
    EXPECT_NEAR (X1 (prepared, prepared.strokes[0]), east + 1.0, 1e-9);
}

TEST (OverlayLayers, APointIsAStrokeOfNoLengthTheMarkersWidth)
{
    ol::Layer layer;
    layer.name = "p";
    ol::PointSet set;
    set.points = { 2.0, 3.0, 0.0, 4.0, 5.0, 0.0 };
    set.sizePixels = 9.0f;
    layer.points.push_back (set);
    const ol::Prepared2D prepared = ol::Prepare2D ({ std::make_shared<const ol::Layer> (layer) });
    ASSERT_EQ (prepared.strokes.size (), 2u);
    for (const ol::StrokeInstance& stroke : prepared.strokes) {
        EXPECT_EQ (stroke.segment.x0, stroke.segment.x1);
        EXPECT_EQ (stroke.segment.y0, stroke.segment.y1);
        EXPECT_FLOAT_EQ (stroke.widthPixels, 9.0f);
    }
}

TEST (OverlayLayers, FillsComeBeforeStrokesAndAThreeDOnlyLayerIsNotInTwoD)
{
    ol::Layer both = Square ("both");
    ol::Mesh triangle;
    triangle.points = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    triangle.indices = { 0, 1, 2 };
    triangle.vertexRgba = { 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu };
    both.meshes.push_back (triangle);
    ol::Layer only3d = Square ("3d", 100.0, 100.0);
    only3d.views = ol::Views::ThreeD;
    const ol::Prepared2D prepared =
        ol::Prepare2D ({ std::make_shared<const ol::Layer> (both), std::make_shared<const ol::Layer> (only3d) });
    ASSERT_EQ (prepared.fills.size (), 3u);
    EXPECT_EQ (prepared.fills[1].rgba, ol::ToUnorm (0x00FF00FFu));
    EXPECT_EQ (prepared.strokes.size (), 4u); // the 3D-only square is not here
    EXPECT_LT (prepared.originX, 10.0);       // nor does it pull the centre
}

TEST (OverlayLayers, TheThreeDLinesArePairsAndPointsAreAxisCrosses)
{
    ol::Layer layer = Square ("s");
    layer.polylines[0].closed = false;
    ol::PointSet set;
    set.points = { 1.0, 2.0, 3.0 };
    set.sizeMetres = 0.5f;
    layer.points.push_back (set);
    const ol::Prepared3D prepared = ol::Prepare3D ({ std::make_shared<const ol::Layer> (layer) });
    // Three open segments, then the cross's three axes: two vertices each.
    ASSERT_EQ (prepared.occludedLines.size (), 3u * 2u + 3u * 2u);
    EXPECT_TRUE (prepared.overLines.empty ());
    const ol::ColourVertex& crossStart = prepared.occludedLines[6];
    EXPECT_FLOAT_EQ (crossStart.x, 0.75f);
    EXPECT_FLOAT_EQ (crossStart.y, 2.0f);
    EXPECT_FLOAT_EQ (crossStart.z, 3.0f);
}

TEST (OverlayLayers, AnUnoccludedLayerGoesToTheOverPasses)
{
    ol::Layer layer = Square ("over");
    layer.occluded = false;
    ol::Mesh triangle;
    triangle.points = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    triangle.indices = { 0, 1, 2 };
    layer.meshes.push_back (triangle);
    const ol::Prepared3D prepared = ol::Prepare3D ({ std::make_shared<const ol::Layer> (layer) });
    EXPECT_TRUE (prepared.occludedLines.empty ());
    EXPECT_TRUE (prepared.occludedFills.empty ());
    EXPECT_EQ (prepared.overLines.size (), 8u);
    EXPECT_EQ (prepared.overFills.size (), 3u);
}
