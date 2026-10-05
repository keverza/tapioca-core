#include "ArchViz/MassingSlices.hpp"
#include "Geometry/Primitives.hpp"
#include "NodeGraph/Json.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <fstream>
#include <iterator>

namespace ms = geomsrv::archviz::massingslices;
namespace meta = geomsrv::metadata;
namespace layers = geomsrv::archviz::overlaylayers;
using geomsrv::archviz::SliceChain;

namespace {
SliceChain Ring (double x0, double y0, double x1, double y1)
{
    SliceChain ring;
    ring.closed = true;
    ring.xy = { x0, y0, x1, y0, x1, y1, x0, y1 };
    return ring;
}
layers::Mesh Box (double x0 = 0, double y0 = 0, double x1 = 10, double y1 = 10)
{
    geomsrv::Mesh raw;
    std::string error;
    EXPECT_TRUE (geomsrv::engine::MakeBox ({ (x0 + x1) / 2, (y0 + y1) / 2, 5 }, x1 - x0, y1 - y0, 12, raw, error));
    layers::Mesh mesh;
    mesh.points = raw.vertices;
    mesh.indices = raw.triangles;
    return mesh;
}
ms::Input Slab (double height = 9)
{
    ms::Input input;
    input.slab.guid = "slab";
    input.slab.id = "A";
    input.slab.outer.xy = Ring (0, 0, 10, 10).xy;
    input.slab.top = height;
    return input;
}
void Set (ms::Input& input, const char* key, meta::Value value)
{
    meta::Property property;
    property.key = key;
    property.value = std::move (value);
    meta::SetProperty (input.metadata, std::move (property));
}
geomsrv::archviz::massingcalculation::Result Envelope (layers::Mesh mesh = Box ())
{
    geomsrv::archviz::massingcalculation::Result envelope;
    envelope.hasEnvelope = true;
    envelope.layer.meshes.push_back (std::move (mesh));
    return envelope;
}
} // namespace

TEST (MassingSlices, ThinSelectedSlabIsOneFloorWithoutAnEnvelope)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab (0.3) }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 1u);
    EXPECT_DOUBLE_EQ (result.rawArea, 100);
    EXPECT_FALSE (result.clipped);
    EXPECT_EQ (result.layer.name, ms::kLayer);
    EXPECT_FALSE (result.layer.meshes.empty ());
    EXPECT_FALSE (result.layer.polylines.empty ());
}

TEST (MassingSlices, FloorHeightIsFloorToFloorAndLegacyStoryIndexDoesNotControlTheSlices)
{
    auto input = Slab (12);
    Set (input, "massing.floorHeight", meta::Value::Number (4, meta::ValueType::Length));
    Set (input, "massing.story", meta::Value::Integer (-1));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ (result.rows[i].z, double (i) * 4);
        EXPECT_EQ (result.rows[i].story, int (i));
    }
}

TEST (MassingSlices, FunctionChangeRestylesFillAndOutlineButNotGeometry)
{
    auto input = Slab (0.3);
    ms::Result residential, commercial;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, residential, error)) << error;
    Set (input, "massing.function", meta::Value::Text ("commercial"));
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, commercial, error)) << error;
    ASSERT_FALSE (commercial.layer.meshes.empty ());
    ASSERT_FALSE (commercial.layer.polylines.empty ());
    EXPECT_EQ (residential.layer.meshes[0].rgba, 0xF2C14E59u);
    EXPECT_EQ (commercial.layer.meshes[0].rgba, 0xE4572E59u);
    EXPECT_EQ (commercial.layer.polylines[0].rgba, 0xE4572EFFu);
    EXPECT_EQ (residential.rawArea, commercial.rawArea);
    EXPECT_EQ (residential.layer.meshes[0].points, commercial.layer.meshes[0].points);
}

