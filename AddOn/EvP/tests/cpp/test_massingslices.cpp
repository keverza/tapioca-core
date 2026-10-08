#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/SlabBodies.hpp"
#include "ArchViz/MassingBake.hpp"
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

TEST (MassingSlices, BodySourceUsesOperatedVertexElevationsNotHomeStoryOrCachedBounds)
{
    geomsrv::Mesh body;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 5, 5, -6.5 }, 10, 10, 9, body, error));
    body.bounds.mn[2] = 100;
    body.bounds.mx[2] = 200; // The source extent must not trust stale bounds.
    auto input = Slab (99);
    input.slab.bottom = 50;
    ASSERT_TRUE (geomsrv::archviz::slabslices::FromBody (input.slab, body, error)) << error;
    EXPECT_DOUBLE_EQ (input.slab.bottom, -11);
    EXPECT_DOUBLE_EQ (input.slab.top, -2);
    EXPECT_TRUE (input.slab.bodyRequired);
    EXPECT_EQ (input.slab.guid, "slab");
    EXPECT_EQ (input.slab.id, "A");
}

TEST (MassingSlices, BodySourceRejectsMissingInvalidFlatAndOverBudgetGeometryWithoutChangingTheSource)
{
    geomsrv::Mesh box;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 0, 0, 4.5 }, 10, 10, 9, box, error));
    std::vector<geomsrv::Mesh> bad { {} };
    auto broken = box;
    broken.vertices.push_back (0);
    bad.push_back (broken);
    broken = box;
    broken.triangles[0] = uint32_t (broken.VertexCount ());
    bad.push_back (broken);
    broken = box;
    broken.vertices[0] = std::numeric_limits<double>::quiet_NaN ();
    bad.push_back (broken);
    broken = box;
    for (size_t i = 2; i < broken.vertices.size (); i += 3)
        broken.vertices[i] = 0;
    bad.push_back (broken);
    broken = box;
    broken.vertices.resize (600003);
    bad.push_back (broken);
    for (const auto& body : bad) {
        auto input = Slab (99);
        EXPECT_FALSE (geomsrv::archviz::slabslices::FromBody (input.slab, body, error));
        EXPECT_FALSE (error.empty ());
        EXPECT_DOUBLE_EQ (input.slab.top, 99);
        EXPECT_FALSE (input.slab.bodyRequired);
    }
    auto input = Slab ();
    input.slab.bodyRequired = true;
    ms::Result result;
    EXPECT_FALSE (ms::Build ({ input }, {}, nullptr, result, error));
    EXPECT_NE (error.find ("no prism substitute"), std::string::npos);
}

TEST (MassingSlices, TaperedBodyOnlyMassingSourceFeedsFloorMetadataPlanAndBakeExport)
{
    auto body = std::make_shared<geomsrv::Mesh> ();
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 5, 5, 4.5 }, 10, 10, 9, *body, error));
    for (size_t i = 0; i < body->vertices.size (); i += 3)
        if (body->vertices[i + 2] > 5) {
            body->vertices[i] = 5 + (body->vertices[i] - 5) * 0.5;
            body->vertices[i + 1] = 5 + (body->vertices[i + 1] - 5) * 0.5;
        }
    ms::Input input; // No slab polygon or native thickness: a mesh/Morph-shaped source.
    input.slab.guid = "morph";
    input.body = input.facadeBody = body;
    ASSERT_TRUE (geomsrv::archviz::slabslices::FromBody (input.slab, *body, error));
    Set (input, "tapioca.role", meta::Value::Text ("MassingSlab"));
    Set (input, "massing.buildingId", meta::Value::Text ("A"));
    Set (input, "massing.floorHeight", meta::Value::Number (3, meta::ValueType::Length));
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 3u);
    EXPECT_NEAR (result.rows[0].rawVolume / 3, 100, 1e-4);
    EXPECT_NEAR (result.rows[1].rawVolume / 3, 100 * 25.0 / 36.0, 1e-4);
    EXPECT_NEAR (result.rows[2].rawVolume / 3, 100 * 4.0 / 9.0, 1e-4);
    EXPECT_GT (result.rows[0].rawArea, result.rows[1].rawArea);
    EXPECT_GT (result.rows[1].rawArea, result.rows[2].rawArea);
    const auto previews =
        geomsrv::archviz::massingbuildings::Previews (result.section, { { "morph", "A" } }, {}, { "morph" });
    ASSERT_EQ (previews.size (), 1u);
    const auto plan = geomsrv::archviz::buildingplan::Build (result, previews.front ());
    ASSERT_EQ (plan.floors.size (), 3u);
    EXPECT_TRUE (geomsrv::archviz::buildingplan::Contains (plan.floors[0], { 5, 5 }));
    EXPECT_FALSE (geomsrv::archviz::buildingplan::Contains (plan.floors[2], { 0.5, 0.5 }));
    std::string json;
    ASSERT_TRUE (geomsrv::archviz::massingbake::Export2DJson (result, json, error)) << error;
    EXPECT_NE (json.find ("morph"), std::string::npos);
    EXPECT_NE (json.find ("tapioca.story-slices.2d"), std::string::npos);
    evp::nodegraph::json::JsonValue geometry;
    ASSERT_TRUE (geomsrv::archviz::massingbake::Geometry (geomsrv::archviz::massingbake::Kind::Slices, &result, nullptr,
                                                          {}, geometry, error))
        << error;
    EXPECT_EQ (geometry.Find ("items")->AsArray ()->size (), 3u);
}

