#include "ArchViz/MassingHeadroom.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "Geometry/Primitives.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace hr = geomsrv::archviz::massingheadroom;
namespace ms = geomsrv::archviz::massingslices;
namespace layers = geomsrv::archviz::overlaylayers;
namespace meta = geomsrv::metadata;
using geomsrv::archviz::SliceChain;
namespace {
SliceChain Ring (double x0 = 0, double y0 = 0, double x1 = 10, double y1 = 10)
{
    return { { x0, y0, x1, y0, x1, y1, x0, y1 }, true };
}
geomsrv::Mesh Roof (double low = 0.8, double high = 3.2, double ox = 0, double oy = 0, double z = 0)
{
    geomsrv::Mesh mesh;
    std::string error;
    EXPECT_TRUE (geomsrv::engine::MakeBox ({ ox + 5, oy + 5, z + 2 }, 10, 10, 4, mesh, error));
    for (size_t i = 0; i < mesh.vertices.size (); i += 3)
        if (mesh.vertices[i + 2] > z + 1)
            mesh.vertices[i + 2] = z + low + (high - low) * (mesh.vertices[i] - ox) / 10;
    return mesh;
}
ms::Input Slab (double top = 3.2, double ox = 0, double oy = 0, double z = 0)
{
    ms::Input input;
    input.slab.guid = "slab";
    input.slab.bottom = z;
    input.slab.top = z + top;
    input.slab.outer.xy = Ring (ox, oy, ox + 10, oy + 10).xy;
    meta::Property id;
    id.key = "massing.buildingId";
    id.value = meta::Value::Text ("Tower");
    meta::SetProperty (input.metadata, id);
    return input;
}
geomsrv::archviz::massingcalculation::Result Envelope (const std::vector<geomsrv::Mesh>& bodies)
{
    geomsrv::archviz::massingcalculation::Result envelope;
    envelope.hasEnvelope = true;
    for (const auto& body : bodies) {
        layers::Mesh mesh;
        mesh.points = body.vertices;
        mesh.indices = body.triangles;
        envelope.layer.meshes.push_back (std::move (mesh));
    }
    return envelope;
}
double Area (const std::vector<SliceChain>& chains)
{
    double area = 0;
    for (const auto& chain : chains) {
        const double ox = chain.xy[0], oy = chain.xy[1];
        for (size_t i = 0, j = chain.Count () - 1; i < chain.Count (); j = i++)
            area += ((chain.xy[j * 2] - ox) * (chain.xy[i * 2 + 1] - oy) -
                     (chain.xy[i * 2] - ox) * (chain.xy[j * 2 + 1] - oy)) /
                    2;
    }
    return std::abs (area);
}
} // namespace

TEST (MassingHeadroom, SlopedRoofSplitsExactlyAtTheVerticalClearanceBoundary)
{
    const auto roof = Roof ();
    hr::Split split;
    std::string error;
    size_t work = 0;
    ASSERT_TRUE (hr::Partition ({ Ring () }, 0, roof.vertices, roof.triangles, split, work, error)) << error;
    EXPECT_NEAR (split.countedArea, 200.0 / 3, 2e-5);
    EXPECT_NEAR (split.excludedArea, 100.0 / 3, 2e-5);
    EXPECT_NEAR (Area (split.counted) + Area (split.excluded), 100, 1e-8);
    for (const auto& chain : split.excluded)
        for (size_t i = 0; i < chain.Count (); ++i)
            EXPECT_LE (chain.xy[i * 2], 10.0 / 3 + 1e-6);
}

TEST (MassingHeadroom, ExactlyOnePointSixIsCountedButLowerFlatRoofIsExcluded)
{
    for (double height : { 1.5999, 1.6, 1.6001 }) {
        const auto roof = Roof (height, height);
        hr::Split split;
        size_t work = 0;
        std::string error;
        ASSERT_TRUE (hr::Partition ({ Ring () }, 0, roof.vertices, roof.triangles, split, work, error)) << error;
        EXPECT_NEAR (split.countedArea, height >= 1.6 ? 100 : 0, 1e-7);
        EXPECT_NEAR (split.excludedArea, height < 1.6 ? 100 : 0, 1e-7);
        ms::Result result;
        ASSERT_TRUE (ms::Build ({ Slab (height) }, {}, nullptr, result, error));
        EXPECT_NEAR (result.rawArea, split.countedArea, 1e-7);
        EXPECT_NEAR (result.excludedArea, split.excludedArea, 1e-7);
        EXPECT_NEAR (result.rawVolume, 100 * height, 1e-7);
    }
}