TEST (MassingSlices, PickedStoryFunctionRangeRestylesOnlyThoseSlices)
{
    auto input = Slab ();
    ms::Result initial, changed;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, initial, error)) << error;
    EXPECT_EQ (initial.section.key, "massing.function");
    const auto edits = geomsrv::archviz::hudsection::RunEdits (initial.section, { 1, 1 }, "commercial", false);
    ASSERT_EQ (edits.size (), 1u);
    EXPECT_EQ (edits[0].type, meta::ValueType::String);
    ASSERT_TRUE (geomsrv::archviz::hudmeta::Apply (input.metadata, edits[0], meta::DefaultSchema (), 0, error))
        << error;
    EXPECT_TRUE (meta::Validate (input.metadata, meta::DefaultSchema ()).empty ());
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, changed, error)) << error;
    ASSERT_EQ (changed.rows.size (), 3u);
    EXPECT_EQ (changed.rows[0].function, "residential");
    EXPECT_EQ (changed.rows[1].function, "commercial");
    EXPECT_EQ (changed.rows[2].function, "residential");
    ASSERT_EQ (changed.layer.meshes.size (), 3u);
    EXPECT_EQ (changed.layer.meshes[0].rgba, 0xF2C14E59u);
    EXPECT_EQ (changed.layer.meshes[1].rgba, 0xE4572E59u);
    EXPECT_EQ (changed.layer.meshes[2].rgba, 0xF2C14E59u);
    EXPECT_EQ (changed.section.floors[1].parts[0].rgba, 0xE4572EFFu);
}

TEST (MassingSlices, IntersectionKeepsSlabHolesAndReportsTheSameAllowedArea)
{
    auto input = Slab (0.3);
    input.slab.holes.push_back ({ Ring (4, 4, 6, 6).xy, {} });
    const auto envelope = Envelope (Box (2, 2, 8, 8));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
    EXPECT_TRUE (result.clipped);
    EXPECT_DOUBLE_EQ (result.rawArea, 96);
    EXPECT_NEAR (result.allowedArea, 32, 1e-6);
    ASSERT_EQ (result.rows.size (), 1u);
    EXPECT_NEAR (result.rows[0].allowedArea, 32, 1e-6);
    EXPECT_EQ (result.layer.polylines.size (), 2u);
}

TEST (MassingSlices, ShellCutsShrinkAtEachLevelRatherThanReusingItsFootprint)
{
    auto mesh = Box ();
    for (size_t i = 0; i < mesh.points.size (); i += 3)
        if (mesh.points[i + 2] > 5) {
            mesh.points[i] = 5 + (mesh.points[i] - 5) * 0.4;
            mesh.points[i + 1] = 5 + (mesh.points[i + 1] - 5) * 0.4;
        }
    const auto envelope = Envelope (mesh);
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 3u);
    EXPECT_GT (result.rows[0].allowedArea, result.rows[1].allowedArea);
    EXPECT_GT (result.rows[1].allowedArea, result.rows[2].allowedArea);
    // At z=3, one-third of the way from -1 to 11: each side is 8 m.
    EXPECT_NEAR (result.rows[1].allowedArea, 64, 1e-5);
}

TEST (MassingSlices, EmptyIntersectionIsZeroWithoutDrawingTheUnclippedSlab)
{
    const auto envelope = Envelope (Box (20, 20, 30, 30));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    EXPECT_TRUE (result.clipped);
    EXPECT_EQ (result.rows.size (), 3u);
    EXPECT_DOUBLE_EQ (result.allowedArea, 0);
    EXPECT_TRUE (result.layer.polylines.empty ());
    EXPECT_TRUE (result.layer.meshes.empty ());
}

TEST (MassingSlices, PartialPreviewCannotFabricateAllowedAreas)
{
    auto envelope = Envelope ();
    envelope.hasEnvelope = false;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    EXPECT_FALSE (result.clipped);
    EXPECT_DOUBLE_EQ (result.rawArea, 300);
    EXPECT_DOUBLE_EQ (result.allowedArea, 0);
    EXPECT_FALSE (result.layer.meshes.empty ());
}

TEST (MassingSlices, IntersectionsRetainDisconnectedComponents)
{
    auto mesh = Box (0, 0, 2, 2);
    const auto other = Box (8, 8, 10, 10);
    const auto base = uint32_t (mesh.points.size () / 3);
    mesh.points.insert (mesh.points.end (), other.points.begin (), other.points.end ());
    for (uint32_t index : other.indices)
        mesh.indices.push_back (base + index);
    std::vector<SliceChain> outlines;
    double area = -1;
    std::string error;
    ASSERT_TRUE (ms::Intersect ({ Ring (0, 0, 10, 10) }, mesh, 3, outlines, area, error)) << error;
    EXPECT_EQ (outlines.size (), 2u);
    EXPECT_NEAR (area, 8, 1e-6);
}