TEST (MassingSlices, BodyOnlySourcePreservesCourtyardsAndIslandsWithoutInventingAnOuterBox)
{
    auto body = std::make_shared<geomsrv::Mesh> ();
    std::string error;
    const auto append = [&] (geomsrv::engine::Vector3 centre, double width, bool reverse) {
        geomsrv::Mesh part;
        EXPECT_TRUE (geomsrv::engine::MakeBox (centre, width, width, 9, part, error));
        const auto base = uint32_t (body->VertexCount ());
        body->vertices.insert (body->vertices.end (), part.vertices.begin (), part.vertices.end ());
        for (size_t i = 0; i < part.triangles.size (); i += 3) {
            body->triangles.push_back (base + part.triangles[i]);
            body->triangles.push_back (base + part.triangles[i + (reverse ? 2 : 1)]);
            body->triangles.push_back (base + part.triangles[i + (reverse ? 1 : 2)]);
        }
    };
    append ({ 5, 5, 4.5 }, 10, false);
    append ({ 5, 5, 4.5 }, 2, true);
    append ({ 22, 2, 4.5 }, 4, false);
    ms::Input input;
    input.slab.guid = "mesh";
    input.body = input.facadeBody = body;
    ASSERT_TRUE (geomsrv::archviz::slabslices::FromBody (input.slab, *body, error));
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 3u);
    EXPECT_NEAR (result.rows[0].rawArea, 112, 1e-5);
    EXPECT_EQ (result.rows[0].footprintChains.size (), 3u);
    evp::nodegraph::json::JsonValue geometry;
    ASSERT_TRUE (geomsrv::archviz::massingbake::Geometry (geomsrv::archviz::massingbake::Kind::Slices, &result, nullptr,
                                                          {}, geometry, error))
        << error;
    EXPECT_EQ (geometry.Find ("items")->AsArray ()->size (), 6u); // Two separate slabs at each floor.
}

TEST (MassingSlices, OpenMassingBodySectionsAreRefusedBeforeAreaAndBakeContoursArePublished)
{
    auto body = std::make_shared<geomsrv::Mesh> ();
    body->vertices = { 0, 0, 0, 10, 0, 0, 10, 0, 9, 0, 0, 9 };
    body->triangles = { 0, 1, 2, 0, 2, 3 }; // An open Morph plane, not a building volume.
    ms::Input input;
    input.body = body;
    std::string error;
    ASSERT_TRUE (geomsrv::archviz::slabslices::FromBody (input.slab, *body, error));
    ms::Result result;
    EXPECT_FALSE (ms::Build ({ input }, {}, nullptr, result, error));
    EXPECT_NE (error.find ("open cross-section"), std::string::npos);
    EXPECT_TRUE (result.rows.empty ());
}

TEST (MassingSlices, ThinSelectedSlabRetainsOneFloorButIsGrayAndExcludedWithoutAnEnvelope)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab (0.3) }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 1u);
    EXPECT_DOUBLE_EQ (result.rawArea, 0);
    EXPECT_DOUBLE_EQ (result.excludedArea, 100);
    EXPECT_DOUBLE_EQ (result.rawVolume, 30);
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
    auto input = Slab (3);
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
    auto input = Slab (3);
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
    EXPECT_EQ (result.layer.polylines.size (), 4u); // allowed outer/hole, excess outer/allowed cutout
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
    // z=3 footprint has 8 m sides, but 1.6 m vertical clearance needs the
    // z=4.6 roof section: 7.2 m sides. Its outer strip is gray, not counted.
    EXPECT_NEAR (result.rows[1].allowedArea, 51.84, 2e-5);
    EXPECT_NEAR (result.rows[1].excludedArea, 12.16, 2e-5);
}

