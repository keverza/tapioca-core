#include "ArchViz/DiagramPublication.hpp"
#include "ArchViz/DiagramReplay.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dg = geomsrv::archviz::diagram;
namespace {
std::string Fixture ()
{
    const auto file = std::filesystem::path (__FILE__).parent_path () / "fixtures/diagram-capture-stepped-v1.json";
    std::ifstream in (file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf ();
    EXPECT_FALSE (text.str ().empty ()) << file.string ();
    return text.str ();
}
dg::Prepared Replay (const std::string& text, dg::StoreyBindings bindings = {})
{
    dg::DiagramInputSnapshot snapshot;
    std::string error;
    EXPECT_TRUE (dg::ReplayCapture (text, snapshot, error)) << error;
    return dg::Prepare (snapshot, std::move (bindings), dg::DefaultRequest (), 1);
}
const dg::PlanNode* Node (const dg::LayoutPlan& plan, const std::string& key)
{
    for (const auto& node : plan.nodes)
        if (node.id.key == key)
            return &node;
    return nullptr;
}
} // namespace

TEST (DiagramReplay, FixtureBuildsTheStoreyScene)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication) << (prepared.notes.empty () ? "" : prepared.notes.front ());
    const auto& p = *prepared.publication;
    EXPECT_TRUE (prepared.status.compatible);
    EXPECT_EQ (p.domain, dg::StoreyDomain::ProjectStories);
    EXPECT_EQ (p.buildings, 2u); // shared ID 'A' groups g1+g2; g3 stands alone
    EXPECT_EQ (p.storeys, 4u);
    EXPECT_EQ (p.geometry.meshes.size (), 4u);
    EXPECT_EQ (p.representedGuids, (std::vector<std::string> { "g1", "g2", "g3" }));
    EXPECT_EQ (p.revisions.modelStamp, 1207u);
    // Identity comes from the stories' floorIds, never from position.
    const auto& a = prepared.bindings.byBuilding.at ("building:A");
    ASSERT_EQ (a.size (), 3u);
    EXPECT_EQ (a[0].source.persistentId, "floor:11");
    EXPECT_EQ (a[2].source.persistentId, "floor:13");
}

TEST (DiagramReplay, RoundTripIsExact)
{
    dg::DiagramInputSnapshot snapshot;
    std::string error;
    ASSERT_TRUE (dg::ReplayCapture (Fixture (), snapshot, error)) << error;
    const std::string once = dg::WriteCapture (snapshot, "round trip");
    dg::DiagramInputSnapshot again;
    ASSERT_TRUE (dg::ReplayCapture (once, again, error)) << error;
    EXPECT_EQ (dg::WriteCapture (again, "round trip"), once);
    const auto a = dg::Prepare (snapshot, {}, dg::DefaultRequest (), 1);
    const auto b = dg::Prepare (again, {}, dg::DefaultRequest (), 1);
    ASSERT_TRUE (a.publication && b.publication);
    ASSERT_EQ (a.publication->plan.nodes.size (), b.publication->plan.nodes.size ());
    for (size_t i = 0; i < a.publication->plan.nodes.size (); ++i) {
        EXPECT_EQ (a.publication->plan.nodes[i].id, b.publication->plan.nodes[i].id);
        EXPECT_EQ (a.publication->plan.nodes[i].target, b.publication->plan.nodes[i].target);
    }
    ASSERT_EQ (a.publication->geometry.vertices.size (), b.publication->geometry.vertices.size ());
}

TEST (DiagramReplay, RefusesForeignFormatsAndMalformedRows)
{
    dg::CaptureInputs inputs;
    std::string error;
    EXPECT_FALSE (dg::ReadCapture ("{}", inputs, error));
    EXPECT_FALSE (dg::ReadCapture ("not json", inputs, error));
    std::string text = Fixture ();
    const auto version = text.find ("\"version\": 1");
    ASSERT_NE (version, std::string::npos);
    std::string newer = text;
    newer.replace (version, 12, "\"version\": 2");
    EXPECT_FALSE (dg::ReadCapture (newer, inputs, error));
    std::string broken = text;
    broken.replace (broken.find ("\"floorHeight\": 3,"), 17, "\"floorHeight\": \"x\",");
    EXPECT_FALSE (dg::ReadCapture (broken, inputs, error));
    EXPECT_NE (error.find ("row 0"), std::string::npos) << error;
}

