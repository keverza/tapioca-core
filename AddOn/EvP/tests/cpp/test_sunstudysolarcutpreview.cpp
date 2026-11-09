#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <map>
#include <tuple>

using namespace geomsrv::archviz;

namespace {

double FillArea (const GhPreviewBucket& bucket)
{
    double area = 0.0;
    for (size_t i = 0; i + 2 < bucket.meshIndices.size (); i += 3) {
        const auto& a = bucket.meshVertices[bucket.meshIndices[i]];
        const auto& b = bucket.meshVertices[bucket.meshIndices[i + 1]];
        const auto& c = bucket.meshVertices[bucket.meshIndices[i + 2]];
        const double u[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, v[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
        const double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        area += 0.5 * std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    return area;
}

evp::sunstudy::SolarCutTriangle Tri (double ax, double ay, double bx, double by, double cx, double cy)
{
    evp::sunstudy::SolarCutTriangle t {};
    const double p[3][2] = { { ax, ay }, { bx, by }, { cx, cy } };
    for (int c = 0; c < 3; ++c) {
        t.v[c][0] = p[c][0];
        t.v[c][1] = p[c][1];
        t.v[c][2] = 0.0;
    }
    return t;
}

bool OnLine (const std::array<double, 3>& a, const std::array<double, 3>& b, const std::array<double, 6>& line,
             double tolerance)
{
    const auto subtract = [] (const std::array<double, 3>& u, const std::array<double, 3>& v) {
        return std::array<double, 3> { u[0] - v[0], u[1] - v[1], u[2] - v[2] };
    };
    const auto cross = [] (const std::array<double, 3>& u, const std::array<double, 3>& v) {
        return std::array<double, 3> { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                       u[0] * v[1] - u[1] * v[0] };
    };
    const auto length = [] (const std::array<double, 3>& u) {
        return std::sqrt (u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    };
    const std::array<double, 3> origin { line[0], line[1], line[2] }, end { line[3], line[4], line[5] };
    const auto direction = subtract (end, origin), edge = subtract (b, a);
    const double directionLength = length (direction), edgeLength = length (edge);
    if (directionLength < 1.0e-12 || edgeLength < 1.0e-12)
        return false;
    const auto parallel = cross (direction, edge);
    if (length (parallel) > 1.0e-8 * directionLength * edgeLength)
        return false;
    return length (cross (subtract (a, origin), direction)) <= tolerance * directionLength;
}

} // namespace

TEST (SunStudySolarCutPreview, WonCellsBecomeOneRegionWithItsPerimeter)
{
    // A 4 x 4 block of floor cells, 0.5 m apart, with one cell missing inside.
    std::vector<SolarCutWonCell> cells;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (i == 1 && j == 2)
                continue;
            SolarCutWonCell cell;
            cell.position[0] = 10.0 + 0.5 * i;
            cell.position[1] = -3.0 + 0.5 * j;
            cells.push_back (cell);
        }
    GhPreviewDrawables drawables;
    AppendSolarCutWonCells (cells, 0.5, drawables);
    // The hole is filled; the shape runs through the cells' outer edges with
    // its four corners chamfered: (16 - 4 * 1/8) cells of 0.25 m2.
    EXPECT_NEAR (FillArea (drawables.depthTested), 15.5 * 0.25, 1.0e-4);
    // Its perimeter only: three segments a side and four chamfers.
    EXPECT_EQ (drawables.depthTested.lineVertices.size () / 6, 16u);
}

TEST (SunStudySolarCutPreview, OutlineSkipsDiagonalsAndTJunctions)
{
    // One square cut face of three triangles, one diagonal split at (1,1).
    evp::sunstudy::SolarCutResult cut;
    cut.caps = { Tri (0, 0, 2, 0, 2, 2), Tri (0, 0, 1, 1, 0, 2), Tri (1, 1, 2, 2, 0, 2) };
    const double point[3] = { 0.0, 0.0, 0.0 };
    GhPreviewDrawables drawables;
    BuildSolarCutGeometry (cut, point, drawables);
    EXPECT_EQ (drawables.xray.lineVertices.size () / 6, 4u);  // the square's four sides
    EXPECT_FALSE (drawables.xray.meshIndices.empty ());       // the cut face, seen through
    EXPECT_TRUE (drawables.depthTested.meshIndices.empty ()); // no sheet/solid can occlude the won overlay
}

TEST (SunStudySolarCutPreview, CuttingSheetOmitsSectorCreasesAndTriangulationDiagonals)
{
    // Two non-coplanar triangles share the fan's diagonal. It is a sector
    // crease, not another edge of the cutting sheet, and must not be outlined.
    evp::sunstudy::SolarCutResult cut;
    cut.envelope = { Tri (0, 0, 2, 0, 2, 2), Tri (0, 0, 2, 2, 0, 2) };
    cut.envelope[1].v[2][2] = 1.0;
    const double point[3] = { 0.0, 0.0, 0.0 };
    GhPreviewDrawables drawables;
    BuildSolarCutGeometry (cut, point, drawables);
    EXPECT_EQ (drawables.xray.lineVertices.size () / 6, 4u);
}

TEST (SunStudySolarCutPreview, EnlargingTheSweptSheetKeepsOneClosedExteriorBoundary)
{
    evp::sunstudy::SolarCutInput input;
    for (int i = 0; i < 5; ++i) {
        evp::sunstudy::SunStep sun;
        const double a = (-30.0 + i * 15.0) * 3.14159265358979323846 / 180.0;
        const double altitude = (30.0 + i * 4.0) * 3.14159265358979323846 / 180.0;
        sun.direction[0] = std::cos (altitude) * std::cos (a);
        sun.direction[1] = std::cos (altitude) * std::sin (a);
        sun.direction[2] = std::sin (altitude);
        input.steps.push_back (sun);
    }
    for (const double radius : { 0.0, 1.0, 4.0, 10.0 }) {
        input.radius = radius;
        evp::sunstudy::SolarCutResult cut;
        ASSERT_TRUE (evp::sunstudy::ComputeSolarCutSurface (input, cut, {}, 100.0));
        ASSERT_FALSE (cut.envelopeEdges.empty ());
        std::map<std::tuple<long long, long long, long long>, int> degree;
        size_t longEdges = 0;
        for (const auto& edge : cut.envelopeEdges) {
            double length2 = 0.0;
            for (int k = 0; k < 3; ++k)
                length2 += (edge[k] - edge[k + 3]) * (edge[k] - edge[k + 3]);
            longEdges += length2 > 2500.0 ? 1 : 0;
            for (const int at : { 0, 3 })
                ++degree[{ std::llround (edge[at] * 1e6), std::llround (edge[at + 1] * 1e6),
                           std::llround (edge[at + 2] * 1e6) }];
        }
        EXPECT_EQ (longEdges, 2u) << radius; // only the two outer rays, never an interior radial seam
        for (const auto& [point, count] : degree)
            EXPECT_EQ (count, 2) << radius;
        GhPreviewDrawables drawables;
        BuildSolarCutGeometry (cut, input.point, drawables);
        EXPECT_EQ (drawables.xray.lineVertices.size () / 6, cut.envelopeEdges.size ());
        EXPECT_TRUE (drawables.depthTested.meshIndices.empty ());
        for (const auto& vertex : drawables.xray.meshVertices)
            EXPECT_LE (vertex.rgba & 255u, 20u); // the sheet is less than 8% opaque
    }
}

TEST (SunStudySolarCutPreview, CutFacesDoNotRestoreHiddenCutterSeams)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("tower", 103, -48, 12, 4, 16, 30) };
    evp::sunstudy::SolarCutInput input;
    input.point[0] = 100.0;
    input.point[1] = -40.0;
    input.point[2] = 12.0;
    for (int i = 0; i < 5; ++i) {
        evp::sunstudy::SunStep sun;
        const double a = (-40.0 + i * 20.0) * 3.14159265358979323846 / 180.0;
        const double altitude = (25.0 + (2 - std::abs (2 - i)) * 10.0) * 3.14159265358979323846 / 180.0;
        sun.direction[0] = std::cos (altitude) * std::cos (a);
        sun.direction[1] = std::cos (altitude) * std::sin (a);
        sun.direction[2] = std::sin (altitude);
        input.steps.push_back (sun);
    }
    input.blocks = { 1 };
    for (const double radius : { 0.0, 1.0, 4.0 }) {
        input.radius = radius;
        evp::sunstudy::SolarCutResult cut;
        ASSERT_TRUE (evp::sunstudy::ComputeSolarCut (snapshot, input, cut)) << cut.error;
        ASSERT_EQ (cut.envelopeHiddenEdges.size (), 2 * (cut.sectors - 1)) << radius;
        size_t capSeams = 0;
        for (const auto& triangle : cut.caps)
            for (int edge = 0; edge < 3; ++edge) {
                const auto& a = triangle.v[edge];
                const auto& b = triangle.v[(edge + 1) % 3];
                for (const auto& hidden : cut.envelopeHiddenEdges)
                    if (OnLine ({ a[0], a[1], a[2] }, { b[0], b[1], b[2] }, hidden, 1.0e-6)) {
                        ++capSeams;
                        break;
                    }
            }
        ASSERT_GT (capSeams, 0u) << radius; // the fixture really transfers cutter seams to its caps
        GhPreviewDrawables drawables;
        BuildSolarCutGeometry (cut, input.point, drawables);
        size_t solidEdges = 0, transferred = 0;
        for (size_t i = 0; i < drawables.xray.lineVertices.size (); i += 6) {
            const auto& line = drawables.xray.lineVertices[i];
            if (line.rgba != kSolarCutEdgeRgba)
                continue;
            ++solidEdges;
            const std::array<double, 3> a { line.x, line.y, line.z }, b { line.ox, line.oy, line.oz };
            for (const auto& hidden : cut.envelopeHiddenEdges)
                if (OnLine (a, b, hidden, 1.0e-4)) {
                    ++transferred;
                    break;
                }
        }
        EXPECT_GT (solidEdges, 0u) << radius; // real cut boundaries stay
        EXPECT_EQ (transferred, 0u) << radius;
    }
}

TEST (SunStudySolarCutPreview, WonExposureDrawsLastAsAnXrayWithNoDepthWritingGeometry)
{
    evp::sunstudy::SolarCutResult cut;
    cut.envelope = { Tri (0, 0, 2, 0, 0, 2) };
    cut.removed = cut.envelope;
    cut.caps = cut.envelope;
    GhPreviewDrawables drawables;
    const double point[3] = {};
    BuildSolarCutGeometry (cut, point, drawables);
    evp::sunstudy::SolarExposureResult exposure;
    exposure.triangles = { Tri (0, 0, 2, 0, 0, 2) };
    exposure.boundary = { { 0, 0, 0, 2, 0, 0 } };
    AppendSolarExposure (exposure, drawables);
    ASSERT_FALSE (drawables.xray.meshVertices.empty ());
    EXPECT_EQ (drawables.xray.meshVertices.back ().rgba, kSolarCutWonRgba);
    EXPECT_EQ (drawables.xray.lineVertices.back ().rgba, kSolarCutWonEdgeRgba);
    EXPECT_TRUE (drawables.depthTested.meshIndices.empty ());
    EXPECT_LT (kSolarCutRemovedRgba & 255u, 0x40u);
    EXPECT_LT (kSolarCutCapRgba >> 24, kSolarCutRemovedRgba >> 24);
}

TEST (SunStudySolarCutPreview, WonRegionUsesTheSingleShadowTriangleLatticeAndRealBoundary)
{
    geomsrv::Snapshot snapshot;
    geomsrv::Mesh mesh;
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 0, 4, 0 };
    mesh.triangles = { 0, 1, 2 };
    snapshot.meshes = { mesh };
    evp::sunstudy::SamplerOptions options;
    options.spacing = 1.0;
    options.normalOffset = 0.0;
    options.wantLayouts = true;
    const auto grid = evp::sunstudy::BuildSampleGrid (mesh.vertices.data (), mesh.VertexCount (),
                                                      mesh.triangles.data (), mesh.TriangleCount (), nullptr, options);
    ASSERT_TRUE (grid.valid);
    ASSERT_GT (grid.Count (), 8u);
    std::vector<SolarCutWonCell> surface (grid.Count ());
    for (size_t sample = 0; sample < surface.size (); ++sample)
        surface[sample].sample = sample;
    const auto& won = surface;

