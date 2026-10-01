// ArchViz/OverlayHover: hover mode's pick in the plan (the user's stage 3) -- which item of
// the layers is under the pointer, in model metres, and what the HUD says of it: a storey
// slice's figures, a heatmap's value at that point.

#include "ArchViz/OverlayHover.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace layers = geomsrv::archviz::overlaylayers;
namespace hover = geomsrv::archviz::overlayhover;

namespace {

// Two triangles over the rectangle (x0, y0)-(x1, y1) at height z.
layers::Mesh Square (double x0, double y0, double x1, double y1, double z = 0.0)
{
    layers::Mesh mesh;
    mesh.points = { x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };
    return mesh;
}

std::shared_ptr<const layers::Layer> Layer (const std::string& name, std::vector<layers::Mesh> meshes,
                                            layers::Views views = layers::Views::Both)
{
    auto layer = std::make_shared<layers::Layer> ();
    layer->name = name;
    layer->views = views;
    layer->meshes = std::move (meshes);
    return layer;
}

layers::Mesh Slice (const char* name, double x0, double y0, double x1, double y1)
{
    layers::Mesh mesh = Square (x0, y0, x1, y1);
    mesh.hoverTitle = name;
    mesh.hoverRows = { { "Area", "100.0 m\xC2\xB2" } };
    return mesh;
}

} // namespace

// ⚠️ THE USER, 2026-09-29: hover mode shows a storey slice's figures under the pointer. The
// slice's own title and rows, and nothing off it.
TEST (OverlayHover, ASliceUnderThePointerSaysItsFigures)
{
    const std::vector<std::shared_ptr<const layers::Layer>> all = { Layer ("tapioca.storeySlices",
                                                                           { Slice ("A-01 F2", 0, 0, 10, 10) }) };
    const hover::Hit hit = hover::PickPlan (all, 4.0, 6.0);
    ASSERT_TRUE (hit.found);
    EXPECT_FALSE (hit.hasValue);
    const geomsrv::archviz::overlayhud::Hover said = hover::Readout (*all[0], hit);
    EXPECT_TRUE (said.active);
    EXPECT_EQ (said.title, "A-01 F2");
    ASSERT_EQ (said.rows.size (), 1u);
    EXPECT_EQ (said.rows[0].first, "Area");
    EXPECT_FALSE (hover::PickPlan (all, 12.0, 6.0).found) << "off it, nothing";
}

// What is drawn over is what is picked: the layer drawn last, and in it the mesh drawn last.
TEST (OverlayHover, TheItemDrawnOnTopIsTheOnePicked)
{
    const std::vector<std::shared_ptr<const layers::Layer>> all = {
        Layer ("first", { Slice ("under", 0, 0, 10, 10) }),
        Layer ("second", { Slice ("lower", 0, 0, 10, 10), Slice ("upper", 5, 0, 10, 10) }),
    };
    hover::Hit hit = hover::PickPlan (all, 7.0, 5.0);
    ASSERT_TRUE (hit.found);
    EXPECT_EQ (hit.layer, 1u);
    EXPECT_EQ (hit.mesh, 1u);
    hit = hover::PickPlan (all, 2.0, 5.0);
    ASSERT_TRUE (hit.found);
    EXPECT_EQ (hit.mesh, 0u) << "where the upper one is not, the lower";
}

// A heatmap says its value at the point, between its triangle's corners, in its legend's
// decimals and unit, before the mesh's own rows.
TEST (OverlayHover, AHeatmapSaysItsValueAtThePointInItsLegendsUnit)
{
    layers::Mesh heat = Square (0, 0, 10, 10);
    heat.values = { 0.0, 10.0, 10.0, 0.0 }; // rises with x
    auto layer = std::make_shared<layers::Layer> ();
    layer->name = "sun";
    layer->meshes = { heat };
    layers::Legend legend;
    legend.title = "Sun hours";
    legend.unit = "h";
    legend.decimals = 1;
    layer->legends = { legend };
    const std::vector<std::shared_ptr<const layers::Layer>> all = { layer };

    const hover::Hit hit = hover::PickPlan (all, 2.5, 5.0);
    ASSERT_TRUE (hit.found);
    ASSERT_TRUE (hit.hasValue);
    EXPECT_NEAR (hit.value, 2.5, 1e-9);
    const geomsrv::archviz::overlayhud::Hover said = hover::Readout (*layer, hit);
    EXPECT_EQ (said.title, "sun") << "no title of its own: the layer's name";
    ASSERT_EQ (said.rows.size (), 1u);
    EXPECT_EQ (said.rows[0].first, "Sun hours");
    EXPECT_EQ (said.rows[0].second, "2.5 h");
}

// A plain fill says nothing and is passed over: what is under it is found. A layer the plan
// does not draw is not picked there.
TEST (OverlayHover, WhatSaysNothingOrIsNotInThePlanIsPassedOver)
{
    const std::vector<std::shared_ptr<const layers::Layer>> all = {
        Layer ("slices", { Slice ("A-01 F1", 0, 0, 10, 10) }),
        Layer ("decoration", { Square (0, 0, 10, 10) }),
        Layer ("only3d", { Slice ("3D", 0, 0, 10, 10) }, layers::Views::ThreeD),
    };
    const hover::Hit hit = hover::PickPlan (all, 5.0, 5.0);
    ASSERT_TRUE (hit.found);
    EXPECT_EQ (hit.layer, 0u);
}

// A slice is tinted whole; a heatmap, the triangle under the pointer -- through the plan's
// transform to view pixels.
TEST (OverlayHover, TheTintIsTheItemOrItsCellInViewPixels)
{
    const layers::Mesh mesh = Square (0, 0, 10, 10);
    const hover::Project twice = [] (double x, double y, float& px, float& py) {
        px = float (x * 2.0 + 100.0);
        py = float (500.0 - y * 2.0);
    };
    std::vector<float> whole;
    hover::Tint (mesh, twice, whole);
    EXPECT_EQ (whole.size (), 12u) << "two triangles, three corners, x and y";
    EXPECT_FLOAT_EQ (whole[0], 100.0f);
    EXPECT_FLOAT_EQ (whole[1], 500.0f);
    std::vector<float> cell;
    hover::Tint (mesh, twice, cell, 1);
    ASSERT_EQ (cell.size (), 6u);
    EXPECT_FLOAT_EQ (cell[4], 100.0f); // its third corner, (0, 10)
    EXPECT_FLOAT_EQ (cell[5], 480.0f);
}
