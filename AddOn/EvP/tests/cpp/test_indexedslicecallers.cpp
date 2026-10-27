#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/MassingCollapseZone.hpp"
#include "ArchViz/TerrainProjection.hpp"
#include "Geometry/Primitives.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace av = geomsrv::archviz;
namespace ms = av::massingslices;
namespace layers = av::overlaylayers;
namespace {
av::SliceChain Rect (double x, double y, double width, double height)
{
    return { { x, y, x + width, y, x + width, y + height, x, y + height }, true };
}
void Reverse (av::SliceChain& chain)
{
    for (size_t i = 0; i < chain.Count () / 2; ++i) {
        const size_t j = chain.Count () - i - 1;
        std::swap (chain.xy[2 * i], chain.xy[2 * j]);
        std::swap (chain.xy[2 * i + 1], chain.xy[2 * j + 1]);
    }
}
double FlatArea (const layers::Mesh& mesh, double z, double* signedArea = nullptr)
{
    double area = 0, signedSum = 0;
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        const size_t a = 3 * mesh.indices[i], b = 3 * mesh.indices[i + 1], c = 3 * mesh.indices[i + 2];
        if (mesh.points[a + 2] != z || mesh.points[b + 2] != z || mesh.points[c + 2] != z)
            continue;
        const double cross = (mesh.points[b] - mesh.points[a]) * (mesh.points[c + 1] - mesh.points[a + 1]) -
                             (mesh.points[c] - mesh.points[a]) * (mesh.points[b + 1] - mesh.points[a + 1]);
        area += std::abs (cross) / 2;
        signedSum += cross / 2;
    }
    if (signedArea)
        *signedArea = signedSum;
    return area;
}
double Volume (const layers::Mesh& mesh)
{
    double sum = 0;
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        double p[3][3];
        for (size_t v = 0; v < 3; ++v)
            for (size_t k = 0; k < 3; ++k)
                p[v][k] = mesh.points[3 * mesh.indices[i + v] + k] - mesh.points[k];
        sum += p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) + p[0][1] * (p[1][2] * p[2][0] - p[1][0] * p[2][2]) +
               p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
    }
    return sum / 6;
}
geomsrv::Mesh Ground (double ox, double oy)
{
    geomsrv::Mesh mesh;
    mesh.vertices = { ox - 10, oy - 10, 0, ox + 30, oy - 10, 0, ox + 30, oy + 30, 0, ox - 10, oy + 30, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    return mesh;
}
} // namespace

TEST (IndexedMeasuredSlices, ConcaveCapsRetainAreaVolumeWindingAndSharedIndices)
{
    for (double offset : { 0.0, 600000.0 })
        for (bool reverse : { false, true }) {
            av::SliceChain chain { { 0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4 }, true };
            for (size_t i = 0; i < chain.Count (); ++i) {
                chain.xy[2 * i] += offset;
                chain.xy[2 * i + 1] += offset * 10;
            }
            if (reverse)
                Reverse (chain);
            layers::Mesh mesh;
            std::string error;
            ASSERT_TRUE (ms::ExtrudeChains ({ chain }, 2, 5, mesh, error)) << error;
            double sign = 0;
            EXPECT_NEAR (FlatArea (mesh, 5, &sign), 7, 1e-9);
            EXPECT_NEAR (sign, 7, 1e-9);
            EXPECT_NEAR (FlatArea (mesh, 2, &sign), 7, 1e-9);
            EXPECT_NEAR (sign, -7, 1e-9);
            EXPECT_NEAR (Volume (mesh), 21, 1e-9);
            EXPECT_LT (mesh.points.size () / 3, mesh.indices.size ());
            layers::Mesh open;
            ASSERT_TRUE (ms::ExtrudeChains ({ chain }, 2, 5, open, error, false));
            EXPECT_EQ (FlatArea (open, 5), 0);
            EXPECT_NEAR (FlatArea (open, 2), 7, 1e-9);
        }
}

TEST (IndexedMeasuredSlices, LargeSpanThinCapsDoNotRoundDoubleCornersThroughFloat)
{
    const double width = 1000000.03125, expected = width * 0.125;
    layers::Mesh mesh;
    std::string error;
    ASSERT_TRUE (ms::ExtrudeChains ({ Rect (600000, 6500000, width, 0.125) }, 1, 4, mesh, error)) << error;
    EXPECT_NEAR (FlatArea (mesh, 4), expected, 1e-8);
    EXPECT_NEAR (Volume (mesh), expected * 3, 1e-7);
    EXPECT_NEAR (av::UnionArea ({ Rect (600000, 6500000, width, 0.125) }), expected, 1e-8);
}

TEST (IndexedMeasuredSlices, InvalidCapsRefuseAtomicallyAndAnEmptyLeadingContourCannotBeDereferenced)
{
    layers::Mesh mesh;
    mesh.points = { 1, 2, 3 };
    mesh.indices = { 0 };
    const auto before = mesh;
    std::string error;
    auto bad = Rect (0, 0, 1, 1);
    bad.xy.push_back (1);
    EXPECT_FALSE (ms::ExtrudeChains ({ bad }, 0, 3, mesh, error));
    EXPECT_FALSE (ms::ExtrudeChains ({ Rect (0, 0, 1, 1) }, 0, std::numeric_limits<double>::quiet_NaN (), mesh, error));
    EXPECT_EQ (mesh.points, before.points);
    EXPECT_EQ (mesh.indices, before.indices);
    mesh = {};
    ASSERT_TRUE (ms::ExtrudeChains ({ {}, Rect (0, 0, 1, 1) }, 0, 3, mesh, error)) << error;
    EXPECT_NEAR (FlatArea (mesh, 3), 1, 1e-9);
}

