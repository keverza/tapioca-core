#include "ArchViz/MassingCollapseZone.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Geometry/Primitives.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <limits>

namespace zone = geomsrv::archviz::massingcollapse;
namespace slices = geomsrv::archviz::massingslices;
namespace cp = Clipper2Lib;
namespace {
geomsrv::Mesh Box (double x = 0, double y = 0, double width = 10, double depth = 10, double base = 0, double height = 3)
{
    geomsrv::Mesh mesh;
    std::string error;
    EXPECT_TRUE (geomsrv::engine::MakeBox ({ x + width / 2, y + depth / 2, base + height / 2 }, width, depth, height,
                                           mesh, error));
    return mesh;
}
slices::Input Input (geomsrv::Mesh mesh, std::string guid = "building")
{
    slices::Input input;
    input.slab.guid = std::move (guid);
    input.body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
    return input;
}
cp::RectD Bounds (const zone::Result& result)
{
    cp::PathsD paths;
    for (const auto& chain : result.chains) {
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i)
            path.emplace_back (chain.xy[i * 2], chain.xy[i * 2 + 1]);
        paths.push_back (std::move (path));
    }
    return cp::GetBounds (paths);
}
void Append (geomsrv::Mesh& mesh, const geomsrv::Mesh& other)
{
    const auto offset = uint32_t (mesh.VertexCount ());
    mesh.vertices.insert (mesh.vertices.end (), other.vertices.begin (), other.vertices.end ());
    for (auto index : other.triangles)
        mesh.triangles.push_back (index + offset);
}
geomsrv::Mesh Courtyard ()
{
    std::vector<geomsrv::archviz::SliceChain> chains;
    for (const auto& xy :
         { std::vector<double> { 0, 0, 10, 0, 10, 10, 0, 10 }, std::vector<double> { 3, 3, 3, 7, 7, 7, 7, 3 } }) {
        geomsrv::archviz::SliceChain chain;
        chain.closed = true;
        chain.xy = xy;
        chains.push_back (chain);
    }
    geomsrv::Mesh mesh;
    const auto vertex = [&] (double x, double y, double z) {
        mesh.triangles.push_back (uint32_t (mesh.VertexCount ()));
        mesh.vertices.insert (mesh.vertices.end (), { x, y, z });
    };
    const std::vector<double> capXY { 0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 7, 3, 7, 7, 3, 7 };
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        for (const auto& tri : { std::array<int, 3> { i, j, j + 4 }, std::array<int, 3> { i, j + 4, i + 4 } }) {
            for (int k : { 0, 1, 2 })
                vertex (capXY[tri[k] * 2], capXY[tri[k] * 2 + 1], 1.5);
            for (int k : { 2, 1, 0 })
                vertex (capXY[tri[k] * 2], capXY[tri[k] * 2 + 1], 0);
        }
    }
    for (const auto& chain : chains)
        for (size_t i = 0; i < chain.Count (); ++i) {
            const size_t j = (i + 1) % chain.Count ();
            const double ax = chain.xy[i * 2], ay = chain.xy[i * 2 + 1], bx = chain.xy[j * 2], by = chain.xy[j * 2 + 1];
            vertex (ax, ay, 0);
            vertex (bx, by, 0);
            vertex (bx, by, 1.5);
            vertex (ax, ay, 0);
            vertex (bx, by, 1.5);
            vertex (ax, ay, 1.5);
        }
    return mesh;
}
} // namespace

TEST (MassingCollapse, FlatBuildingHasOneThirdHeightRoundOffsetAndOneHatchedUnionFill)
{
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Box ()) }, 0, result, error)) << error;
    ASSERT_EQ (result.layer.meshes.size (), 1u);
    EXPECT_FALSE (result.layer.polylines.empty ());
    const auto bounds = Bounds (result);
    EXPECT_NEAR (bounds.left, -0.9999, 0.011);
    EXPECT_NEAR (bounds.right, 10.9999, 0.011);
    EXPECT_NEAR (result.area, 100 + 40 * 0.9999 + 3.14159265359 * 0.9999 * 0.9999, 0.6);
    EXPECT_EQ (result.layer.meshes[0].rgba, 0xAA446528);
    EXPECT_TRUE (geomsrv::archviz::overlaylayers::Validate (result.layer).empty ());
}

