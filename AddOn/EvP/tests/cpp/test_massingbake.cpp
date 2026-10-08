#include "ArchViz/MassingBake.hpp"
#include "Geometry/Primitives.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <cmath>
#include <algorithm>

namespace bake = geomsrv::archviz::massingbake;
namespace ms = geomsrv::archviz::massingslices;
namespace js = evp::nodegraph::json;
using geomsrv::archviz::SliceChain;
namespace {
SliceChain Ring (double a, double b, double c, double d)
{
    return { { a, b, c, b, c, d, a, d }, true };
}
} // namespace
TEST (MassingBake, SlabCleanupRemovesTheLoggedMicrometreCornerStepsBeforeNativeCreation)
{
    geomsrv::archviz::slabslices::Ring input;
    // First three vertices are verbatim from the 2026-10-08 10:02:40 failure.
    // The remaining rectangle closes a representative 1.9-micrometre stair-step.
    input.xy = { 96.14302738290297,  -261.1064460082009, 96.14302738290297,  -246.10674832295922, 96.14302738290297,
                 -246.1067464156106, 96.14302547555434,  -246.1067464156106, 96.14302547555434,   -246.106744508262,
                 86.14302738290297,  -246.106744508262,  86.14302738290297,  -261.1064460082009 };
    const auto before = input.xy;
    geomsrv::archviz::slabslices::Ring cleaned;
    std::string error;
    ASSERT_TRUE (bake::CleanSlabRing (input, cleaned, error)) << error;
    ASSERT_EQ (cleaned.xy.size (), 8u);
    EXPECT_EQ (input.xy, before);
    for (size_t i = 0; i < cleaned.xy.size (); i += 2) {
        const size_t j = (i + 2) % cleaned.xy.size ();
        EXPECT_GT (std::hypot (cleaned.xy[i] - cleaned.xy[j], cleaned.xy[i + 1] - cleaned.xy[j + 1]),
                   bake::kSlabEdgeTolerance);
    }
    geomsrv::archviz::slabslices::Ring again;
    ASSERT_TRUE (bake::CleanSlabRing (cleaned, again, error));
    EXPECT_EQ (again.xy, cleaned.xy);
}
TEST (MassingBake, SlabCleanupHandlesClosingNearDuplicatesCollinearPointsAndLargeCoordinates)
{
    geomsrv::archviz::slabslices::Ring input;
    input.xy = { 700000,  6000000, 700005,  6000000,       700010,         6000000, 700010,
                 6000010, 700000,  6000010, 700000.000002, 6000000.000002, 700000,  6000000 };
    geomsrv::archviz::slabslices::Ring cleaned;
    std::string error;
    ASSERT_TRUE (bake::CleanSlabRing (input, cleaned, error)) << error;
    EXPECT_EQ (cleaned.xy,
               (std::vector<double> { 700000, 6000000, 700010, 6000000, 700010, 6000010, 700000, 6000010 }));
    std::reverse (cleaned.xy.begin (), cleaned.xy.end ());
    input = cleaned; // Reflected/reversed coordinates must remain equally regular.
    ASSERT_TRUE (bake::CleanSlabRing (input, cleaned, error));
    EXPECT_EQ (cleaned.xy.size (), 8u);
}
TEST (MassingBake, SlabCleanupPreservesSmallRealNotchesAndDoesNotRoundAnEntireContourToAGrid)
{
    geomsrv::archviz::slabslices::Ring input;
    input.xy = { 0.123456789, 0, 10, 0, 10, 10, 5.0001, 10, 5.0001, 9.9999, 5, 9.9999, 5, 10, 0.123456789, 10 };
    geomsrv::archviz::slabslices::Ring cleaned;
    std::string error;
    ASSERT_TRUE (bake::CleanSlabRing (input, cleaned, error)) << error;
    EXPECT_EQ (cleaned.xy, input.xy);
}
TEST (MassingBake, SlabCleanupRefusesCollapsedHoleInvalidInputsAndArcReplacementWithoutMutatingOutput)
{
    geomsrv::archviz::slabslices::Ring input, cleaned;
    cleaned.xy = { 42, 43 };
    std::string error;
    for (const auto& xy :
         std::vector<std::vector<double>> { {},
                                            { 0, 0, 1, 0, 1 },
                                            { 0, 0, 0.000005, 0, 0, 0.000005 },
                                            { 0, 0, 1, 0, 2, 0 },
                                            { 0, 0, 1, 0, 1, std::numeric_limits<double>::quiet_NaN () } }) {
        input.xy = xy;
        EXPECT_FALSE (bake::CleanSlabRing (input, cleaned, error));
        EXPECT_EQ (cleaned.xy, (std::vector<double> { 42, 43 }));
    }
    input.xy = { 0, 0, 1, 0, 1, 1, 0, 1 };
    input.arcs = { 0.5 };
    EXPECT_FALSE (bake::CleanSlabRing (input, cleaned, error));
    EXPECT_EQ (cleaned.xy, (std::vector<double> { 42, 43 }));
}
TEST (MassingBake, CountedSlicesRetainHolesSeparateIslandsAndPhysicalElevation)
{
    ms::Result slices;
    ms::Row row;
    row.z = 103.4;
    row.floorHeight = 4.2;
    row.chains = { Ring (0, 0, 10, 10), Ring (2, 2, 8, 8), Ring (3, 3, 4, 4), Ring (20, 20, 30, 30) };
    row.lowChains = { Ring (-10, -10, -5, -5) };
    slices.rows.push_back (row);
    js::JsonValue out;
    std::string error;
    ASSERT_TRUE (bake::Geometry (bake::Kind::Slices, &slices, nullptr, {}, out, error)) << error;
    const auto* items = out.Find ("items")->AsArray ();
    ASSERT_EQ (items->size (), 3u);
    size_t holes = 0;
    for (const auto& item : *items) {
        holes += item.Find ("holes")->AsArray ()->size ();
        double z = 0, height = 0;
        ASSERT_TRUE (item.Find ("z")->AsDouble (z));
        ASSERT_TRUE (item.Find ("height")->AsDouble (height));
        EXPECT_DOUBLE_EQ (z, 103.4);
        EXPECT_DOUBLE_EQ (height, 4.2);
    }
    EXPECT_EQ (holes, 1u);
    EXPECT_EQ (slices.rows[0].chains.size (), 4u);
}
TEST (MassingBake, EmptyExcludedSlicesAndInvalidContoursRefuseAtomically)
{
    ms::Result slices;
    slices.rows.push_back ({});
    js::JsonValue out = js::JsonValue::String ("unchanged");
    std::string error;
    EXPECT_FALSE (bake::Geometry (bake::Kind::Slices, &slices, nullptr, {}, out, error));
    std::string held;
    EXPECT_TRUE (out.AsString (held));
    EXPECT_EQ (held, "unchanged");
    slices.complete = false;
    slices.rows[0].chains = { Ring (0, 0, 10, 10) };
    EXPECT_FALSE (bake::Geometry (bake::Kind::Slices, &slices, nullptr, {}, out, error));
    slices.complete = true;
    slices.rows[0].chains = { Ring (0, 0, 10, 10) };
    slices.rows[0].chains[0].closed = false;
    EXPECT_FALSE (bake::Geometry (bake::Kind::Slices, &slices, nullptr, {}, out, error));
    slices.rows[0].chains[0].closed = true;
    slices.rows[0].z = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (bake::Geometry (bake::Kind::Slices, &slices, nullptr, {}, out, error));
}
TEST (MassingBake, EnvelopeUsesClosedWeldedOutwardBodyNotAPrismSubstitute)
{
    geomsrv::Mesh box;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 700005, 6000005, 100 }, 10, 10, 8, box, error));
    geomsrv::archviz::massingcalculation::Result envelope;
    envelope.hasEnvelope = true;
    geomsrv::archviz::overlaylayers::Mesh mesh;
    mesh.points = box.vertices;
    mesh.indices = box.triangles;
    envelope.layer.meshes = { mesh, mesh };
    js::JsonValue out;
    ASSERT_TRUE (bake::Geometry (bake::Kind::Envelope, nullptr, &envelope, {}, out, error)) << error;
    ASSERT_EQ (out.Find ("items")->AsArray ()->size (), 2u);
    const auto& body = *out.Find ("items")->AsArray ()->front ().Find ("body");
    EXPECT_EQ (body.Find ("vertices")->AsArray ()->size (), 8u);
    EXPECT_EQ (body.Find ("polygons")->AsArray ()->size (), 12u);
    envelope.layer.meshes[0].indices.pop_back ();
    EXPECT_FALSE (bake::Geometry (bake::Kind::Envelope, nullptr, &envelope, {}, out, error));
    envelope.layer.meshes[0] = mesh;
    envelope.layer.meshes[0].indices.resize (mesh.indices.size () - 3);
    EXPECT_FALSE (bake::Geometry (bake::Kind::Envelope, nullptr, &envelope, {}, out, error));
}
TEST (MassingBake, CollapseUsesZoneContoursWithCourtyardNotTerrainTriangles)
{
    js::JsonValue out;
    std::string error;
    ASSERT_TRUE (bake::Geometry (bake::Kind::Collapse, nullptr, nullptr,
                                 { Ring (700000, 6000000, 700010, 6000010), Ring (700002, 6000002, 700008, 6000008) },
                                 out, error));
    const auto& items = *out.Find ("items")->AsArray ();
    ASSERT_EQ (items.size (), 1u);
    EXPECT_EQ (items[0].Find ("holes")->AsArray ()->size (), 1u);
    EXPECT_EQ (items[0].Find ("outer")->AsArray ()->size (), 4u);
}
TEST (MassingBake, GraphBridgeBindsRequestAndSettingsAsOneNamedInput)
{
    js::JsonValue geometry;
    std::string error;
    ASSERT_TRUE (bake::Geometry (bake::Kind::Collapse, nullptr, nullptr, { Ring (0, 0, 10, 10) }, geometry, error));
    const auto parsed = js::Parse (bake::Inputs (geometry, js::JsonValue::Object ({}), 123));
    ASSERT_TRUE (parsed.ok);
    ASSERT_NE (parsed.value.Find ("request"), nullptr);
    const auto& request = *parsed.value.Find ("request");
    ASSERT_NE (request.Find ("settings"), nullptr);
    ASSERT_NE (request.Find ("items"), nullptr);
    int64_t token = 0;
    ASSERT_TRUE (request.Find ("token")->AsInteger (token));
    EXPECT_EQ (token, 123);
    EXPECT_EQ (parsed.value.Find ("kind"), nullptr);
}
TEST (MassingBake, HomeStoriesStartClosestAdvancePerFloorAndClampAtHighest)
{
    geomsrv::archviz::ProjectStoreys storeys;
    storeys.levels = { -3, 0, 6, 12, 18 };
    storeys.indices = { -1, 0, 1, 2, 3 };
    const auto homes = bake::HomeStoreys (storeys, { 3.2, 7.2, 11.2, 15.2, 19.2, 23.2, 7.2, -2.7 },
                                          { "A", "A", "A", "A", "A", "A", "A", "B" });
    EXPECT_EQ (homes, (std::vector<size_t> { 2, 3, 4, 4, 4, 4, 3, 0 }));
    EXPECT_TRUE (bake::HomeStoreys ({}, { 0 }, { "A" }).empty ());
}
TEST (MassingBake, Export2DJsonRetainsCountedHolesIslandsSourceFloorsAndBuildingIdentity)
{
    ms::Result slices;
    ms::Row row;
    row.guid = "source-slab";
    row.story = -2;
    row.z = 103.4;
    row.floorHeight = 4.2;
    row.chains = { Ring (700000, 6000000, 700010, 6000010), Ring (700002, 6000002, 700008, 6000008),
                   Ring (700020, 6000020, 700030, 6000030) };
    row.lowChains = { Ring (-10, -10, -5, -5) };
    slices.rows = { row, row };
    slices.rows[1].story = -1;
    slices.rows[1].z += 4.2;
    slices.buildingSurfaces.push_back ({ { row.guid, "Building \"A\"" }, {} });
    std::string text, error;
    ASSERT_TRUE (bake::Export2DJson (slices, text, error)) << error;
    const auto parsed = js::Parse (text);
    ASSERT_TRUE (parsed.ok);
    std::string format, units;
    ASSERT_TRUE (parsed.value.Find ("format")->AsString (format));
    ASSERT_TRUE (parsed.value.Find ("units")->AsString (units));
    EXPECT_EQ (format, "tapioca.story-slices.2d");
    EXPECT_EQ (units, "m");
    const auto* exported = parsed.value.Find ("slices")->AsArray ();
    ASSERT_EQ (exported->size (), 4u);
    size_t holes = 0;
    for (const auto& item : *exported) {
        std::string source, group;
        ASSERT_TRUE (item.Find ("sourceGuid")->AsString (source));
        ASSERT_TRUE (item.Find ("group")->AsString (group));
        EXPECT_EQ (source, row.guid);
        EXPECT_EQ (group, "building:Building \"A\"");
        int64_t story = 0;
        double z = 0;
        ASSERT_TRUE (item.Find ("story")->AsInteger (story));
        ASSERT_TRUE (item.Find ("z")->AsDouble (z));
        EXPECT_TRUE (story == -2 || story == -1);
        EXPECT_DOUBLE_EQ (z, story == -2 ? 103.4 : 107.6);
        holes += item.Find ("holes")->AsArray ()->size ();
        ASSERT_EQ (item.Find ("outer")->AsArray ()->size (), 4u);
        for (const auto& point : *item.Find ("outer")->AsArray ()) {
            double x = 0;
            ASSERT_TRUE (point.Find ("x")->AsDouble (x));
            EXPECT_GE (x, 700000); // Gray excluded contours were not restored.
        }
    }
    EXPECT_EQ (holes, 2u);
    EXPECT_EQ (slices.rows[0].chains.size (), 3u);
    EXPECT_EQ (slices.rows[0].lowChains.size (), 1u);
    EXPECT_EQ (parsed.value.Find ("settings"), nullptr);
    EXPECT_EQ (parsed.value.Find ("token"), nullptr);
}
TEST (MassingBake, Export2DRefusesIncompleteEmptyAndInvalidSourcesWithoutChangingOutput)
{
    ms::Result slices;
    std::string text = "unchanged", error;
    EXPECT_FALSE (bake::Export2DJson (slices, text, error));
    EXPECT_EQ (text, "unchanged");
    ms::Row row;
    row.chains = { Ring (0, 0, 10, 10) };
    slices.rows = { row };
    slices.complete = false;
    EXPECT_FALSE (bake::Export2DJson (slices, text, error));
    EXPECT_EQ (text, "unchanged");
    slices.complete = true;
    slices.rows[0].chains[0].closed = false;
    EXPECT_FALSE (bake::Export2DJson (slices, text, error));
    EXPECT_EQ (text, "unchanged");
    slices.rows[0].chains[0].closed = true;
    ASSERT_TRUE (bake::Export2DJson (slices, text, error)) << error;
    const auto parsed = js::Parse (text);
    std::string group;
    ASSERT_TRUE (parsed.value.Find ("slices")->AsArray ()->front ().Find ("group")->AsString (group));
    EXPECT_EQ (group, "slab:"); // Missing building ID is not inferred.
}
TEST (MassingBake, CircularRunsBecomeRealSignedArcsWhileCornersRemainLines)
{
    geomsrv::archviz::slabslices::Ring ring;
    for (int i = 0; i < 64; ++i) {
        const double angle = i * 2 * 3.141592653589793 / 64;
        ring.xy.insert (ring.xy.end (), { 700000 + 10 * std::cos (angle), 6000000 + 10 * std::sin (angle) });
    }
    const auto rounded = bake::CircularRing (ring);
    ASSERT_LT (rounded.xy.size (), ring.xy.size ());
    EXPECT_GE (rounded.xy.size (), 6u);
    double sweep = 0;
    for (double arc : rounded.arcs)
        sweep += arc;
    EXPECT_NEAR (sweep, 2 * 3.141592653589793, 1e-6);
    ring.xy = { 0, 0, 10, 0, 10, 10, 0, 10 };
    EXPECT_EQ (bake::CircularRing (ring).xy, ring.xy);
    for (double arc : bake::CircularRing (ring).arcs)
        EXPECT_DOUBLE_EQ (arc, 0);
}