    GhPreviewDrawables drawables;
    const SolarCutWonStudy study { &snapshot, &grid, nullptr, false };
    AppendSolarCutWonCells (won, 1.0, drawables, &surface, &study);
    EXPECT_NEAR (FillArea (drawables.depthTested), 8.0, 1.0e-5); // the real triangle, not its raster overhang
    EXPECT_EQ (drawables.depthTested.lineVertices.size () / 6, 3u);
    for (const auto& vertex : drawables.depthTested.meshVertices)
        EXPECT_DOUBLE_EQ (vertex.z, 0.0); // no invented plane lift; the renderer applies its depth pull
}

TEST (SunStudySolarCutPreview, WonPatchRegionClipsToItsSquareWithoutInternalEdges)
{
    geomsrv::Snapshot snapshot;
    geomsrv::Mesh mesh;
    mesh.vertices = { 0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snapshot.meshes = { mesh };
    evp::sunstudy::PatchSamplerOptions options;
    options.spacing = 1.0;
    options.normalOffset = 0.0;
    const auto grid =
        evp::sunstudy::BuildPatchSampleGrid (mesh.vertices.data (), mesh.VertexCount (), mesh.triangles.data (),
                                             mesh.TriangleCount (), nullptr, { "floor" }, options);
    ASSERT_TRUE (grid.valid);
    ASSERT_GT (grid.Count (), 3u);
    std::vector<SolarCutWonCell> surface (grid.Count ());
    for (size_t sample = 0; sample < surface.size (); ++sample)
        surface[sample].sample = sample;
    auto won = surface;
    won.erase (won.begin () + grid.Count () / 2);

    GhPreviewDrawables drawables;
    const SolarCutWonStudy study { &snapshot, nullptr, &grid, true };
    AppendSolarCutWonCells (won, 1.0, drawables, &surface, &study);
    EXPECT_NEAR (FillArea (drawables.depthTested), 16.0, 1.0e-5);
    EXPECT_EQ (drawables.depthTested.lineVertices.size () / 6, 4u);
}

TEST (SunStudySolarCutPreview, WonRegionIsTrimmedToItsSurface)
{
    // Two 3 x 3 blocks of won cells one empty column apart. Alone, closing
    // joins them across the gap; given the study's cells -- no cell in that
    // column, the surface has an edge there -- they stay two regions.
    std::vector<SolarCutWonCell> cells;
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 3; ++j) {
            if (i == 3)
                continue;
            SolarCutWonCell cell;
            cell.position[0] = 0.5 * i;
            cell.position[1] = 0.5 * j;
            cells.push_back (cell);
        }
    GhPreviewDrawables joined, trimmed;
    AppendSolarCutWonCells (cells, 0.5, joined);
    AppendSolarCutWonCells (cells, 0.5, trimmed, &cells);
    EXPECT_GT (FillArea (joined.depthTested), FillArea (trimmed.depthTested) + 0.2);
    EXPECT_NEAR (FillArea (trimmed.depthTested), 2.0 * 8.5 * 0.25, 1.0e-4); // each block, corners chamfered
}
