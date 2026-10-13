#include "ArchViz/OverlayScene.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <iterator>

namespace scene = geomsrv::archviz::overlayscene;
namespace layers = geomsrv::archviz::overlaylayers;
namespace hud = geomsrv::archviz::overlayhud;
namespace {
geomsrv::archviz::overlaytext::Engine& TextEngine ()
{
    static geomsrv::archviz::overlaytext::Engine engine;
    if (!engine.Ready ()) {
        std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
        std::vector<uint8_t> font { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
        std::string error;
        EXPECT_TRUE (engine.Init (std::move (font), error)) << error;
    }
    return engine;
}
layers::Layer Cells ()
{
    layers::Layer layer;
    layer.name = "cells";
    layers::Mesh mesh;
    mesh.styled = true;
    mesh.cellLabels = true;
    mesh.colormap.autoRange = false;
    mesh.colormap.max = 4;
    mesh.colormap.hasGreen = true;
    mesh.colormap.green = 3.2;
    mesh.colormap.hasCutoff = true;
    mesh.colormap.cutoff = 2.2;
    layers::PresetStops ("clearance", mesh.colormap.stops);
    for (uint32_t i = 0; i < 2; ++i) {
        const double x = i;
        const uint32_t vertex = uint32_t (mesh.points.size () / 3);
        mesh.points.insert (mesh.points.end (), { x, 0, 0, x + 1, 0, 0, x + 1, 1, 0, x, 1, 0 });
        mesh.indices.insert (mesh.indices.end (), { vertex, vertex + 1, vertex + 2, vertex, vertex + 2, vertex + 3 });
        mesh.values.insert (mesh.values.end (), 4, 2.8);
        layers::HoverCell cell;
        cell.triangles = { i * 2, i * 2 + 1 };
        cell.center[0] = cell.from[0] = cell.to[0] = x + 0.5;
        cell.center[1] = cell.from[1] = cell.to[1] = 0.5;
        cell.to[2] = 2.8;
        mesh.hoverCells.push_back (cell);
    }
    layer.meshes = { mesh };
    return layer;
}
hud::Input View (double zoom)
{
    hud::Input input;
    input.width = 1000;
    input.height = 800;
    input.planar = true;
    input.plan[0] = zoom;
    input.plan[4] = -zoom;
    input.plan[2] = 100;
    input.plan[5] = 500;
    return input;
}
scene::Plan Prepare (const layers::Layer& layer, hud::Input input, hud::Engine* host = nullptr)
{
    return scene::PreparePlanHud ({ std::make_shared<const layers::Layer> (layer) }, host, 1, input, nullptr,
                                  &TextEngine ());
}
size_t CellGlyphs (const scene::Plan& frame)
{
    return size_t (std::count_if (frame.glyphs.begin (), frame.glyphs.end (),
                                  [] (const auto& v) { return (v.flags & scene::kModelQuad) != 0; }));
}
} // namespace

TEST (OverlaySceneCells, EveryVisibleCellGetsACompleteDarkerModelSizedValueOnlyAtCloseZoom)
{
    const auto layer = Cells ();
    EXPECT_EQ (CellGlyphs (Prepare (layer, View (30))), 0u);
    const auto close = Prepare (layer, View (100));
    ASSERT_EQ (CellGlyphs (close), 48u) << "both four-character values, six vertices per character";
    const uint32_t color = layers::ToUnorm (scene::RampTextColor (layer.meshes[0].colormap, 2.8, 0, 4));
    for (const auto& glyph : close.glyphs) {
        EXPECT_EQ (glyph.rgba, color);
        EXPECT_NE (glyph.flags & scene::kWholeLabel, 0u);
        EXPECT_EQ (glyph.flags & scene::kSolid, 0u);
        EXPECT_EQ (glyph.haloPixels, 0);
    }
    auto clipped = View (100);
    clipped.width = 240; // second value extends beyond the view: never upload half a number
    EXPECT_EQ (CellGlyphs (Prepare (layer, clipped)), 24u);
}

TEST (OverlaySceneCells, ALabelAcrossAMissingTriangleIsHiddenNotPatchedOrCopied)
{
    auto layer = Cells ();
    layer.meshes[0].hoverCells[0].triangles = { 0 };
    EXPECT_EQ (CellGlyphs (Prepare (layer, View (100))), 24u);
    layer.meshes[0].cellLabels = false;
    EXPECT_EQ (CellGlyphs (Prepare (layer, View (100))), 0u);
}

TEST (OverlaySceneCells, CoincidentPlanLayersNeverPrintOverlappingValues)
{
    const auto layer = Cells ();
    auto top = layer;
    top.name = "top";
    const auto frame = scene::PreparePlanHud (
        { std::make_shared<const layers::Layer> (layer), std::make_shared<const layers::Layer> (top) }, nullptr, 1,
        View (100), nullptr, &TextEngine ());
    EXPECT_EQ (CellGlyphs (frame), 48u);
}

TEST (OverlaySceneCells, LayerVisibilityAndLiveGradientApplyToPlanValues)
{
    hudtest::Watched host;
    auto layer = Cells ();
    const auto before = Prepare (layer, View (100), &host.engine);
    ASSERT_EQ (CellGlyphs (before), 48u);
    layer.meshes[0].colormap.green = 2.8;
    const auto after = Prepare (layer, View (100), &host.engine);
    ASSERT_EQ (CellGlyphs (after), 48u);
    const auto color = [] (const auto& frame) {
        return std::find_if (frame.glyphs.begin (), frame.glyphs.end (),
                             [] (const auto& v) { return (v.flags & scene::kModelQuad) != 0; })
            ->rgba;
    };
    EXPECT_NE (color (before), color (after));
    hud::SetLayerShown (*host.state, layer.name, false);
    EXPECT_EQ (CellGlyphs (Prepare (layer, View (100), &host.engine)), 0u);
    hud::SetLayerShown (*host.state, layer.name, true);
    hud::SetContentShown (*host.state, false);
    EXPECT_EQ (CellGlyphs (Prepare (layer, View (100), &host.engine)), 0u);
}

TEST (OverlaySceneCells, CloseZoomHoverDoesNotPrintTheSameValueTwice)
{
    hudtest::Watched host;
    hud::SetHoverMode (*host.state, true);
    auto layer = Cells ();
    auto input = View (100);
    input.pointer = true;
    input.x = 150;
    input.y = 450;
    input.hover.active = input.hover.hasMeasuredCell = true;
    input.hover.layer = layer.name;
    input.hover.cellCenter[0] = input.hover.cellCenter[1] = 0.5;
    input.hover.measuredValue = 2.8;
    input.hover.tintModel = { 0, 0, 0, 1, 0, 0, 1, 1, 0 };
    const auto frame = Prepare (layer, input, &host.engine);
    EXPECT_EQ (CellGlyphs (frame), 48u);
    EXPECT_FALSE (std::any_of (frame.glyphs.begin (), frame.glyphs.end (), [] (const auto& glyph) {
        return (glyph.flags & (scene::kScreenAnchored | scene::kModelQuad)) == 0;
    }));
}

TEST (OverlaySceneCells, PerTextViewsDoNotLeakThreeDLabelsOntoThePlan)
{
    auto layer = Cells ();
    layers::Text text;
    text.text = "3D only";
    text.views = layers::Views::ThreeD;
    layer.texts = { text };
    EXPECT_TRUE (
        scene::PreparePlan ({ std::make_shared<const layers::Layer> (layer) }, &TextEngine ()).glyphs.empty ());
    EXPECT_FALSE (
        scene::PrepareScene ({ std::make_shared<const layers::Layer> (layer) }, &TextEngine ()).glyphs.empty ());
}

TEST (OverlaySceneCells, CoincidentStoreysUseTheLastDrawnCellAndDoNotDuplicateItsHoverValue)
{
    hudtest::Watched host;
    hud::SetHoverMode (*host.state, true);
    auto layer = Cells ();
    auto& mesh = layer.meshes[0];
    mesh.points.insert (mesh.points.end (), { 0, 0, 3, 1, 0, 3, 1, 1, 3, 0, 1, 3 });
    mesh.indices.insert (mesh.indices.end (), { 8, 9, 10, 8, 10, 11 });
    mesh.values.insert (mesh.values.end (), 4, 3.1);
    auto top = mesh.hoverCells[0];
    top.triangles = { 4, 5 };
    top.center[2] = top.from[2] = 3;
    top.to[2] = 6.1;
    mesh.hoverCells.push_back (top);
    auto input = View (100);
    input.pointer = true;
    input.x = 150;
    input.y = 450;
    input.hover.active = input.hover.hasMeasuredCell = true;
    input.hover.layer = layer.name;
    std::copy (std::begin (top.center), std::end (top.center), input.hover.cellCenter);
    input.hover.measuredValue = 3.1;
    input.hover.tintModel = { 0, 0, 3, 1, 0, 3, 1, 1, 3 };
    const auto frame = Prepare (layer, input, &host.engine);
    EXPECT_EQ (CellGlyphs (frame), 48u);
    EXPECT_FALSE (std::any_of (frame.glyphs.begin (), frame.glyphs.end (), [] (const auto& glyph) {
        return (glyph.flags & (scene::kScreenAnchored | scene::kModelQuad)) == 0;
    }));
}

TEST (OverlaySceneCells, GreenAnchorCompressesTheRampAndDuplicateStopsMatchTheShader)
{
    auto map = Cells ().meshes[0].colormap;
    map.cutoff = 2;
    map.green = 3;
    const auto compressed = scene::RampStops (map);
    EXPECT_EQ (scene::RampAt (compressed, 0.49f), 0xD73027FFu);
    EXPECT_EQ (scene::RampAt (compressed, 0.75f), map.stops.back ().rgba);
    EXPECT_EQ (scene::RampAt (compressed, 1), map.stops.back ().rgba);
    map.green = 4;
    EXPECT_NE (scene::RampAt (scene::RampStops (map), 0.6f), scene::RampAt (compressed, 0.6f));
    const std::vector<layers::ColourStop> seam {
        { 0, 0xD73027FFu }, { 0.5f, 0xD73027FFu }, { 0.5f, 0x1A9850FFu }, { 1, 0x1A9850FFu }
    };
    EXPECT_EQ (scene::RampAt (seam, 0.5f), 0x1A9850FFu);
}
