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

TEST (OverlayLayers, LiveRampCutoffRecoloursOnlyItsLayerAndKeepsMeasurements)
{
    Fresh fresh;
    ol::Layer layer = Square ("clearance-heatmap");
    ol::Mesh mesh;
    mesh.points = { 0, 0, 0, 1, 0, 0, 1, 1, 0 };
    mesh.indices = { 0, 1, 2 };
    mesh.values = { 2.8, 2.8, 2.8 };
    ol::PresetStops ("clearance", mesh.colormap.stops);
    mesh.colormap.autoRange = false;
    mesh.colormap.max = 4;
    mesh.colormap.hasCutoff = true;
    mesh.colormap.cutoff = 2.2;
    mesh.colormap.cutoffControl = "cutoff";
    mesh.colormap.hasGreen = true;
    mesh.colormap.green = 3.2;
    mesh.colormap.greenControl = "green";
    layer.meshes = { mesh };
    ol::Panel panel;
    ol::PanelItem ramp;
    ramp.kind = ol::ItemKind::Ramp;
    ramp.colormap = mesh.colormap;
    panel.items = { ramp };
    layer.panels = { panel };
    ASSERT_TRUE (ol::Validate (layer).empty ());
    ol::Set (layer);
    ol::Set (Square ("unrelated"));
    EXPECT_FALSE (ol::SetRampCutoff ("clearance-heatmap", "unknown", 3));
    EXPECT_TRUE (ol::SetRampCutoff ("clearance-heatmap", "cutoff", 3));
    const auto changed = ol::Layers ()[0];
    EXPECT_DOUBLE_EQ (changed->meshes[0].colormap.cutoff, 3);
    EXPECT_DOUBLE_EQ (changed->panels[0].items[0].colormap.cutoff, 3);
    EXPECT_EQ (changed->meshes[0].points, mesh.points);
    EXPECT_EQ (changed->meshes[0].values, mesh.values);
    EXPECT_EQ (changed->meshes[0].colormap.stops.size (), mesh.colormap.stops.size ());
    const auto generation = ol::Generation ();
    EXPECT_FALSE (ol::SetRampCutoff ("clearance-heatmap", "cutoff", 3));
    EXPECT_EQ (ol::Generation (), generation);
    EXPECT_EQ (ol::Layers ()[1]->name, "unrelated");
    EXPECT_FALSE (ol::SetRampGreen ("clearance-heatmap", "unknown", 3.5));
    EXPECT_TRUE (ol::SetRampGreen ("clearance-heatmap", "green", 3.5));
    EXPECT_DOUBLE_EQ (ol::Layers ()[0]->meshes[0].colormap.green, 3.5);
    EXPECT_DOUBLE_EQ (ol::Layers ()[0]->panels[0].items[0].colormap.green, 3.5);
    EXPECT_EQ (ol::Layers ()[0]->meshes[0].points, mesh.points);
    EXPECT_TRUE (ol::SetRampGreen ("clearance-heatmap", "green", 1));
    EXPECT_DOUBLE_EQ (ol::Layers ()[0]->meshes[0].colormap.green, 3);
    EXPECT_TRUE (ol::SetRampCutoff ("clearance-heatmap", "cutoff", 4));
    EXPECT_DOUBLE_EQ (ol::Layers ()[0]->meshes[0].colormap.green, 4);
    EXPECT_TRUE (ol::Validate (*ol::Layers ()[0]).empty ());
    layer.meshes[0].colormap.green = 1;
    EXPECT_FALSE (ol::Validate (layer).empty ());
    layer.meshes[0].colormap.green = 3.2;
    layer.meshes[0].colormap.cutoff = 5;
    EXPECT_FALSE (ol::Validate (layer).empty ());
}

TEST (OverlayLayers, CommandBoundaryClearsOnlyOptInTransientLayers)
{
    Fresh fresh;
    ol::Layer transient = Square ("temporary-preview");
    transient.lifetime = ol::LayerLifetime::Command;
    ol::Set (std::move (transient));
    ol::Set (Square ("project-analysis"));
    EXPECT_TRUE (ol::ClearCommandScoped ());
    const auto remaining = ol::Layers ();
    ASSERT_EQ (remaining.size (), 1u);
    EXPECT_EQ (remaining[0]->name, "project-analysis");
    const auto generation = ol::Generation ();
    EXPECT_FALSE (ol::ClearCommandScoped ());
    EXPECT_EQ (ol::Generation (), generation);
}