TEST (MassingCollapse, SlopedTopUsesTheLocalHeightAtBothEndsNotTheBoundingBoxMaximum)
{
    auto wedge = Box ();
    for (size_t i = 0; i < wedge.vertices.size (); i += 3)
        if (wedge.vertices[i + 2] > 0)
            wedge.vertices[i + 2] += 0.6 * wedge.vertices[i];
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (wedge) }, 0, result, error)) << error;
    const auto bounds = Bounds (result);
    EXPECT_NEAR (bounds.left, -0.9999, 0.011);  // local 3 m height
    EXPECT_NEAR (bounds.right, 12.9997, 0.011); // local 9 m height
    EXPECT_GT (bounds.left, -1.1) << "not a uniform 3 m offset from the bbox";
}

TEST (MassingCollapse, SlopingTopAndBottomUseLocalVerticalSpanNotGlobalHighestMinusLowest)
{
    auto sloping = Box ();
    for (size_t i = 0; i < sloping.vertices.size (); i += 3)
        sloping.vertices[i + 2] += 0.6 * sloping.vertices[i];
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (sloping) }, 0, result, error)) << error;
    const auto bounds = Bounds (result);
    EXPECT_NEAR (bounds.left, -0.9999, 0.011);
    EXPECT_NEAR (bounds.right, 10.9999, 0.011);
}

TEST (MassingCollapse, AHeightStepKeepsTheLowSideLocalInsteadOfUsingTheTallSidesHeight)
{
    const std::vector<std::pair<double, double>> profile {
        { 0, 0 }, { 10, 0 }, { 10, 9 }, { 5, 9 }, { 5, 3 }, { 0, 3 }
    };
    geomsrv::Mesh mesh;
    const auto vertex = [&] (int at, double y) {
        mesh.triangles.push_back (uint32_t (mesh.VertexCount ()));
        mesh.vertices.insert (mesh.vertices.end (), { profile[at].first, y, profile[at].second });
    };
    for (const auto& tri : { std::array<int, 3> { 0, 1, 4 }, std::array<int, 3> { 1, 2, 3 },
                             std::array<int, 3> { 1, 3, 4 }, std::array<int, 3> { 0, 4, 5 } }) {
        for (int k : { 0, 1, 2 })
            vertex (tri[k], 0);
        for (int k : { 2, 1, 0 })
            vertex (tri[k], 10);
    }
    for (int i = 0; i < 6; ++i) {
        const int j = (i + 1) % 6;
        vertex (i, 0);
        vertex (j, 0);
        vertex (j, 10);
        vertex (i, 0);
        vertex (j, 10);
        vertex (i, 10);
    }
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (mesh) }, 0, result, error)) << error;
    EXPECT_NEAR (Bounds (result).left, -0.9999, 0.011);
    EXPECT_NEAR (Bounds (result).right, 12.9997, 0.011);
}

TEST (MassingCollapse, SeoCourtyardSurvivesAndHatchesAreClippedAroundTheRemainingHole)
{
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Courtyard ()) }, 0, result, error)) << error;
    ASSERT_EQ (result.chains.size (), 2u);
    cp::PathD hole;
    for (const auto& chain : result.chains) {
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i)
            path.emplace_back (chain.xy[i * 2], chain.xy[i * 2 + 1]);
        if (cp::Area (path) < 0)
            hole = std::move (path);
    }
    EXPECT_EQ (cp::PointInPolygon (cp::PointD (5, 5), hole), cp::PointInPolygonResult::IsInside);
    for (const auto& line : result.layer.polylines) {
        ASSERT_GE (line.points.size (), 6u);
        const cp::PointD midpoint ((line.points[0] + line.points[3]) / 2, (line.points[1] + line.points[4]) / 2);
        EXPECT_NE (cp::PointInPolygon (midpoint, hole), cp::PointInPolygonResult::IsInside);
    }
}

