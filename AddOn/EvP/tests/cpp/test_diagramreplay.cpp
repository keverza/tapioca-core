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

namespace {
// A node-local axis box as two triangles per face: the diagonals are coplanar and must
// stay undrawn; the twelve box edges are creases.
dg::PickMesh Box (const std::string& key, double w, double d, double h)
{
    dg::PickMesh mesh;
    mesh.id = { dg::Level::Storey, key };
    for (int i = 0; i < 8; ++i)
        for (const double v : { (i & 1 ? w : -w) / 2, (i & 2 ? d : -d) / 2, (i & 4 ? h : -h) / 2 })
            mesh.positions.push_back (v);
    const uint32_t quads[6][4] = { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 },
                                   { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
    for (const auto& q : quads)
        for (const uint32_t c : { q[0], q[1], q[2], q[0], q[2], q[3] })
            mesh.indices.push_back (c);
    return mesh;
}
} // namespace

TEST (DiagramGeometry, OutlinesAreTheCreasesNotTheDiagonals)
{
    dg::BuiltScene built;
    built.shells.push_back (Box ("storey:box:1", 4, 2, 3));
    const auto g = dg::BuildGeometry (built);
    ASSERT_EQ (g.meshes.size (), 1u);
    EXPECT_EQ (g.meshes[0].lineCount, 24u); // 12 edges, two vertices each
    size_t vertical = 0;
    for (uint32_t i = 0; i + 1 < g.lines.size (); i += 2) {
        const auto& a = g.lines[i].position;
        const auto& b = g.lines[i + 1].position;
        int axes = 0;
        for (int k = 0; k < 3; ++k)
            axes += std::abs (a[k] - b[k]) > 1e-6f;
        EXPECT_EQ (axes, 1) << "a diagonal was drawn";
        vertical += std::abs (a[2] - b[2]) > 1e-6f;
        EXPECT_EQ (g.lines[i].end, 0.0f);
    }
    EXPECT_EQ (vertical, 4u);

    // A T-junction: the roof gets a vertex mid-way along one rim edge that its wall does
    // not share. The roof's split rim halves and its fan are not drawn; the wall still
    // draws the rim, so the outline is the same twelve edges.
    auto split = Box ("storey:box:2", 4, 2, 3);
    // Drop the roof (the second quad) ...
    split.indices.erase (split.indices.begin () + 6, split.indices.begin () + 12);
    const uint32_t mid = uint32_t (split.positions.size () / 3);
    for (const double v : { 0.0, -1.0, 1.5 })
        split.positions.push_back (v); // ... midway along roof rim 4-5
    for (const uint32_t c : { 4u, mid, 6u, mid, 7u, 6u, mid, 5u, 7u })
        split.indices.push_back (c);
    dg::BuiltScene tee;
    tee.shells.push_back (split);
    EXPECT_EQ (dg::BuildGeometry (tee).meshes[0].lineCount, 24u);

    // A sliver: slice chains repeat a corner a few micrometres away, across a weld cell
    // boundary. The cap triangle from the twin to the far corner collapses when welded and
    // must not make the roof diagonal non-manifold.
    auto sliver = Box ("storey:box:3", 4, 2, 3);
    const uint32_t twin = uint32_t (sliver.positions.size () / 3);
    for (const double v : { 2.0 + 0.5e-4 + 2e-6, 1.0, 1.5 }) // vertex 7, just over a cell edge
        sliver.positions.push_back (v);
    sliver.positions[7 * 3] = 2.0 + 0.5e-4 - 1e-6;
    for (const uint32_t c : { 4u, 7u, twin })
        sliver.indices.push_back (c);
    dg::BuiltScene thin;
    thin.shells.push_back (sliver);
    EXPECT_EQ (dg::BuildGeometry (thin).meshes[0].lineCount, 24u);

    EXPECT_EQ (g.Bytes (), g.vertices.size () * sizeof (dg::DiagramVertex) + g.indices.size () * 4 +
                               g.lines.size () * sizeof (dg::DiagramLineVertex));
}

TEST (DiagramGeometry, ConnectorsSpanTheGapBetweenConsecutiveStoreys)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication);
    const auto& p = *prepared.publication;
    // Building A stacks three storeys: two pairs. The lone slab has nothing to connect.
    ASSERT_EQ (p.geometry.connectors.size (), 2u);
    for (const auto& c : p.geometry.connectors) {
        EXPECT_GT (c.lineCount, 0u);
        EXPECT_EQ (c.lineCount % 2, 0u);
        const auto* upper = Node (p.plan, c.upper.key);
        const auto* lower = Node (p.plan, c.lower.key);
        ASSERT_TRUE (upper && lower);
        EXPECT_EQ (upper->sibling, lower->sibling + 1);
    }
    // Assembled: the stack touches, so no connector is drawn; outlines still are.
    const auto still =
        dg::LineDrawsOf (p.geometry, p.plan, dg::Evaluate (dg::Spaced (p.plan, { 0.0 }), {}, 1.0), {}, true, true);
    EXPECT_EQ (still.size (), p.geometry.meshes.size ());
    for (const auto& draw : still)
        EXPECT_FALSE (draw.dashed);

    // At a 3 m gap every connector line is exactly one gap long, vertical, and dashed from
    // the upper storey's bottom.
    const auto spaced = dg::Spaced (p.plan, { 3.0 });
    const auto frame = dg::Evaluate (spaced, {}, 1.0);
    const auto lines = dg::LineDrawsOf (p.geometry, spaced, frame, {}, false, true);
    ASSERT_EQ (lines.size (), 2u);
    for (const auto& draw : lines) {
        EXPECT_TRUE (draw.dashed);
        for (uint32_t i = draw.firstLine; i < draw.firstLine + draw.lineCount; i += 2) {
            const auto& top = p.geometry.lines[i];
            const auto& bottom = p.geometry.lines[i + 1];
            ASSERT_EQ (top.end, 0.0f);
            ASSERT_EQ (bottom.end, 1.0f);
            float a[3], b[3];
            for (int k = 0; k < 3; ++k) {
                a[k] = top.position[k] + draw.first[size_t (k)];
                b[k] = bottom.position[k] + draw.second[size_t (k)];
            }
            EXPECT_NEAR (a[0], b[0], 1e-3f);
            EXPECT_NEAR (a[1], b[1], 1e-3f);
            EXPECT_NEAR (a[2] - b[2], 3.0f, 1e-3f);
            EXPECT_NEAR (a[2], draw.dashOrigin, 1e-3f);
        }
    }
    // Lines off draws none.
    EXPECT_TRUE (dg::LineDrawsOf (p.geometry, spaced, frame, {}, false, false).empty ());
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

