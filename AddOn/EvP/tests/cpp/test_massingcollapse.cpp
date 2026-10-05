#include "ArchViz/MassingCollapseZone.hpp"
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
