#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/TerrainProjection.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "Geometry/Primitives.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ms = geomsrv::archviz::massingslices;
namespace tp = geomsrv::archviz::terrainprojection;
namespace layers = geomsrv::archviz::overlaylayers;
using geomsrv::archviz::SliceChain;
namespace {
SliceChain Ring (double x0, double y0, double x1, double y1, bool hole = false)
{
    SliceChain chain;
    chain.closed = true;
    chain.xy = hole ? std::vector<double> { x0, y0, x0, y1, x1, y1, x1, y0 }
                    : std::vector<double> { x0, y0, x1, y0, x1, y1, x0, y1 };
    return chain;
}
ms::Result Coverage (double ox = 0, double oy = 0)
{
    ms::Result result;
    result.hasCoverage = true;
    ms::CoveragePatch patch;
    patch.hasElevation = true;
    patch.z = 500; // This plane must never supply projected 3D elevations.
    patch.unbuilt = { Ring (ox, oy, ox + 10, oy + 10), Ring (ox + 4, oy + 4, ox + 6, oy + 6, true) };
    result.coverage.push_back (patch);
    return result;
}
geomsrv::Mesh Terrain (double ox = 0, double oy = 0)
{
    geomsrv::Mesh terrain;
    terrain.vertices = { ox - 5, oy - 5, 0, ox + 15, oy - 5, 0, ox + 15, oy + 15, 0, ox - 5, oy + 15, 0 };
    terrain.triangles = { 0, 1, 2, 0, 2, 3 };
    return terrain;
}
double LengthXY (const layers::Layer& layer)
{
    double length = 0;
    for (const auto& line : layer.polylines)
        for (size_t i = 3; i < line.points.size (); i += 3)
            length += std::hypot (line.points[i] - line.points[i - 3], line.points[i + 1] - line.points[i - 2]);
    return length;
}
} // namespace

TEST (TerrainProjection, UnbuiltHighlightIsStrokesOnlyOnSlopedGroundAndPlanWithMatchingPhase)
{
    const auto coverage = Coverage ();
    auto terrain = Terrain ();
    for (size_t i = 0; i < terrain.vertices.size (); i += 3)
        terrain.vertices[i + 2] = 12 + 0.5 * terrain.vertices[i] + 0.2 * terrain.vertices[i + 1];
    layers::Layer plan, projected;
    std::string error;
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error)) << error;
    EXPECT_TRUE (plan.meshes.empty ());
    EXPECT_TRUE (projected.meshes.empty ());
    ASSERT_FALSE (plan.polylines.empty ());
    ASSERT_FALSE (projected.polylines.empty ());
    EXPECT_EQ (plan.views, layers::Views::TwoD);
    EXPECT_EQ (projected.views, layers::Views::ThreeD);
    EXPECT_EQ (projected.occlusion, layers::Behind::Fade);
    EXPECT_EQ (projected.graphicsCategory, "coverageHighlight");
    EXPECT_NEAR (LengthXY (projected), LengthXY (plan), 2e-5);
    for (const auto& line : projected.polylines) {
        EXPECT_EQ (line.rgba, 0x66BB6AFF);
        EXPECT_FLOAT_EQ (line.widthPixels, 0.7f);
        for (size_t i = 0; i < line.points.size (); i += 3) {
            const double x = line.points[i], y = line.points[i + 1];
            EXPECT_NEAR (line.points[i + 2], 12 + 0.5 * x + 0.2 * y + 0.014, 1e-7);
            EXPECT_NEAR ((x + y) / 0.7, std::round ((x + y) / 0.7), 2e-6);
        }
        const double x = (line.points[0] + line.points[3]) / 2, y = (line.points[1] + line.points[4]) / 2;
        EXPECT_FALSE (x > 4 && x < 6 && y > 4 && y < 6) << "No hatch through built courtyard footprint";
    }
    const auto scene =
        geomsrv::archviz::overlayscene::PrepareScene ({ std::make_shared<const layers::Layer> (projected) }, nullptr);
    EXPECT_TRUE (scene.fills.empty ());
    ASSERT_FALSE (scene.lines.empty ());
    for (const auto& line : scene.lines)
        EXPECT_EQ (line.behind, geomsrv::archviz::overlayscene::kBehindFade);
    EXPECT_EQ (coverage.coverage[0].z, 500);
}

TEST (TerrainProjection, OverlappingParcelHatchesAreUnionedInsteadOfDoubleDrawn)
{
    auto coverage = Coverage ();
    const auto terrain = Terrain ();
    layers::Layer one, duplicate, projected;
    std::string error;
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, one, projected, error)) << error;
    const double projectedLength = LengthXY (projected);
    coverage.coverage.push_back (coverage.coverage.front ());
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, duplicate, projected, error)) << error;
    EXPECT_NEAR (LengthXY (duplicate), LengthXY (one), 1e-6);
    EXPECT_NEAR (LengthXY (projected), projectedLength, 1e-6);
}