TEST (MassingSlices, EmptyIntersectionIsZeroAndShowsOnlyTheRedOutsideRegion)
{
    const auto envelope = Envelope (Box (20, 20, 30, 30));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    EXPECT_TRUE (result.clipped);
    EXPECT_EQ (result.rows.size (), 3u);
    EXPECT_DOUBLE_EQ (result.allowedArea, 0);
    EXPECT_EQ (result.layer.polylines.size (), 3u);
    ASSERT_EQ (result.layer.meshes.size (), 3u);
    EXPECT_TRUE (result.layer.texts.empty ());
    for (const auto& mesh : result.layer.meshes) {
        EXPECT_EQ (mesh.rgba, 0xAA4465FF);
        EXPECT_FLOAT_EQ (mesh.style.opacity, 0.5f);
        EXPECT_NE (mesh.hoverTitle.find ("outside envelope"), std::string::npos);
    }
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

TEST (MassingSlices, DisplayControlsPreserveFunctionColoursAndLabelTheFinalArea)
{
    const auto envelope = Envelope (Box (2, 2, 8, 8));
    geomsrv::archviz::storysliceoverlay::Controls display;
    display.outlineWidthPixels = 4;
    display.outlineBehind = layers::Behind::Hide;
    display.fillOpacity = 0.3f;
    display.outlineRgba = 0xFF00FFFF;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error, display)) << error;
    ASSERT_EQ (result.layer.texts.size (), 3u);
    EXPECT_EQ (result.layer.texts[0].text, "36.0 m\xC2\xB2");
    EXPECT_TRUE (result.layer.texts[0].planar);
    EXPECT_DOUBLE_EQ (result.layer.texts[0].sizeMetres, 0.21);
    EXPECT_EQ (result.layer.texts[0].minProjectedPixels, 9);
    EXPECT_EQ (result.layer.polylines[0].rgba, 0xF2C14EFF);
    EXPECT_EQ (result.layer.polylines[0].widthPixels, 4);
    EXPECT_EQ (result.layer.polylines[0].behind, layers::Behind::Hide);
    EXPECT_FLOAT_EQ (result.layer.meshes[0].style.opacity, 0.3f);
    display.label = false;
    display.fillRgba &= 0xFFFFFF00;
    ASSERT_TRUE (ms::Build ({ Slab () }, {}, &envelope, result, error, display)) << error;
    EXPECT_TRUE (result.layer.texts.empty ());
    EXPECT_TRUE (result.layer.meshes.empty ());
    EXPECT_EQ (result.allowedArea, 108);
}

TEST (MassingSlices, AreaLabelsUseStandaloneFittingAndOneSizeAcrossFunctionColours)
{
    auto large = Slab (3), small = Slab (3);
    small.slab.guid = "small";
    small.slab.outer.xy = Ring (20, 0, 24, 4).xy;
    Set (small, "massing.function", meta::Value::Text ("commercial"));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ large, small }, {}, nullptr, result, error)) << error;
    ASSERT_EQ (result.layer.texts.size (), 2u);
    EXPECT_EQ (result.layer.texts[0].text, "100.0 m\xC2\xB2");
    EXPECT_EQ (result.layer.texts[1].text, "16.0 m\xC2\xB2");
    EXPECT_DOUBLE_EQ (result.layer.texts[0].sizeMetres, result.layer.texts[1].sizeMetres);
    EXPECT_DOUBLE_EQ (result.layer.texts[0].sizeMetres,
                      geomsrv::archviz::storysliceoverlay::LabelSizeMetres (0, 16, 3.5, result.layer.texts[1].text));
    EXPECT_EQ (result.layer.meshes[0].rgba, 0xF2C14E59);
    EXPECT_EQ (result.layer.meshes[1].rgba, 0xE4572E59);
}

TEST (MassingSlices, NamesHoverRowsAndEditorUseTheSameNonRepeatingZeroBasedSequence)
{
    geomsrv::archviz::ProjectStoreys storeys;
    storeys.levels = { 0, 30 };
    storeys.indices = { 0, 1 };
    geomsrv::archviz::storysliceoverlay::Controls display;
    display.labelName = true;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab (15) }, storeys, nullptr, result, error, display)) << error;
    ASSERT_EQ (result.rows.size (), 5u);
    ASSERT_EQ (result.section.floors.size (), 5u);
    for (size_t i = 0; i < result.rows.size (); ++i) {
        EXPECT_EQ (result.rows[i].story, int (i));
        EXPECT_EQ (result.layer.meshes[i].hoverTitle, "A S" + std::to_string (i));
        EXPECT_EQ (result.layer.meshes[i].hoverRows[1].second, std::to_string (i));
        EXPECT_EQ (result.section.floors[i].label, "Floor " + std::to_string (i));
        EXPECT_EQ (result.layer.texts[i].text, "A S" + std::to_string (i) + "  100.0 m\xC2\xB2");
    }
}

