// #201 M2: the Massing workflow's planned floors at the diagram's Units and Rooms detail.
#include "ArchViz/DiagramPublication.hpp"
#include "ArchViz/DiagramReplay.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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
    return text.str ();
}
dg::PlannedSpace Rect (uint64_t id, double x0, double y0, double x1, double y1, uint32_t rgba)
{
    return { id, { { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } }, rgba };
}
// Building A's ground floor (the fixture's L, 20 x 20 with an 8 m arm) planned as two flats
// and a corridor; its first floor (z 3) left unplanned.
std::shared_ptr<const dg::FloorPlans> Plans ()
{
    auto plans = std::make_shared<dg::FloorPlans> ();
    dg::PlannedFloor ground;
    ground.story = 0;
    ground.z = 0.02; // within the storey tolerance of the slices' 0
    ground.height = 3;
    ground.units = { Rect (1, 0.2, 0.2, 9.8, 7.8, 0xD9A441FFu), Rect (2, 10.2, 0.2, 19.8, 7.8, 0x7FA7C7FFu),
                     Rect (3, 0.2, 8.2, 7.8, 19.8, 0xEAD9A6FFu) };
    ground.units[1].label = "2-room flat";
    ground.units[1].area = 72.96;
    // The first flat as two rooms; the others whole.
    ground.rooms = { Rect (4, 0.2, 0.2, 4.0, 7.8, 0x5FAE9FFFu), Rect (5, 4.2, 0.2, 9.8, 7.8, 0xD9A441FFu),
                     ground.units[1], ground.units[2] };
    plans->buildings.push_back ({ "building:A", { ground } });
    plans->revision = 7;
    return plans;
}
dg::DiagramInputSnapshot Snapshot (std::shared_ptr<const dg::FloorPlans> plans)
{
    dg::CaptureInputs inputs;
    std::string error;
    EXPECT_TRUE (dg::ReadCapture (Fixture (), inputs, error)) << error;
    return dg::Capture (inputs.slices, inputs.storeys, inputs.revisions, std::move (plans));
}
const dg::PlanNode* StoreyAt (const dg::LayoutPlan& plan, const std::string& building, double z)
{
    for (const auto& node : plan.nodes)
        if (node.id.level == dg::Level::Storey && node.id.key.rfind ("storey:" + building, 0) == 0 &&
            std::abs (node.boundsMin[2] - z) < 1e-6)
            return &node;
    return nullptr;
}
} // namespace

TEST (DiagramPlans, APlannedStoreyCarriesUnitsAndRoomsInItsShellsFrame)
{
    const auto prepared = dg::Prepare (Snapshot (Plans ()), {}, dg::DefaultRequest (), 1);
    ASSERT_TRUE (prepared.publication);
    const auto& p = *prepared.publication;
    EXPECT_EQ (p.storeys, 4u);
    EXPECT_EQ (p.plannedStoreys, 1u);
    const auto* ground = StoreyAt (p.plan, "building:A", 0);
    const auto* first = StoreyAt (p.plan, "building:A", 3);
    ASSERT_TRUE (ground && first);
    const auto& summary = p.storeyInfo.at (ground->id);
    EXPECT_TRUE (summary.planned);
    EXPECT_EQ (summary.units, 3u);
    EXPECT_EQ (summary.rooms, 4u);
    EXPECT_FALSE (p.storeyInfo.at (first->id).planned);

    EXPECT_EQ (p.geometry.Of (ground->id, dg::Detail::Topology).count, 1u);
    const auto units = p.geometry.Of (ground->id, dg::Detail::Units);
    const auto rooms = p.geometry.Of (ground->id, dg::Detail::Rooms);
    ASSERT_EQ (units.count, 3u); // one mesh per flat: each its own pick and highlight
    ASSERT_EQ (rooms.count, 4u);
    EXPECT_EQ (p.geometry.Of (first->id, dg::Detail::Units).count, 0u);
    // Node-local like the shell: world = vertex + pivot, inside the storey, under its top.
    std::set<uint32_t> colours;
    for (uint32_t m = units.first; m < units.first + units.count; ++m) {
        const auto& mesh = p.geometry.meshes[m];
        EXPECT_EQ (mesh.node, ground->id);
        EXPECT_EQ (mesh.id.level, dg::Level::Apartment);
        EXPECT_EQ (mesh.indexCount, 36u); // a box: 12 triangles
        EXPECT_GT (mesh.lineCount, 0u);   // its own rim
        for (uint32_t i = mesh.firstIndex; i < mesh.firstIndex + mesh.indexCount; ++i) {
            const auto& v = p.geometry.vertices[p.geometry.indices[i]];
            const double x = v.position[0] + ground->pivot[0], y = v.position[1] + ground->pivot[1],
                         z = v.position[2] + ground->pivot[2];
            EXPECT_GE (x, 0.2 - 1e-4);
            EXPECT_LE (x, 19.8 + 1e-4);
            EXPECT_GE (y, 0.2 - 1e-4);
            EXPECT_GE (z, 0.02 - 1e-4);
            EXPECT_LE (z, 3.02 - dg::kDetailSlabGap + 1e-4);
            colours.insert (v.rgba);
        }
    }
    EXPECT_EQ (colours.size (), 3u); // one per flat, the programme's colour kept
    EXPECT_EQ (p.spaceInfo.size (), 7u);
    const auto& flat = p.spaceInfo.at (dg::SpaceId (ground->id, dg::Detail::Units, 2));
    EXPECT_EQ (flat.storey, ground->id);
    EXPECT_EQ (flat.label, "2-room flat");
    EXPECT_NEAR (flat.area, 72.96, 1e-6);
}