TEST (MassingHeadroom, HolesAndDisconnectedFloorRegionsRemainHolesNotGrayFill)
{
    const auto roof = Roof ();
    hr::Split split;
    size_t work = 0;
    std::string error;
    ASSERT_TRUE (hr::Partition ({ Ring (), Ring (1, 2, 2, 8) }, 0, roof.vertices, roof.triangles, split, work, error));
    EXPECT_NEAR (split.countedArea, 200.0 / 3, 2e-5);
    EXPECT_NEAR (split.excludedArea, 100.0 / 3 - 6, 2e-5);
    ASSERT_TRUE (hr::Partition ({ Ring (0, 0, 2, 10), Ring (5, 0, 10, 10) }, 0, roof.vertices, roof.triangles, split,
                                work, error));
    EXPECT_NEAR (split.countedArea, 50, 1e-6);
    EXPECT_NEAR (split.excludedArea, 20, 1e-6);
}

TEST (MassingHeadroom, SurveyCoordinatesAndElevatedFloorDoNotFlattenOrShiftTheRoofBoundary)
{
    constexpr double ox = 700000, oy = 6000000, z = 1000000;
    const auto roof = Roof (0.8, 3.2, ox, oy, z);
    hr::Split split;
    size_t work = 0;
    std::string error;
    ASSERT_TRUE (
        hr::Partition ({ Ring (ox, oy, ox + 10, oy + 10) }, z, roof.vertices, roof.triangles, split, work, error))
        << error;
    EXPECT_NEAR (split.countedArea, 200.0 / 3, 2e-5);
    EXPECT_NEAR (split.excludedArea, 100.0 / 3, 2e-5);
}

TEST (MassingHeadroom, HigherDisconnectedSolidDoesNotHideTheNearestLowExitCeiling)
{
    auto lower = Roof (1, 1);
    geomsrv::Mesh upper;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 5, 5, 2.2 }, 10, 10, 2, upper, error)); // begins at 1.2
    const auto offset = uint32_t (lower.vertices.size () / 3);
    lower.vertices.insert (lower.vertices.end (), upper.vertices.begin (), upper.vertices.end ());
    for (uint32_t index : upper.triangles)
        lower.triangles.push_back (index + offset);
    hr::Split split;
    size_t work = 0;
    ASSERT_TRUE (hr::Partition ({ Ring () }, 0, lower.vertices, lower.triangles, split, work, error)) << error;
    EXPECT_NEAR (split.countedArea, 0, 1e-7);
    EXPECT_NEAR (split.excludedArea, 100, 1e-7);
}

TEST (MassingHeadroom, SlabBodyAndEnvelopeEachLimitTheCountedAreaAndGrayHasNoFunctionColour)
{
    auto input = Slab ();
    input.body = std::make_shared<const geomsrv::Mesh> (Roof ());
    auto envelope = Envelope ({ Roof (3.2, 0.8) });
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 1u);
    EXPECT_NEAR (result.rawArea, 200.0 / 3, 2e-5);
    EXPECT_NEAR (result.allowedArea, 100.0 / 3, 2e-5);
    EXPECT_NEAR (result.excludedArea, 200.0 / 3, 2e-5);
    EXPECT_NEAR (geomsrv::archviz::hudsection::TotalArea (result.section), result.allowedArea, 1e-8);
    EXPECT_NEAR (result.rawVolume, 320, 1e-7);
    EXPECT_NEAR (result.allowedVolume, 320, 1e-7);
    ASSERT_EQ (result.layer.texts.size (), 1u);
    EXPECT_EQ (result.layer.texts[0].text, "33.3 m\xC2\xB2");
    const auto gray = std::find_if (result.layer.meshes.begin (), result.layer.meshes.end (),
                                    [] (const auto& mesh) { return mesh.rgba == 0xD9DDE280; });
    ASSERT_NE (gray, result.layer.meshes.end ());
    EXPECT_TRUE (gray->graphicsFunction.empty ());
    const auto usages = ms::UsageMix (result);
    ASSERT_EQ (usages.size (), 1u);
    EXPECT_NEAR (usages[0].area, result.allowedArea, 1e-8);
    EXPECT_NEAR (usages[0].volume, 320, 1e-8);
    layers::Layer picked;
    ASSERT_TRUE (ms::FloorHighlight (result, "building:Tower", { 0, 0 }, picked, error));
    ASSERT_EQ (picked.meshes.size (), 1u);
    for (size_t i = 0; i < picked.meshes[0].points.size (); i += 3) {
        EXPECT_GE (picked.meshes[0].points[i], 10.0 / 3 - 1e-6);
        EXPECT_LE (picked.meshes[0].points[i], 20.0 / 3 + 1e-6);
    }
}