TEST (MassingSlices, OperatedBodyCutsReplaceThePolygonAndKeepRemovedFloorsIdentities)
{
    auto input = Slab (9);
    auto body = std::make_shared<geomsrv::Mesh> ();
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 5, 5, 6.5 }, 4, 4, 5, *body, error)); // z 4..9
    input.body = body;
    const auto envelope = Envelope (Box (4, 4, 6, 6));
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
    ASSERT_EQ (result.rows.size (), 3u);
    EXPECT_EQ (result.rows[0].rawArea, 0);
    EXPECT_EQ (result.rows[1].rawArea, 0);
    EXPECT_EQ (result.rows[2].rawArea, 16);
    EXPECT_EQ (result.rows[2].story, 2);
    EXPECT_EQ (result.rows[2].z, 6);
    EXPECT_EQ (result.rows[2].allowedArea, 4);
    EXPECT_EQ (result.rawFirstFloorArea, 0);
    EXPECT_TRUE (result.hasFacade);
    EXPECT_NEAR (result.facadeArea, 16 * 5, 1e-5); // measured final body walls, not record prism
    ASSERT_EQ (result.layer.texts.size (), 1u);
    EXPECT_EQ (result.layer.texts[0].text, "4.0 m\xC2\xB2");
    EXPECT_EQ (result.section.floors.size (), 3u);
}

TEST (MassingSlices, OperatedCourtyardAndDisconnectedSolidsSurviveEnvelopeIntersection)
{
    auto input = Slab (9);
    const auto outer = Box (), hole = Box (4, 4, 6, 6), separate = Box (12, 0, 14, 2);
    auto body = std::make_shared<geomsrv::Mesh> ();
    for (const auto* mesh : { &outer, &hole, &separate }) {
        const auto base = uint32_t (body->VertexCount ());
        body->vertices.insert (body->vertices.end (), mesh->points.begin (), mesh->points.end ());
        for (uint32_t index : mesh->indices)
            body->triangles.push_back (base + index);
    }
    input.body = body;
    const auto envelope = Envelope (Box (-1, -1, 15, 11));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
    EXPECT_NEAR (result.rawArea, 300, 1e-6); // 100 - 4 + 4, three floors
    EXPECT_NEAR (result.allowedArea, 300, 1e-6);
    EXPECT_EQ (result.layer.polylines.size (), 9u);
    body->triangles[0] = 999999;
    EXPECT_FALSE (ms::Build ({ input }, {}, &envelope, result, error));
}

TEST (SlabBodies, IndependentConsumersAndTicketsPreventStaleOrMissingBodies)
{
    namespace bodies = geomsrv::archviz::slabbodies;
    bodies::Clear ();
    bodies::Want ({ "standalone" });
    bodies::Want ({ "massing" }, "massing");
    const auto capture = bodies::Capture ();
    ASSERT_EQ (capture.size (), 2u);
    geomsrv::Mesh a, b;
    a.guid = "standalone";
    b.guid = "massing";
    bodies::Publish ({ a, b }, capture);
    ASSERT_EQ (bodies::Latest ()->meshes.size (), 2u);
    bodies::Invalidate ({ "massing" });
    EXPECT_EQ (bodies::Latest ()->meshes.count ("massing"), 0u);
    bodies::Publish ({ a, b }, capture);
    EXPECT_EQ (bodies::Latest ()->meshes.count ("massing"), 0u) << "an old in-flight pass is refused";
    const auto fresh = bodies::Capture ();
    bodies::Publish ({ a, b }, fresh);
    EXPECT_EQ (bodies::Latest ()->meshes.count ("massing"), 1u);
    bodies::Want ({}); // turning standalone slices off does not cancel massing
    EXPECT_EQ (bodies::Wanted (), (std::set<std::string> { "massing" }));
    bodies::Publish ({}, bodies::Capture ());
    EXPECT_TRUE (bodies::Latest ()->meshes.empty ()) << "a hidden/deleted body cannot reuse a previous pass";
    bodies::Clear ();
    bodies::Want ({ "massing" }, "massing");
    bodies::Publish ({ b }, fresh);
    EXPECT_TRUE (bodies::Latest ()->meshes.empty ()) << "project reset never reuses request tickets";
    bodies::Clear ();
}