// User, 2026-11-19: "in topology selects slabs, in units selects units, in rooms selects rooms".
TEST (DiagramPlans, AClickSelectsWhatTheDetailShows)
{
    const auto prepared = dg::Prepare (Snapshot (Plans ()), {}, dg::DefaultRequest (), 1);
    ASSERT_TRUE (prepared.publication);
    const auto& p = *prepared.publication;
    const auto plan = dg::Spaced (p.plan, { 0.0 });
    const auto frame = dg::Evaluate (plan, {}, 1.0);
    const auto* ground = StoreyAt (p.plan, "building:A", 0);
    const auto* first = StoreyAt (p.plan, "building:A", 3);
    ASSERT_TRUE (ground && first);
    // Straight down onto the ground floor's first flat (x 0.2..9.8, y 0.2..7.8): through the
    // first floor above it, which is unplanned and so its shell takes the click first.
    const auto pick = [&] (dg::Detail detail, double z) {
        return dg::PickShown (frame, plan, p.geometry, p.shells, p.details, detail, { 2.0, 2.0, z }, { 0, 0, -1 });
    };
    auto hit = pick (dg::Detail::Units, 50);
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id.level, dg::Level::Storey); // the top of the stack is a shell
    // From inside the ground storey's height, under the first floor: the flat itself.
    hit = pick (dg::Detail::Units, 2.9);
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id, dg::SpaceId (ground->id, dg::Detail::Units, 1));
    hit = pick (dg::Detail::Rooms, 2.9);
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id, dg::SpaceId (ground->id, dg::Detail::Rooms, 4)); // the flat's first room
    hit = pick (dg::Detail::Topology, 2.9);
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id, ground->id);

    // A selected flat highlights alone; a selected storey highlights all it shows.
    const auto flat = dg::SpaceId (ground->id, dg::Detail::Units, 1);
    size_t lit = 0;
    for (const auto& draw : dg::DrawsOf (p.geometry, plan, dg::Evaluate (plan, { flat }, 1.0), {}, dg::Detail::Units))
        lit += draw.selected;
    EXPECT_EQ (lit, 1u);
    lit = 0;
    for (const auto& draw :
         dg::DrawsOf (p.geometry, plan, dg::Evaluate (plan, { ground->id }, 1.0), {}, dg::Detail::Units))
        lit += draw.selected;
    EXPECT_EQ (lit, 3u);
}