TEST (MassingSlices, SurveyCoordinatesAreIntersectedInDoublePrecision)
{
    constexpr double x = 1000000, y = -2000000;
    auto mesh = Box (x, y, x + 10, y + 10);
    std::vector<SliceChain> outlines;
    double area = -1;
    std::string error;
    ASSERT_TRUE (ms::Intersect ({ Ring (x + 9.123, y, x + 20, y + 20) }, mesh, 3, outlines, area, error)) << error;
    EXPECT_NEAR (area, 8.77, 1e-5);
}

TEST (MassingSlices, InvalidMeshOrOpenContourRefusesAtomically)
{
    auto mesh = Box ();
    std::vector<SliceChain> outlines { Ring (100, 100, 110, 110) };
    const auto before = outlines[0].xy;
    double area = -1;
    std::string error;
    mesh.indices[0] = 99999;
    EXPECT_FALSE (ms::Intersect ({ Ring (0, 0, 10, 10) }, mesh, 3, outlines, area, error));
    EXPECT_EQ (outlines[0].xy, before);
    EXPECT_EQ (area, -1);
    mesh = Box ();
    auto ring = Ring (0, 0, 10, 10);
    ring.closed = false;
    EXPECT_FALSE (ms::Intersect ({ ring }, mesh, 3, outlines, area, error));
    mesh.points[0] = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (ms::Intersect ({ Ring (0, 0, 10, 10) }, mesh, 3, outlines, area, error));
}

TEST (MassingSlices, BadFloorHeightAndSlopedSlabsDoNotPublishPartialFigures)
{
    auto input = Slab ();
    Set (input, "massing.floorHeight", meta::Value::Number (0, meta::ValueType::Length));
    ms::Result result;
    result.note = "sentinel";
    std::string error;
    EXPECT_FALSE (ms::Build ({ input }, {}, nullptr, result, error));
    EXPECT_EQ (result.note, "sentinel");
    input = Slab ();
    input.slab.slopedEdges = 1;
    EXPECT_FALSE (ms::Build ({ input }, {}, nullptr, result, error));
    EXPECT_EQ (result.note, "sentinel");
}

TEST (MassingSlices, SharedPythonConvexAndConcaveShellsCutAndIntersectAtEveryFloor)
{
    namespace js = evp::nodegraph::json;
    std::ifstream file (EVP_TEST_FIXTURE_DIR "/massing-shells.json", std::ios::binary);
    ASSERT_TRUE (file.good ());
    const std::string text { std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> () };
    const auto fixtures = js::Parse (text);
    ASSERT_TRUE (fixtures.ok);
    const auto* cases = fixtures.value.AsArray ();
    ASSERT_NE (cases, nullptr);
    ASSERT_EQ (cases->size (), 2u);
    for (const auto& fixture : *cases) {
        const auto* payload = fixture.Find ("payload");
        ASSERT_NE (payload, nullptr);
        const auto bridge =
            js::JsonValue::Object ({ { "ok", js::JsonValue::Bool (true) },
                                     { "outputs", js::JsonValue::Object ({ { "payload", *payload } }) } });
        geomsrv::archviz::massingcalculation::Result envelope;
        std::string error;
        ASSERT_TRUE (geomsrv::archviz::massingcalculation::Decode (js::Write (bridge, 0), envelope, error)) << error;
        auto input = Slab (30);
        input.slab.outer.xy = Ring (-1, -1, 31, 31).xy;
        ms::Result result;
        ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
        ASSERT_EQ (result.rows.size (), 10u);
        EXPECT_NEAR (result.rows[0].allowedArea, envelope.allowedArea, 1e-4);
        for (size_t i = 1; i < result.rows.size (); ++i)
            EXPECT_LE (result.rows[i].allowedArea, result.rows[i - 1].allowedArea + 1e-4);
        EXPECT_DOUBLE_EQ (result.rows.back ().allowedArea, 0);
    }
}