TEST (OverlayLayers, ProjectBoundaryDropsTheStyledCopyCacheAsWell)
{
    Fresh fresh;
    ol::Set (Square ("project-preview"));
    std::weak_ptr<const ol::Layer> cached = ol::Layers ()[0];
    ol::ClearEverything ();
    EXPECT_TRUE (cached.expired ());
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

TEST (OverlayLayers, SemanticHoverCellsValidateTheirTriangleMapAndMeasuredEndpoints)
{
    ol::Layer layer;
    layer.name = "clearance";
    ol::Mesh mesh;
    mesh.points = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };
    mesh.values = { 2.8, 2.8, 2.8, 2.8 };
    ASSERT_TRUE (ol::PresetStops ("clearance", mesh.colormap.stops));
    ol::HoverCell cell;
    cell.triangles = { 0, 1 };
    cell.center[0] = cell.center[1] = 0.5;
    cell.from[0] = cell.to[0] = 0.5;
    cell.from[1] = cell.to[1] = 0.5;
    cell.from[2] = 0.2;
    cell.to[2] = 3.0;
    cell.normal[2] = 1.0;
    cell.floorGuid = "measured-floor";
    mesh.hoverCells = { cell };
    mesh.hoverCellByTriangle = { 0, 0 };
    layer.meshes = { mesh };
    EXPECT_TRUE (ol::Validate (layer).empty ()) << ol::Validate (layer);

    layer.meshes[0].hoverCells[0].triangles[1] = 4;
    EXPECT_NE (ol::Validate (layer).find ("triangle references"), std::string::npos);
}

TEST (OverlayLayers, AFinelyTessellatedCellIsNotLimitedToSixtyFourFragments)
{
    ol::Layer layer;
    layer.name = "clearance";
    ol::Mesh mesh;
    mesh.points = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    ol::HoverCell cell;
    cell.normal[2] = 1;
    cell.to[2] = 3;
    cell.floorGuid = "floor";
    for (uint32_t triangle = 0; triangle < 128; ++triangle) {
        mesh.indices.insert (mesh.indices.end (), { 0, 1, 2 });
        cell.triangles.push_back (triangle);
    }
    mesh.hoverCells = { cell };
    layer.meshes = { mesh };
    EXPECT_TRUE (ol::Validate (layer).empty ()) << ol::Validate (layer);
    layer.meshes[0].hoverCells[0].triangles.resize (4097);
    EXPECT_NE (ol::Validate (layer).find ("1 to 4096 triangles"), std::string::npos);
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
    layer.occlusion = ol::Behind::Show;
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

// A layer that fades its items behind the building: the raw pipelines know only hidden
// and over-everything, so its plain lines and fills are the guest's -- drawn once, faded.
TEST (OverlayLayers, AFadedLayerHandsItsPlainItemsToTheGuest)
{
    ol::Layer layer = Square ("faded");
    layer.occlusion = ol::Behind::Fade;
    ol::Mesh triangle;
    triangle.points = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    triangle.indices = { 0, 1, 2 };
    layer.meshes.push_back (triangle);
    EXPECT_TRUE (ol::DrawnByGuest (layer.polylines[0], layer));
    EXPECT_TRUE (ol::DrawnByGuest (layer.meshes[0], layer));
    EXPECT_EQ (ol::Resolve (ol::Behind::Layer, layer), ol::Behind::Fade);
    EXPECT_EQ (ol::Resolve (ol::Behind::Show, layer), ol::Behind::Show); // an item's own wins
    const ol::Prepared3D prepared = ol::Prepare3D ({ std::make_shared<const ol::Layer> (layer) });
    EXPECT_TRUE (prepared.occludedLines.empty ());
    EXPECT_TRUE (prepared.occludedFills.empty ());
    EXPECT_TRUE (prepared.overFills.empty ());
}