TEST (DiagramPlans, DetailPicksTheBodyAndAnUnplannedStoreyKeepsItsShell)
{
    const auto prepared = dg::Prepare (Snapshot (Plans ()), {}, dg::DefaultRequest (), 1);
    ASSERT_TRUE (prepared.publication);
    const auto& p = *prepared.publication;
    const auto frame = dg::Evaluate (dg::Spaced (p.plan, { 3.0 }), {}, 1.0);
    const auto* ground = StoreyAt (p.plan, "building:A", 0);
    ASSERT_TRUE (ground);
    const auto meshOf = [&] (dg::Detail detail) {
        std::map<dg::EntityId, dg::Detail> out; // storey -> the detail it draws
        for (const auto& draw : dg::DrawsOf (p.geometry, p.plan, frame, {}, detail))
            out[p.geometry.meshes[draw.mesh].node] = p.geometry.meshes[draw.mesh].detail;
        return out;
    };
    for (const auto detail : { dg::Detail::Topology, dg::Detail::Units, dg::Detail::Rooms, dg::Detail::Walls }) {
        const auto shown = meshOf (detail);
        EXPECT_EQ (shown.size (), 4u); // every storey draws, planned or not
        for (const auto& [id, level] : shown)
            if (id != ground->id)
                EXPECT_EQ (level, dg::Detail::Topology);
    }
    EXPECT_EQ (meshOf (dg::Detail::Topology).at (ground->id), dg::Detail::Topology);
    EXPECT_EQ (meshOf (dg::Detail::Units).at (ground->id), dg::Detail::Units);
    EXPECT_EQ (meshOf (dg::Detail::Rooms).at (ground->id), dg::Detail::Rooms);
    EXPECT_EQ (meshOf (dg::Detail::Walls).at (ground->id), dg::Detail::Rooms); // not drawn yet: the nearest
    // The outlines follow the body shown; the corner lines stay on the shells.
    const auto lines = dg::LineDrawsOf (p.geometry, p.plan, frame, {}, true, false, dg::Detail::Units);
    const auto& units = p.geometry.meshes[size_t (p.geometry.Find (ground->id, dg::Detail::Units))];
    EXPECT_TRUE (std::any_of (lines.begin (), lines.end (),
                              [&] (const dg::DiagramLineDraw& d) { return d.firstLine == units.firstLine; }));
    EXPECT_EQ (p.geometry.connectors.size (), 2u);
}

TEST (DiagramPlans, PlansRoundTripAndMoveTheContentSignature)
{
    const auto without = Snapshot (nullptr);
    const auto with = Snapshot (Plans ());
    EXPECT_NE (dg::ContentSignature (without), dg::ContentSignature (with));
    // The planner's revision alone is a stamp, not content.
    auto bumped = std::make_shared<dg::FloorPlans> (*Plans ());
    bumped->revision = 99;
    auto restamped = Snapshot (bumped);
    restamped.revisions.topology = 99;
    EXPECT_EQ (dg::ContentSignature (with), dg::ContentSignature (restamped));

    const std::string text = dg::WriteCapture (with);
    dg::DiagramInputSnapshot again;
    std::string error;
    ASSERT_TRUE (dg::ReplayCapture (text, again, error)) << error;
    ASSERT_TRUE (again.plans);
    EXPECT_EQ (dg::WriteCapture (again), text);
    ASSERT_EQ (again.plans->buildings.size (), 1u);
    EXPECT_EQ (again.plans->buildings[0].floors[0].rooms.size (), 4u);
    EXPECT_EQ (again.plans->buildings[0].floors[0].units[1].rgba, 0x7FA7C7FFu);

    // A malformed plan refuses the whole file.
    auto broken = text;
    broken.replace (broken.find ("\"rings\""), 7, "\"ringz\"");
    dg::CaptureInputs inputs;
    EXPECT_FALSE (dg::ReadCapture (broken, inputs, error));
}

TEST (DiagramPlans, AFloorMatchesByElevationThenByStory)
{
    dg::FloorPlans plans;
    dg::PlannedFloor a, b;
    a.story = 0;
    a.z = 0;
    b.story = 1;
    b.z = 3.2;
    plans.buildings.push_back ({ "building:A", { a, b } });
    EXPECT_EQ (dg::PlannedFloorOf (&plans, "building:A", 5, 0.04, false), &plans.buildings[0].floors[0]);
    EXPECT_EQ (dg::PlannedFloorOf (&plans, "building:A", 1, 3.0, false), nullptr); // 0.2 m off, positional
    EXPECT_EQ (dg::PlannedFloorOf (&plans, "building:A", 1, 3.0, true), &plans.buildings[0].floors[1]);
    EXPECT_EQ (dg::PlannedFloorOf (&plans, "building:B", 0, 0, true), nullptr);
    EXPECT_EQ (dg::PlannedFloorOf (nullptr, "building:A", 0, 0, true), nullptr);
}