TEST (MassingSlices, OrderedHeightsAllowATallerFirstFloorAndRepeatTheLastEntry)
{
    auto input = Slab (16);
    meta::Value heights;
    ASSERT_TRUE (geomsrv::archviz::hudmeta::ParseHeights ("4, 3", heights));
    Set (input, "massing.floorHeight", heights);
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 5u);
    EXPECT_EQ (result.rows[0].floorHeight, 4);
    EXPECT_EQ (result.rows[1].z, 4);
    EXPECT_EQ (result.rows[4].z, 13);
    EXPECT_EQ (result.rows[4].floorHeight, 3);
    EXPECT_EQ (result.rawFirstFloorArea, 100);
    EXPECT_EQ (result.rawArea, 500);
    ASSERT_EQ (result.heightControls.size (), 1u);
    EXPECT_EQ (result.heightControls[0].element, "slab");
    EXPECT_EQ (result.heightControls[0].fields[1].numbers, std::vector<double> ({ 4, 3 }));
}

TEST (MassingSlices, ArchicadModeUsesActualStoryElevationsRatherThanARepeatedGap)
{
    auto input = Slab (16);
    Set (input, "massing.heightMode", meta::Value::Option ("archicad"));
    geomsrv::archviz::ProjectStoreys storeys;
    storeys.levels = { -3, 0, 4.5, 7.5, 10.7, 14, 17 };
    storeys.indices = { -1, 0, 1, 2, 3, 4, 5 };
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, storeys, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 5u);
    EXPECT_EQ (result.rows[0].floorHeight, 4.5);
    EXPECT_EQ (result.rows[2].z, 7.5);
    EXPECT_EQ (result.rows[3].z, 10.7);
    EXPECT_EQ (result.heightControls[0].fields.size (), 1u);
    EXPECT_FALSE (ms::Build ({ input }, {}, nullptr, result, error));
    Set (input, "massing.heightMode", meta::Value::Option ("override"));
    ASSERT_TRUE (ms::Build ({ input }, storeys, nullptr, result, error)) << error;
    EXPECT_EQ (result.rows[1].z, 3);
}

TEST (MassingSlices, FacadeExcludesTopBottomSharedAndIntersectingWalls)
{
    auto a = Slab (9), b = Slab (9);
    b.slab.guid = "other";
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ a }, {}, nullptr, result, error)) << error;
    ASSERT_TRUE (result.hasFacade);
    EXPECT_NEAR (result.facadeArea, 360, 1e-6);
    ASSERT_TRUE (ms::Build ({ a, b }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 360, 1e-6); // identical volumes, not twice the facade
    b.slab.outer.xy = Ring (10, 0, 20, 10).xy;
    ASSERT_TRUE (ms::Build ({ a, b }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 540, 1e-6); // adjacent shared face removed
    b.slab.outer.xy = Ring (5, 0, 15, 10).xy;
    ASSERT_TRUE (ms::Build ({ a, b }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 450, 1e-6); // intersecting parts removed
    b.slab.bottom = 3;
    b.slab.top = 6;
    ASSERT_TRUE (ms::Build ({ a, b }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 390, 1e-6); // 40*6 + 50*3
}

TEST (MassingSlices, FacadeIncludesCourtyardWallsOnlyWhileTheyFaceAir)
{
    auto a = Slab (9), b = Slab (9);
    a.slab.holes.push_back ({ Ring (4, 4, 6, 6).xy, {} });
    b.slab.outer.xy = Ring (4, 4, 6, 6).xy;
    b.slab.guid = "infill";
    b.slab.bottom = 3;
    b.slab.top = 6;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ a }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 432, 1e-6);
    ASSERT_TRUE (ms::Build ({ a, b }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 408, 1e-6); // courtyard is filled for three metres
}

TEST (MassingSlices, FirstFloorUsesTheSameIntersectionAsTheDisplayedFloor)
{
    const auto envelope = Envelope (Box (2, 2, 8, 8));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    EXPECT_NEAR (result.firstFloorArea, 36, 1e-6);
    EXPECT_NEAR (result.allowedArea, 108, 1e-6);
    EXPECT_NEAR (result.rawFirstFloorArea, 100, 1e-6);
    EXPECT_NEAR (result.facadeArea, 360, 1e-6); // actual slabs, not fictional envelope-cut walls
}