TEST (MassingHeadroom, OverlappingParcelShellsUseTheHigherAvailableRoofNotTheLowShell)
{
    auto input = Slab (4);
    auto envelope = Envelope ({ Roof (), Roof (4, 4) });
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
    EXPECT_NEAR (result.allowedArea, 100, 1e-7);
    EXPECT_NEAR (result.excludedArea, 0, 1e-7);
    EXPECT_TRUE (result.rows[0].lowChains.empty ());
}

TEST (MassingHeadroom, GrayAreasRemainPhysicalParcelCoverageAndDoNotQualifyForLargeGrossMarks)
{
    auto input = Slab (1);
    input.slab.outer.xy = Ring (0, 0, 80, 10).xy;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error));
    EXPECT_NEAR (result.rawArea, 0, 1e-7);
    EXPECT_NEAR (result.excludedArea, 800, 1e-7);
    geomsrv::archviz::massingcalculation::Preview preview;
    preview.inputs.before.known = true;
    preview.inputs.before.guid = "parcel";
    preview.inputs.before.edges = { { 0, 0, 100, 0 }, { 100, 0, 100, 10 }, { 100, 10, 0, 10 }, { 0, 10, 0, 0 } };
    ASSERT_TRUE (ms::Coverage (result, preview, error)) << error;
    EXPECT_NEAR (result.builtArea, 800, 1e-7);
    EXPECT_NEAR (result.unbuiltArea, 200, 1e-7);
    layers::Layer large;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, { { "slab", "Tower" } }, {}, large, error));
    EXPECT_TRUE (large.meshes.empty ());
}

TEST (MassingHeadroom, HighlightIsHatchedGrayAndDoesNotChangeTotalsOrSourceGraphics)
{
    auto input = Slab ();
    input.body = std::make_shared<const geomsrv::Mesh> (Roof ());
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error));
    const auto colour = result.layer.meshes[0].rgba;
    const double area = result.rawArea;
    layers::Layer highlight;
    ASSERT_TRUE (ms::LowHeadroomHighlight (result, highlight, error)) << error;
    EXPECT_EQ (highlight.name, ms::kLowHeadroomLayer);
    EXPECT_EQ (highlight.views, layers::Views::Both);
    ASSERT_FALSE (highlight.meshes.empty ());
    EXPECT_EQ (highlight.meshes[0].rgba, 0xEDF0F4D9);
    EXPECT_TRUE (highlight.meshes[0].style.hatched);
    EXPECT_TRUE (highlight.texts.empty ());
    EXPECT_NEAR (highlight.meshes[0].points[2], 0.02, 1e-8);
    EXPECT_EQ (result.rawArea, area);
    EXPECT_EQ (result.layer.meshes[0].rgba, colour);
    ASSERT_TRUE (ms::LowHeadroomHighlight ({}, highlight, error));
    EXPECT_TRUE (highlight.meshes.empty ());
}

TEST (MassingHeadroom, GrayFloorsRetainStoryIdentityHeightEditorAndRangeMetadataTargets)
{
    auto input = Slab (4);
    meta::Property mode;
    mode.key = "massing.heightMode";
    mode.value = meta::Value::Option ("archicad");
    meta::SetProperty (input.metadata, mode);
    geomsrv::archviz::ProjectStoreys storeys;
    storeys.levels = { 0, 3, 6 };
    storeys.indices = { 0, 1, 2 };
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, storeys, nullptr, result, error));
    ASSERT_EQ (result.section.floors.size (), 2u);
    EXPECT_NEAR (result.section.floors[1].areaM2, 0, 1e-7);
    EXPECT_EQ (result.rows[1].story, 1);
    EXPECT_EQ (result.heightControls.size (), 1u);
    const auto edits = geomsrv::archviz::hudsection::RunEdits (result.section, { 1, 1 }, "commercial", false);
    ASSERT_EQ (edits.size (), 1u);
    EXPECT_EQ (edits[0].element, "slab");
    EXPECT_EQ (edits[0].from, 1);
    EXPECT_EQ (edits[0].to, 1);
}