TEST (DiagramPublication, StoreySummariesCarryTheSourceFields)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication);
    const auto& info = prepared.publication->storeyInfo;
    ASSERT_EQ (info.size (), 4u);
    const dg::StoreySummary* top = nullptr;
    for (const auto& [id, s] : info)
        if (s.buildingKey == "building:A" && s.sourceStory == 2)
            top = &s;
    ASSERT_TRUE (top);
    EXPECT_EQ (top->buildingId, "A");
    EXPECT_EQ (top->storyName, "Second");
    EXPECT_DOUBLE_EQ (top->z, 6.0);
    EXPECT_DOUBLE_EQ (top->top, 9.5);
    EXPECT_DOUBLE_EQ (top->rawArea, 64.0);
    EXPECT_DOUBLE_EQ (top->allowedArea, 60.0);
    EXPECT_EQ (top->functions, (std::vector<std::string> { "office" }));
    EXPECT_EQ (top->slabs, (std::vector<std::string> { "g2" }));
    EXPECT_FALSE (top->planned);
}

TEST (DiagramPublication, AClickPicksTheDisplacedStoreyNotItsSource)
{
    const auto prepared = Replay (Fixture ());
    ASSERT_TRUE (prepared.publication);
    const auto& p = *prepared.publication;
    const dg::EntityId* topId = nullptr;
    for (const auto& [id, s] : p.storeyInfo)
        if (s.buildingKey == "building:A" && s.sourceStory == 2)
            topId = &id;
    ASSERT_TRUE (topId);
    const int node = p.plan.Find (*topId);
    ASSERT_GE (node, 0);
    const auto exploded = dg::Evaluate (p.plan, {}, 1.0);
    const auto& pose = exploded.poses[size_t (node)];
    ASSERT_GT (pose.offset[2], 0.5); // it rose
    // Straight down through the set-back top's centre, from above everything.
    const dg::Vec3 down { 0, 0, -1 };
    const dg::Vec3 above { pose.pivot[0] + pose.offset[0], pose.pivot[1] + pose.offset[1], 100 };
    const auto hit = dg::Pick (exploded, p.plan, p.shells, above, down);
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id, *topId);
    EXPECT_NEAR (hit->point[2], 9.5 + pose.offset[2], 1e-6); // its displaced roof, not its source roof
    // A horizontal ray at the top's SOURCE height, beside the lower storeys, finds nothing of it.
    const dg::Vec3 side { -50, 4, 7.5 };
    const auto miss = dg::Pick (exploded, p.plan, p.shells, side, dg::Vec3 { 1, 0, 0 });
    EXPECT_TRUE (!miss || miss->id != *topId);
}

// The live follow keys builds on this: a republished massing whose only change is a stamp
// (every extraction pass bumps the model capture stamp) must not rebuild or re-upload.
TEST (DiagramReplay, ContentSignatureIgnoresStampsButSeesContent)
{
    dg::DiagramInputSnapshot snapshot;
    std::string error;
    ASSERT_TRUE (dg::ReplayCapture (Fixture (), snapshot, error)) << error;
    const std::string base = dg::ContentSignature (snapshot);
    auto revisions = snapshot.revisions;
    revisions.modelStamp += 17;
    revisions.slices += 3;
    const auto restamped = dg::Capture (snapshot.slices, snapshot.storeys, revisions);
    EXPECT_EQ (dg::ContentSignature (restamped), base);
    auto storeys = snapshot.storeys;
    ASSERT_FALSE (storeys.levels.empty ());
    storeys.levels.back () += 0.5;
    const auto moved = dg::Capture (snapshot.slices, storeys, snapshot.revisions);
    EXPECT_NE (dg::ContentSignature (moved), base);
}