TEST (DiagramReplay, MissingSlicesReplayAsARefusal)
{
    const std::string text = R"({"format":"tapioca.diagram.capture","version":1,
        "storeys":{"levels":[],"names":[],"indices":[],"floorIds":[]}})";
    const auto prepared = Replay (text);
    EXPECT_FALSE (prepared.publication);
    EXPECT_FALSE (prepared.status.compatible);
}

TEST (DiagramGeometry, ShellsAreFlatAndNodeLocal)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication);
    const auto& g = prepared.publication->geometry;
    ASSERT_FALSE (g.vertices.empty ());
    EXPECT_EQ (g.indices.size (), g.vertices.size ()); // three unique corners per face
    for (size_t i = 1; i < g.meshes.size (); ++i)
        EXPECT_LT (g.meshes[i - 1].id, g.meshes[i].id);
    for (const auto& v : g.vertices) {
        // Relative to the storey pivot: a 20 m L never strays past its half-extent.
        for (int k = 0; k < 3; ++k)
            EXPECT_LE (std::abs (v.position[k]), 10.001f);
        const float n = v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2];
        EXPECT_NEAR (n, 1.0f, 1e-4f);
        EXPECT_EQ (v.rgba & 0xFFu, 0xFFu); // opaque
    }
    EXPECT_LT (g.Bytes (), size_t (32) << 20); // §5.12 upload budget
}

TEST (DiagramGeometry, DrawsMoveOnlyTheirTranslation)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication);
    const auto& p = *prepared.publication;
    const dg::Vec3 origin {};
    const auto assembled = dg::DrawsOf (p.geometry, p.plan, dg::Evaluate (p.plan, {}, 0.0), origin);
    const auto exploded = dg::DrawsOf (p.geometry, p.plan, dg::Evaluate (p.plan, {}, 1.0), origin);
    ASSERT_EQ (assembled.size (), 4u);
    ASSERT_EQ (exploded.size (), assembled.size ());
    // At t = 0 every storey sits on its source: the translation is its pivot.
    for (const auto& draw : assembled) {
        const auto* node = Node (p.plan, p.geometry.meshes[draw.mesh].id.key);
        ASSERT_TRUE (node);
        for (int k = 0; k < 3; ++k)
            EXPECT_NEAR (draw.translation[k], node->pivot[k], 1e-4);
    }
    // At t = 1 the upper storeys of A rise and nothing sinks.
    float rise = 0;
    for (size_t i = 0; i < assembled.size (); ++i) {
        EXPECT_EQ (assembled[i].mesh, exploded[i].mesh);
        EXPECT_GE (exploded[i].translation[2] + 1e-4f, assembled[i].translation[2]);
        rise = std::max (rise, exploded[i].translation[2] - assembled[i].translation[2]);
    }
    EXPECT_GT (rise, 1.0f);
    // A selection marks its own draw only.
    const auto id = p.geometry.meshes.front ().id;
    const auto selected = dg::DrawsOf (p.geometry, p.plan, dg::Evaluate (p.plan, { id }, 0.6), origin);
    size_t marked = 0;
    for (const auto& draw : selected)
        marked += draw.selected;
    EXPECT_EQ (marked, 1u);
}

TEST (DiagramGeometry, AFrameOfAnotherPlanDrawsNothing)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication);
    dg::Frame stale;
    stale.poses.resize (1);
    EXPECT_TRUE (dg::DrawsOf (prepared.publication->geometry, prepared.publication->plan, stale, {}).empty ());
}

TEST (DiagramPublication, ARefusedSnapshotKeepsTheSessionBindings)
{
    auto first = Replay (Fixture ());
    ASSERT_TRUE (first.publication);
    const auto serial = first.bindings.nextSerial;
    const auto refused = dg::Prepare (dg::Capture (nullptr, {}, {}), first.bindings, dg::DefaultRequest (), 2);
    EXPECT_FALSE (refused.publication);
    // Prepare worked on a copy: the caller's bindings are untouched by a refusal.
    EXPECT_EQ (first.bindings.nextSerial, serial);
    // A replay with the previous bindings keeps every storey ID.
    const auto second = Replay (Fixture (), first.bindings);
    ASSERT_TRUE (second.publication);
    EXPECT_EQ (second.bindings.byBuilding.at ("building:A")[1].id, first.bindings.byBuilding.at ("building:A")[1].id);
}