TEST (MassingCollapse, OverlappingAndDisconnectedBuildingsAreUnionedWithoutDoubleFill)
{
    zone::Result one, both, duplicate;
    std::string error;
    const auto first = Input (Box ());
    ASSERT_TRUE (zone::Build ({ first }, 0, one, error));
    ASSERT_TRUE (zone::Build ({ first, Input (Box (), "duplicate") }, 0, duplicate, error));
    EXPECT_NEAR (duplicate.area, one.area, 1e-6);
    ASSERT_EQ (duplicate.layer.meshes.size (), 1u);
    ASSERT_TRUE (zone::Build ({ first, Input (Box (5), "second") }, 0, both, error));
    EXPECT_LT (both.area, 2 * one.area);
    EXPECT_GT (both.area, one.area);
    ASSERT_EQ (both.layer.meshes.size (), 1u);
    auto split = Box (0, 0, 2, 2);
    Append (split, Box (20, 0, 2, 2, 0, 9));
    ASSERT_TRUE (zone::Build ({ Input (split) }, 0, both, error));
    EXPECT_EQ (both.chains.size (), 2u);
    EXPECT_NEAR (Bounds (both).left, -0.9999, 0.011);
    EXPECT_NEAR (Bounds (both).right, 24.9997, 0.011);
}

TEST (MassingCollapse, StackedSlabsOfOneBuildingUseItsLocalBaseToTopHeight)
{
    auto lower = Input (Box (), "lower"), upper = Input (Box (0, 0, 10, 10, 3, 3), "upper");
    geomsrv::metadata::Property id;
    id.key = "massing.buildingId";
    id.value = geomsrv::metadata::Value::Text ("Tower");
    geomsrv::metadata::SetProperty (lower.metadata, id);
    geomsrv::metadata::SetProperty (upper.metadata, id);
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ lower, upper }, 0, result, error));
    EXPECT_NEAR (Bounds (result).left, -1.9998, 0.011);
}

TEST (MassingCollapse, MissingInvalidAndOpenBodiesRefuseAtomicallyWithoutFlatFallback)
{
    zone::Result result;
    result.area = 123;
    std::string error;
    slices::Input missing;
    EXPECT_FALSE (zone::Build ({ missing }, 0, result, error));
    EXPECT_EQ (result.area, 123);
    auto open = Box ();
    open.triangles.resize (open.triangles.size () - 3);
    EXPECT_FALSE (zone::Build ({ Input (open) }, 0, result, error));
    auto invalid = Box ();
    invalid.triangles[0] = 999999;
    EXPECT_FALSE (zone::Build ({ Input (invalid) }, 0, result, error));
    invalid = Box ();
    invalid.vertices[0] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (zone::Build ({ Input (invalid) }, 0, result, error));
    EXPECT_EQ (result.area, 123);
}

TEST (MassingCollapse, SurveyCoordinatesAndProjectElevationDoNotInflateLocalHeight)
{
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Box (700000, 6000000, 10, 10, 120, 3)) }, 120, result, error)) << error;
    EXPECT_NEAR (Bounds (result).left, 700000 - 0.9999, 0.011);
    for (const auto& line : result.layer.polylines)
        EXPECT_NEAR (line.points[2], 120.012, 1e-8);
}

TEST (MassingCollapse, MassingToggleDefaultsOffAndIsResetWithSharedHudState)
{
    auto state = geomsrv::archviz::overlayhud::NewState ();
    EXPECT_FALSE (geomsrv::archviz::overlayhud::MassingCollapseZone (*state));
    state->massingCollapseZone = true;
    EXPECT_TRUE (geomsrv::archviz::overlayhud::MassingCollapseZone (*state));
    geomsrv::archviz::overlayhud::ClearState (*state);
    EXPECT_FALSE (geomsrv::archviz::overlayhud::MassingCollapseZone (*state));
}