TEST (MassingHeadroom, InvalidOpenBodiesMissingRoofCoverageAndWorkOverflowRefuseAtomically)
{
    auto body = Roof ();
    hr::Split split;
    split.countedArea = -1;
    size_t work = 0;
    std::string error;
    body.triangles.pop_back ();
    EXPECT_FALSE (hr::Partition ({ Ring () }, 0, body.vertices, body.triangles, split, work, error));
    EXPECT_EQ (split.countedArea, -1);
    body = Roof ();
    body.triangles.resize (body.triangles.size () - 3);
    EXPECT_FALSE (hr::Partition ({ Ring () }, 0, body.vertices, body.triangles, split, work, error));
    body = Roof ();
    body.vertices[0] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (hr::Partition ({ Ring () }, 0, body.vertices, body.triangles, split, work, error));
    body = Roof ();
    EXPECT_FALSE (hr::Partition ({ Ring (0, 0, 20, 10) }, 0, body.vertices, body.triangles, split, work, error));
    for (size_t i = 0; i < body.triangles.size (); i += 3)
        std::swap (body.triangles[i + 1], body.triangles[i + 2]);
    EXPECT_FALSE (hr::Partition ({ Ring () }, 0, body.vertices, body.triangles, split, work, error));
    body = Roof ();
    work = 2000000;
    EXPECT_FALSE (hr::Partition ({ Ring () }, 0, body.vertices, body.triangles, split, work, error));
    EXPECT_EQ (split.countedArea, -1);
}

TEST (MassingHeadroom, RoofSlopeInBothDirectionsIsClippedAsATriangleNotAMaximumHeightBand)
{
    auto roof = Roof ();
    for (size_t i = 0; i < roof.vertices.size (); i += 3)
        if (roof.vertices[i + 2] > 0.1)
            roof.vertices[i + 2] = 0.8 + 0.12 * (roof.vertices[i] + roof.vertices[i + 1]);
    hr::Split split;
    size_t work = 0;
    std::string error;
    ASSERT_TRUE (hr::Partition ({ Ring () }, 0, roof.vertices, roof.triangles, split, work, error)) << error;
    EXPECT_NEAR (split.excludedArea, 200.0 / 9, 2e-5);
    EXPECT_NEAR (split.countedArea, 100 - 200.0 / 9, 2e-5);
}

TEST (MassingHeadroom, RoofRidgeRetainsBothLowEavesAndDoesNotFlattenTheCrease)
{
    geomsrv::Mesh roof;
    roof.vertices = { 0, 0, 0,   10, 0, 0,   10, 10, 0,   0, 10, 0,   0, 0,  0.8,
                      5, 0, 3.2, 10, 0, 0.8, 10, 10, 0.8, 5, 10, 3.2, 0, 10, 0.8 };
    roof.triangles = { 0, 2, 1, 0, 3, 2, 4, 5, 8, 4, 8, 9, 5, 6, 7, 5, 7, 8, 0, 1, 6, 0, 6, 5,
                       0, 5, 4, 3, 9, 8, 3, 8, 7, 3, 7, 2, 0, 4, 9, 0, 9, 3, 1, 2, 7, 1, 7, 6 };
    hr::Split split;
    size_t work = 0;
    std::string error;
    ASSERT_TRUE (hr::Partition ({ Ring () }, 0, roof.vertices, roof.triangles, split, work, error)) << error;
    EXPECT_NEAR (split.countedArea, 200.0 / 3, 2e-5);
    EXPECT_NEAR (split.excludedArea, 100.0 / 3, 2e-5);
    EXPECT_EQ (split.excluded.size (), 2u);
    auto input = Slab ();
    input.slab.bottom = 0.1; // A floor above the bottom-cap event also exercises the local clearance datum.
    input.body = std::make_shared<const geomsrv::Mesh> (roof);
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.rawArea, 62.5, 2e-5);
    EXPECT_NEAR (result.excludedArea, 37.5, 2e-5);
}
