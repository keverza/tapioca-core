#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/RoofSlopeHud.hpp"
#include "ArchViz/RoofSlopeBake.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <set>

namespace rs = geomsrv::archviz::roofslope;
namespace rb = geomsrv::archviz::roofslopebake;
namespace js = evp::nodegraph::json;
namespace {
rs::Data Plan (const std::vector<rs::Ring>& rings)
{
    rs::Data data;
    std::string error;
    EXPECT_TRUE (rs::Plan (rings, 5, 2, 200, data, error)) << error;
    return data;
}
double Number (const js::JsonValue& value)
{
    double out = 0;
    EXPECT_TRUE (value.AsDouble (out));
    return out;
}
} // namespace
TEST (RoofSlopeBake, FourPlanesNotRenderTrianglesAndOneAtomicAnnotatedRecipe)
{
    auto data =
        Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } }, { { 2, 0.5 }, { 2, 1.5 }, { 3, 1.5 }, { 3, 0.5 } } });
    js::JsonValue request;
    std::string error;
    size_t roofs = 0, levels = 0;
    ASSERT_TRUE (rb::Transaction (data, -1, 1.5, "styled-symbol", request, roofs, levels, error)) << error;
    EXPECT_EQ (roofs, 4u);
    EXPECT_GT (data.triangles.size (), roofs);
    EXPECT_GT (levels, 0u);
    const auto* steps = request.Find ("steps")->AsArray ();
    ASSERT_NE (steps, nullptr);
    EXPECT_EQ (steps->size (), roofs * 2 + levels);
    size_t holes = 0, symbols = 0, dimensions = 0;
    std::set<size_t> parents;
    for (size_t i = 0; i < steps->size (); ++i) {
        std::string text, command;
        ASSERT_TRUE ((*steps)[i].AsString (text));
        const auto parsed = js::Parse (text);
        ASSERT_TRUE (parsed.ok);
        ASSERT_TRUE (parsed.value.Find ("command")->AsString (command));
        const auto& params = *parsed.value.Find ("params");
        if (command == "Tapioca.CreateRoof") {
            parents.insert (i);
            holes += params.Find ("holes")->AsArray ()->size ();
            const auto* pivot = params.Find ("baseLine")->AsArray ();
            ASSERT_EQ (pivot->size (), 4u);
            const double dx = Number ((*pivot)[2]) - Number ((*pivot)[0]);
            const double dy = Number ((*pivot)[3]) - Number ((*pivot)[1]);
            const double slope = std::tan (Number (*params.Find ("pitch")) * 3.141592653589793 / 180);
            EXPECT_NEAR (std::hypot (dx, dy), 1, 1e-8);
            EXPECT_GT (slope, 0);
        }
        else if (command == "Tapioca.PlaceLibraryObject") {
            ++symbols;
            EXPECT_NE (params.Find ("inheritFrom"), nullptr);
            EXPECT_NE (params.Find ("parameters"), nullptr);
        }
        else {
            EXPECT_EQ (command, "Tapioca.PlaceLevelDimension");
            EXPECT_DOUBLE_EQ (Number (*params.Find ("floorInd")), -1);
            ++dimensions;
            const auto* bindings = parsed.value.Find ("bindings")->AsArray ();
            ASSERT_EQ (bindings->size (), 1u);
            ASSERT_TRUE (bindings->front ().AsString (text));
            const auto binding = js::Parse (text);
            int64_t parent = -1;
            ASSERT_TRUE (binding.value.Find ("step")->AsInteger (parent));
            EXPECT_TRUE (parents.contains (size_t (parent)));
            EXPECT_LT (parent, int64_t (i));
            ASSERT_TRUE (binding.value.Find ("path")->AsString (text));
            EXPECT_EQ (text, "parent.elementId.guid");
        }
    }
    EXPECT_GT (holes, 0u);
    EXPECT_EQ (symbols, roofs);
    EXPECT_EQ (dimensions, levels);
}
TEST (RoofSlopeBake, RejectsInvalidDrainsBeforeProducingAnyWriteRecipe)
{
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    data.cells[0].drainInside = false;
    js::JsonValue request;
    std::string error;
    size_t roofs = 0, levels = 0;
    EXPECT_FALSE (rb::Transaction (data, 0, 1.5, {}, request, roofs, levels, error));
    EXPECT_TRUE (request.IsNull ());
    EXPECT_EQ (roofs, 0u);
}
TEST (RoofSlopeBake, RecipeReferencePlanesMatchTheEditedSurfaceAfterRotationAndHeightEdits)
{
    auto data = Plan ({ { { 100, 200 }, { 110, 205 }, { 105, 215 }, { 95, 210 } } });
    std::string error;
    ASSERT_TRUE (rs::LiftCell (data, 0, 0.4, error)) << error;
    js::JsonValue request;
    size_t roofs = 0, levels = 0;
    ASSERT_TRUE (rb::Transaction (data, -2, 1.5, {}, request, roofs, levels, error)) << error;
    double gx = 0, gy = 0;
    for (const auto& step : *request.Find ("steps")->AsArray ()) {
        std::string encoded, command;
        ASSERT_TRUE (step.AsString (encoded));
        const auto parsed = js::Parse (encoded);
        ASSERT_TRUE (parsed.value.Find ("command")->AsString (command));
        const auto& params = *parsed.value.Find ("params");
        if (command == "Tapioca.CreateRoof") {
            const auto& pivot = *params.Find ("baseLine")->AsArray ();
            const double x = (Number (pivot[0]) + Number (pivot[2])) / 2;
            const double y = (Number (pivot[1]) + Number (pivot[3])) / 2;
            const double dx = Number (pivot[2]) - Number (pivot[0]), dy = Number (pivot[3]) - Number (pivot[1]);
            const double slope = std::tan (Number (*params.Find ("pitch")) * 3.141592653589793 / 180);
            gx = -dy * slope, gy = dx * slope;
            const auto& outline = *params.Find ("outline")->AsArray ();
            bool matchingPlane = false;
            for (const auto& t : data.triangles) {
                bool matches = true;
                for (size_t i = 0; i < outline.size (); i += 2) {
                    const double wx = Number (outline[i]), wy = Number (outline[i + 1]);
                    const auto local = rs::Local (data, { wx, wy });
                    const double expected =
                        t.vertices[0].z + t.gx * (local.x - t.vertices[0].xy.x) + t.gy * (local.y - t.vertices[0].xy.y);
                    const double actual = Number (*params.Find ("base")) + gx * (wx - x) + gy * (wy - y);
                    matches = matches && std::abs (actual - expected) < 1e-6;
                }
                matchingPlane = matchingPlane || matches;
            }
            EXPECT_TRUE (matchingPlane);
        }
        if (command == "Tapioca.PlaceLibraryObject") {
            const double bearing = Number (*params.Find ("angle"));
            EXPECT_LT (gx * std::cos (bearing) + gy * std::sin (bearing), 0);
        }
    }
}
TEST (RoofSlopeBake, RequestIsExplicitOnceAndCancelPreventsIt)
{
    geomsrv::archviz::roofhud::Session session;
    session.editor.data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    EXPECT_FALSE (geomsrv::archviz::roofhud::RequestBake (session));
    session.sourceGuid = "slab";
    EXPECT_TRUE (geomsrv::archviz::roofhud::RequestBake (session));
    EXPECT_FALSE (geomsrv::archviz::roofhud::RequestBake (session));
    session.bakeRequested = false;
    session.close = true;
    EXPECT_FALSE (geomsrv::archviz::roofhud::RequestBake (session));
}
TEST (RoofSlopeEdges, AddBalancesChildrenWithoutChangingTheSourceFootprint)
{
    auto data = Plan ({ { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } } });
    const auto outline = data.outline;
    const auto box = data.cells[0].box;
    std::string error;
    ASSERT_TRUE (rs::AddDivider (data, 0, 0, { box[0], (box[1] + box[3]) / 2 }, error)) << error;
    EXPECT_EQ (data.cells.size (), 4u);
    EXPECT_EQ (data.outline, outline);
    EXPECT_LE (rs::Measure (data).maxAspect, rs::kMaxAspect + 1e-8);
    double area = 0;
    for (const auto& t : data.triangles)
        area += t.area;
    EXPECT_NEAR (area, 100, 1e-4);
    data.nodes[data.cells[0].drain].lockXY = true;
    EXPECT_FALSE (rs::AddDivider (data, 0, 0, { box[0], (box[1] + box[3]) / 4 }, error));
    EXPECT_EQ (data.cells.size (), 4u);
}
TEST (RoofSlopeLabels, WorldTextIsLargerAndParallelToItsArrowOnTheSurface)
{
    auto data = Plan ({ { { 100, 200 }, { 110, 205 }, { 105, 215 }, { 95, 210 } } });
    const auto layer = rs::Layer (data);
    size_t arrows = 0;
    for (const auto& line : layer.polylines) {
        if (line.endArrow != geomsrv::archviz::overlaylayers::Terminator::Arrow)
            continue;
        const auto& text = layer.texts[arrows++];
        const double dx = line.points[3] - line.points[0], dy = line.points[4] - line.points[1];
        EXPECT_GE (text.sizeMetres, 0.30);
        EXPECT_NEAR (dx * text.direction[1] - dy * text.direction[0], 0, 1e-8);
        double tangent = 0;
        for (int i = 0; i < 3; ++i)
            tangent += text.normal[i] * text.direction[i];
        EXPECT_NEAR (tangent, 0, 1e-8);
    }
    EXPECT_EQ (arrows, layer.texts.size ());
    EXPECT_GT (arrows, 0u);
}