TEST (IndexedMeasuredSlices, CoverageKeepsHoleAreaIndependentRegionsStyleAndDatum)
{
    ms::Result result;
    result.hasCoverage = true;
    ms::CoveragePatch patch;
    patch.hasElevation = true;
    patch.z = 123.456789;
    patch.built = { Rect (600000, 6500000, 10, 10), Rect (600004, 6500004, 2, 2), Rect (600020, 6500000, 2, 2) };
    result.coverage.push_back (patch);
    layers::Layer layer;
    std::string error;
    ASSERT_TRUE (ms::Highlight (result, ms::kBuiltHover, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 1u);
    const auto& mesh = layer.meshes[0];
    EXPECT_EQ (mesh.points.size () / 3, 12u);
    EXPECT_LT (mesh.points.size () / 3, mesh.indices.size ());
    EXPECT_NEAR (FlatArea (mesh, patch.z + 0.012), 100, 1e-8);
    EXPECT_EQ (mesh.rgba, 0x9AA0A6FF);
    EXPECT_FLOAT_EQ (mesh.style.opacity, 0.65f);
    for (size_t i = 2; i < mesh.points.size (); i += 3)
        EXPECT_DOUBLE_EQ (mesh.points[i], patch.z + 0.012);
}

TEST (IndexedMeasuredSlices, TerrainProjectionPreservesAreaSlopeHatchLengthAndPhase)
{
    constexpr double ox = 600000, oy = 6500000;
    std::vector<av::SliceChain> chains { Rect (ox, oy, 10, 10), Rect (ox + 4, oy + 4, 2, 2) };
    Reverse (chains[1]);
    auto terrain = Ground (ox, oy);
    for (size_t i = 0; i < terrain.vertices.size (); i += 3)
        terrain.vertices[i + 2] = 120 + 0.25 * (terrain.vertices[i] - ox) + 0.5 * (terrain.vertices[i + 1] - oy);
    av::terrainprojection::Style style;
    style.name = "indexed-terrain-test";
    style.fillRgba = 0xAA446528;
    layers::Layer projected, plan;
    std::string error;
    ASSERT_TRUE (av::terrainprojection::Project (chains, ox + oy, terrain, style, projected, error)) << error;
    ASSERT_TRUE (av::terrainprojection::HatchPlan (chains, ox + oy, 120, style, plan, error)) << error;
    ASSERT_EQ (projected.meshes.size (), 1u);
    const auto& mesh = projected.meshes[0];
    av::StorySliceFillMesh flat { mesh.points, mesh.indices };
    double area = 0;
    ASSERT_TRUE (av::MeasureSliceFill (flat, area, error)) << error;
    EXPECT_NEAR (area, 96, 2e-6);
    EXPECT_LT (mesh.points.size () / 3, mesh.indices.size ());
    for (size_t i = 0; i < mesh.points.size (); i += 3)
        EXPECT_NEAR (mesh.points[i + 2], 120 + 0.25 * (mesh.points[i] - ox) + 0.5 * (mesh.points[i + 1] - oy) + 0.012,
                     1e-8);
    const auto length = [] (const auto& layer) {
        double total = 0;
        for (const auto& line : layer.polylines) {
            const auto& p = line.points;
            total += std::hypot (p[3] - p[0], p[4] - p[1]);
        }
        return total;
    };
    EXPECT_NEAR (length (projected), length (plan), 2e-5);
    for (const auto& line : projected.polylines)
        for (size_t i = 0; i < line.points.size (); i += 3) {
            const double phase = (line.points[i] + line.points[i + 1] - ox - oy) / style.spacing;
            EXPECT_NEAR (phase, std::round (phase), 2e-6);
        }
}

TEST (IndexedMeasuredSlices, CollapseFillMatchesItsMeasuredUnionAtSurveyCoordinates)
{
    for (double offset : { 0.0, 600000.0 }) {
        ms::Input input;
        geomsrv::Mesh body;
        std::string error;
        ASSERT_TRUE (geomsrv::engine::MakeBox ({ offset + 5, offset * 10 + 5, 1.5 }, 10, 10, 3, body, error));
        input.body = std::make_shared<const geomsrv::Mesh> (std::move (body));
        av::massingcollapse::Result result;
        ASSERT_TRUE (av::massingcollapse::Build ({ input }, Ground (offset, offset * 10), 42, result, error)) << error;
        ASSERT_EQ (result.layer.meshes.size (), 1u);
        EXPECT_NEAR (FlatArea (result.layer.meshes[0], 42.01), result.area, 2e-5);
        EXPECT_LT (result.layer.meshes[0].points.size () / 3, result.layer.meshes[0].indices.size ());
    }
}

TEST (IndexedMeasuredSlices, MixedFacadePrismsAndBodiesRetainMeasuredPerimeter)
{
    for (double offset : { 0.0, 600000.0 }) {
        const double width = 10 + std::ldexp (1.0, -23);
        ms::Input prism, operated;
        prism.slab.outer.xy = Rect (offset, offset * 10, width, 10).xy;
        prism.slab.bottom = 0;
        prism.slab.top = 3;
        geomsrv::Mesh body;
        std::string error;
        ASSERT_TRUE (geomsrv::engine::MakeBox ({ offset + 22, offset * 10 + 2, 1.5 }, 4, 4, 3, body, error));
        operated.body = std::make_shared<const geomsrv::Mesh> (std::move (body));
        double area = -1;
        ASSERT_TRUE (ms::Facade ({ prism, operated }, area, error)) << error;
        EXPECT_NEAR (area, 2 * (width + 10) * 3 + 48, 2e-6);
    }
}