TEST (MassingCollapse, ThreeDProjectionFollowsTopographyForFillAndThinHatchesNotMeanZ)
{
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Box ()) }, 120, result, error)) << error;
    geomsrv::Mesh terrain;
    terrain.vertices = { -5, -5, 0, 15, -5, 0, 15, 15, 0, -5, 15, 0 };
    for (size_t i = 0; i < terrain.vertices.size (); i += 3)
        terrain.vertices[i + 2] = 0.5 * terrain.vertices[i] + 0.2 * terrain.vertices[i + 1];
    terrain.triangles = { 0, 1, 2, 0, 2, 3 };
    geomsrv::archviz::overlaylayers::Layer projected;
    ASSERT_TRUE (zone::Project (result, terrain, projected, error)) << error;
    EXPECT_EQ (projected.views, geomsrv::archviz::overlaylayers::Views::ThreeD);
    EXPECT_EQ (projected.occlusion, geomsrv::archviz::overlaylayers::Behind::Fade);
    ASSERT_EQ (projected.meshes.size (), 1u);
    EXPECT_FALSE (projected.polylines.empty ());
    EXPECT_EQ (geomsrv::archviz::overlaylayers::Resolve (projected.meshes[0].style.behind, projected),
               geomsrv::archviz::overlaylayers::Behind::Fade);
    const auto scene = geomsrv::archviz::overlayscene::PrepareScene (
        { std::make_shared<const geomsrv::archviz::overlaylayers::Layer> (projected) }, nullptr);
    ASSERT_FALSE (scene.fills.empty ());
    ASSERT_FALSE (scene.lines.empty ());
    ASSERT_FALSE (scene.fillDraws.empty ());
    for (const auto& fill : scene.fillDraws)
        EXPECT_EQ (fill.behind, geomsrv::archviz::overlayscene::kBehindFade);
    for (const auto& line : scene.lines)
        EXPECT_EQ (line.behind, geomsrv::archviz::overlayscene::kBehindFade);
    const auto& points = projected.meshes[0].points;
    for (size_t i = 0; i < points.size (); i += 3)
        EXPECT_NEAR (points[i + 2], 0.5 * points[i] + 0.2 * points[i + 1] + 0.012, 1e-7);
    for (const auto& line : projected.polylines) {
        EXPECT_FLOAT_EQ (line.widthPixels, 0.7f);
        for (size_t i = 0; i < line.points.size (); i += 3)
            EXPECT_NEAR (line.points[i + 2], 0.5 * line.points[i] + 0.2 * line.points[i + 1] + 0.014, 1e-7);
    }
    for (const auto& line : result.layer.polylines)
        EXPECT_FLOAT_EQ (line.widthPixels, 0.7f);
}

TEST (MassingCollapse, ProjectionKeepsCourtyardClipsToTerrainExtentAndIgnoresUnderside)
{
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Courtyard ()) }, 0, result, error)) << error;
    auto terrain = Box (0, 0, 10, 10, -50, 50);
    geomsrv::archviz::overlaylayers::Layer projected;
    ASSERT_TRUE (zone::Project (result, terrain, projected, error)) << error;
    ASSERT_EQ (projected.meshes.size (), 1u);
    const auto& points = projected.meshes[0].points;
    for (size_t i = 0; i < points.size (); i += 3) {
        EXPECT_GE (points[i], 0);
        EXPECT_LE (points[i], 10);
        EXPECT_GE (points[i + 1], 0);
        EXPECT_LE (points[i + 1], 10);
        EXPECT_NEAR (points[i + 2], 0.012, 1e-8);
    }
    for (const auto& line : projected.polylines) {
        const double x = (line.points[0] + line.points[3]) / 2, y = (line.points[1] + line.points[4]) / 2;
        EXPECT_FALSE (x > 3.51 && x < 6.49 && y > 3.51 && y < 6.49);
    }
    projected.name = "unchanged";
    terrain.triangles[0] = 999999;
    EXPECT_FALSE (zone::Project (result, terrain, projected, error));
    EXPECT_EQ (projected.name, "unchanged");
    EXPECT_FALSE (zone::Project (result, {}, projected, error));
}