TEST (TerrainProjection, MissingTerrainRetainsOnlyPlanHatchesAndNeverInventsThreeDMeanElevation)
{
    auto coverage = Coverage ();
    layers::Layer plan, projected;
    std::string error;
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, nullptr, plan, projected, error)) << error;
    ASSERT_FALSE (plan.polylines.empty ());
    EXPECT_TRUE (projected.meshes.empty ());
    EXPECT_TRUE (projected.polylines.empty ());
    EXPECT_EQ (plan.views, layers::Views::TwoD);
    EXPECT_NEAR (plan.polylines[0].points[2], 500.014, 1e-8);
    coverage.coverage[0].hasElevation = false;
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, nullptr, plan, projected, error));
    EXPECT_NEAR (plan.polylines[0].points[2], 0.014, 1e-8);
}

TEST (TerrainProjection, CreasesAtSurveyCoordinatesPreserveTerrainElevationAndGlobalHatchPhase)
{
    constexpr double ox = 700000, oy = 6000000;
    const auto coverage = Coverage (ox, oy);
    auto terrain = Terrain (ox, oy);
    terrain.vertices[2] = 100;
    terrain.vertices[5] = 120;
    terrain.vertices[8] = 100;
    terrain.vertices[11] = 130;
    layers::Layer plan, projected;
    std::string error;
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error)) << error;
    ASSERT_FALSE (projected.polylines.empty ());
    EXPECT_NEAR (LengthXY (projected), LengthXY (plan), 2e-5);
    for (const auto& line : projected.polylines)
        for (size_t i = 0; i < line.points.size (); i += 3) {
            const double x = line.points[i] - ox, y = line.points[i + 1] - oy;
            EXPECT_NEAR (line.points[i + 2], 100 + (x >= y ? x - y : 1.5 * (y - x)) + 0.014, 2e-6);
            const double phase = (line.points[i] + line.points[i + 1]) / 0.7;
            EXPECT_NEAR (phase, std::round (phase), 2e-6);
        }
}

TEST (TerrainProjection, ProjectionIgnoresUndersideAndClipsToMeshExtentAndGaps)
{
    const auto coverage = Coverage ();
    geomsrv::Mesh terrain;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 2.5, 5, -25 }, 5, 10, 50, terrain, error));
    layers::Layer plan, projected;
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error)) << error;
    ASSERT_FALSE (projected.polylines.empty ());
    for (const auto& line : projected.polylines)
        for (size_t i = 0; i < line.points.size (); i += 3) {
            EXPECT_GE (line.points[i], -1e-7);
            EXPECT_LE (line.points[i], 5 + 1e-7);
            EXPECT_NEAR (line.points[i + 2], 0.014, 1e-7);
        }
    EXPECT_LT (LengthXY (projected), LengthXY (plan));
    terrain.vertices = { 0, 0, 0, 4, 0, 0, 4, 10, 0, 0, 10, 0, 6, 0, 0, 10, 0, 0, 10, 10, 0, 6, 10, 0 };
    terrain.triangles = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error));
    for (const auto& line : projected.polylines) {
        const double x = (line.points[0] + line.points[3]) / 2;
        EXPECT_FALSE (x > 4 && x < 6) << "No invented elevation in missing terrain strip";
    }
    terrain = Terrain (100, 100);
    ASSERT_TRUE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error));
    EXPECT_TRUE (projected.polylines.empty ());
}

TEST (TerrainProjection, MalformedContoursTerrainAndStylesRefuseAtomically)
{
    auto coverage = Coverage ();
    auto terrain = Terrain ();
    layers::Layer plan, projected;
    plan.name = projected.name = "unchanged";
    std::string error;
    terrain.triangles[0] = 99999;
    EXPECT_FALSE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error));
    EXPECT_EQ (plan.name, "unchanged");
    EXPECT_EQ (projected.name, "unchanged");
    terrain = Terrain ();
    coverage.coverage[0].unbuilt[0].closed = false;
    EXPECT_FALSE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error));
    coverage = Coverage ();
    coverage.coverage[0].unbuilt[0].xy[0] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (ms::UnbuiltHighlight (coverage, &terrain, plan, projected, error));
    coverage = Coverage ();
    tp::Style style;
    style.name = "test";
    style.spacing = 0;
    EXPECT_FALSE (tp::HatchPlan (coverage.coverage[0].unbuilt, 0, 0, style, plan, error));
    EXPECT_FALSE (tp::Project (coverage.coverage[0].unbuilt, 0, terrain, style, projected, error));
    EXPECT_EQ (plan.name, "unchanged");
    coverage.hasCoverage = false;
    EXPECT_FALSE (ms::UnbuiltHighlight (coverage, nullptr, plan, projected, error));
}
