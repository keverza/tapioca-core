#include "ArchViz/MassingBake.hpp"
#include "Geometry/Primitives.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <cmath>

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