TEST (MassingCollapse, ProjectionAcrossTerrainCreasesPreservesUnionAreaAndHatchPhaseAtSurveyCoordinates)
{
    constexpr double ox = 700000, oy = 6000000;
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Box (ox, oy, 10, 10, 100, 3)) }, 100, result, error)) << error;
    geomsrv::Mesh terrain;
    terrain.vertices = { ox - 5, oy - 5, 100, ox + 15, oy - 5, 120, ox + 15, oy + 15, 100, ox - 5, oy + 15, 130 };
    terrain.triangles = { 0, 1, 2, 0, 2, 3 };
    geomsrv::archviz::overlaylayers::Layer projected;
    ASSERT_TRUE (zone::Project (result, terrain, projected, error)) << error;
    ASSERT_EQ (projected.meshes.size (), 1u);
    const auto& points = projected.meshes[0].points;
    double area = 0;
    const auto elevation = [&] (double x, double y) {
        x -= ox;
        y -= oy;
        return 100 + (x >= y ? x - y : 1.5 * (y - x));
    };
    for (size_t i = 0; i < points.size (); i += 9) {
        area += std::abs ((points[i + 3] - points[i]) * (points[i + 7] - points[i + 1]) -
                          (points[i + 4] - points[i + 1]) * (points[i + 6] - points[i])) /
                2;
        for (size_t j = i; j < i + 9; j += 3)
            EXPECT_NEAR (points[j + 2], elevation (points[j], points[j + 1]) + 0.012, 2e-6);
    }
    EXPECT_NEAR (area, result.area, 1e-5) << "no lost or double-filled area at triangle boundaries";
    ASSERT_FALSE (projected.polylines.empty ());
    for (const auto& line : projected.polylines) {
        for (size_t i = 0; i < line.points.size (); i += 3) {
            EXPECT_NEAR (line.points[i + 2], elevation (line.points[i], line.points[i + 1]) + 0.014, 2e-6);
            const double phase = (line.points[i] + line.points[i + 1] - result.hatchOriginSum) / 0.7;
            EXPECT_NEAR (phase, std::round (phase), 2e-8);
        }
    }
}

TEST (MassingCollapse, TopographyHolesAndAbsentMeshCoverageAreNotFilledWithInventedElevation)
{
    zone::Result result;
    std::string error;
    ASSERT_TRUE (zone::Build ({ Input (Box ()) }, 120, result, error)) << error;
    const auto terrain = Courtyard ();
    geomsrv::archviz::overlaylayers::Layer projected;
    ASSERT_TRUE (zone::Project (result, terrain, projected, error)) << error;
    ASSERT_EQ (projected.meshes.size (), 1u);
    const auto& points = projected.meshes[0].points;
    double area = 0;
    for (size_t i = 0; i < points.size (); i += 9) {
        const double x = (points[i] + points[i + 3] + points[i + 6]) / 3;
        const double y = (points[i + 1] + points[i + 4] + points[i + 7]) / 3;
        EXPECT_FALSE (x > 3 && x < 7 && y > 3 && y < 7);
        area += std::abs ((points[i + 3] - points[i]) * (points[i + 7] - points[i + 1]) -
                          (points[i + 4] - points[i + 1]) * (points[i + 6] - points[i])) /
                2;
    }
    EXPECT_NEAR (area, 84, 1e-6);
    ASSERT_TRUE (zone::Project (result, Box (100, 100), projected, error)) << error;
    EXPECT_TRUE (projected.meshes.empty ());
    EXPECT_TRUE (projected.polylines.empty ());
}
