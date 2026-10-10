// Every building floor planned off the UI thread among its neighbours (FloorPlanner.hpp): a
// building's own silhouette, blind walls against the others, stairs that stack, newer inputs
// replacing older ones.
#include "ArchViz/FloorPlanner.hpp"
#include "NodeGraph/Json.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace bp = geomsrv::archviz::buildingplan;
namespace fs = geomsrv::archviz::floorscheme;
namespace fe = geomsrv::archviz::floorscheme::edit;
namespace fp = geomsrv::archviz::floorprogramme;

namespace {
geomsrv::archviz::SliceChain Chain (double x0, double y0, double x1, double y1)
{
    geomsrv::archviz::SliceChain c;
    c.closed = true;
    c.xy = { x0, y0, x1, y0, x1, y1, x0, y1 };
    return c;
}
bp::Plan Building (const char* key, double x0, double x1, int floors = 1)
{
    bp::Plan plan;
    plan.key = key;
    for (int k = 0; k < floors; ++k) {
        bp::Floor floor;
        floor.story = k;
        floor.z = 3.0 * k;
        floor.height = 3;
        floor.areaM2 = (x1 - x0) * 16;
        floor.outlineKnown = true;
        floor.outline = { Chain (x0, 0, x1, 16) };
        floor.contours = { floor.outline };
        floor.outlineKey = std::string (key) + "-outline";
        plan.floors.push_back (floor);
    }
    return plan;
}
// Polls until nothing is waiting (at most two seconds).
void Settle (bp::Planner& planner)
{
    for (int n = 0; n < 400; ++n) {
        planner.Poll ();
        if (!planner.Busy ())
            return;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}
double Area (const std::vector<fs::Ring>& rings)
{
    double a = 0;
    for (const auto& r : rings)
        a += fs::Area (r);
    return a;
}
} // namespace

TEST (FloorPlanner, ABuildingIsPlannedOnItsOwnFloorWithBlindWallsAgainstItsNeighbour)
{
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 36) }, { "B", Building ("B", 36, 60) } };
    const std::map<std::string, bp::Draft> drafts;
    const auto input = bp::InputFor (plans, drafts, "A", plans.at ("A").floors[0], fp::Default ());
    EXPECT_NEAR (Area (input.owned), 576, 1e-6);
    EXPECT_NEAR (Area (input.party), 384, 1e-6);
    bp::Planner planner;
    EXPECT_EQ (bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0), nullptr) << "nothing planned yet";
    Settle (planner); // the stairs, on the floor planned free
    bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0);
    Settle (planner);
    const auto* planned = bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0);
    ASSERT_NE (planned, nullptr);
    EXPECT_NEAR (planned->scheme.gross, 576, 1.0);
    ASSERT_EQ (planned->scheme.party.size (), 1u);
    EXPECT_NEAR (planned->scheme.party[0][0].x, 36, 1e-6);
    EXPECT_FALSE (planner.Busy ()) << "the same input is not planned again";
}

TEST (FloorPlanner, FloorsWithoutSavedStairsTakeTheLargestFloorsStairs)
{
    std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 60, 3) } };
    plans["A"].floors[1].areaM2 += 1; // the lead floor: the middle one
    const std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    for (int round = 0; round < 2; ++round) { // the lead floor's free plan first, then every floor
        bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0);
        Settle (planner);
    }
    const auto* lead = planner.Latest (bp::FloorId ("A", 1));
    ASSERT_NE (lead, nullptr);
    ASSERT_FALSE (lead->scheme.cores.empty ());
    for (int story : { 0, 2 }) {
        const auto* other = planner.Latest (bp::FloorId ("A", story));
        ASSERT_NE (other, nullptr);
        ASSERT_EQ (other->scheme.cores.size (), lead->scheme.cores.size ()) << story;
        for (size_t i = 0; i < lead->scheme.cores.size (); ++i)
            EXPECT_LT (std::hypot (other->scheme.cores[i].centre.x - lead->scheme.cores[i].centre.x,
                                   other->scheme.cores[i].centre.y - lead->scheme.cores[i].centre.y),
                       0.1)
                << story;
    }
}

// User, 2026-10-10: "stair core must be same position from ground to top floor". A setback top
// floor shares only part of the floors below: the stairs are planned there and held on all.
TEST (FloorPlanner, StairsStandAtOnePlaceFromTheGroundToATopFloorSetBack)
{
    std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 60, 3) } };
    auto& top = plans["A"].floors[2];
    top.outline = { Chain (0, 0, 30, 16) };
    top.contours = { top.outline };
    top.areaM2 = 30 * 16;
    top.outlineKey = "A-setback";
    const std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    for (int round = 0; round < 2; ++round) {
        bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0);
        Settle (planner);
    }
    const auto* ground = planner.Latest (bp::FloorId ("A", 0));
    ASSERT_NE (ground, nullptr);
    ASSERT_FALSE (ground->scheme.cores.empty ());
    for (int story : { 0, 1, 2 }) {
        const auto* floor = planner.Latest (bp::FloorId ("A", story));
        ASSERT_NE (floor, nullptr) << story;
        ASSERT_EQ (floor->scheme.cores.size (), ground->scheme.cores.size ()) << story;
        for (size_t i = 0; i < floor->scheme.cores.size (); ++i) {
            EXPECT_NEAR (floor->scheme.cores[i].centre.x, ground->scheme.cores[i].centre.x, 1e-6) << story;
            EXPECT_NEAR (floor->scheme.cores[i].centre.y, ground->scheme.cores[i].centre.y, 1e-6) << story;
            EXPECT_LE (floor->scheme.cores[i].centre.x, 30.0) << "inside the top floor";
        }
        for (const auto& d : floor->scheme.diagnostics)
            EXPECT_FALSE (d.level == fs::Diagnostic::Error && d.code.rfind ("core.", 0) == 0)
                << story << ": " << d.code << " " << d.text;
    }
}

TEST (FloorPlanner, AChangedDesignIsPlannedAgainAndTheOldSchemeShowsMeanwhile)
{
    const std::map<std::string, bp::Plan> plans { { "A", Building ("A", 0, 60) } };
    std::map<std::string, bp::Draft> drafts;
    bp::Planner planner;
    for (int round = 0; round < 2; ++round) {
        bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0);
        Settle (planner);
    }
    const auto* before = planner.Latest (bp::FloorId ("A", 0));
    ASSERT_NE (before, nullptr);
    const size_t flats = before->scheme.flats.size ();
    const uint64_t revision = before->revision;
    drafts["A"].designs.floors[0] = fe::SetCount (before->scheme, {}, int (flats) + 1);
    const auto* meanwhile = bp::WantFloors (planner, plans, drafts, "A", fp::Default (), 0);
    ASSERT_NE (meanwhile, nullptr);
    EXPECT_EQ (meanwhile->revision, revision) << "the older scheme while the new one is planned";
    Settle (planner);
    const auto* after = planner.Latest (bp::FloorId ("A", 0));
    EXPECT_GT (after->revision, revision);
    EXPECT_EQ (after->scheme.flats.size (), flats + 1);
}

TEST (FloorPlanner, ExportIsAStorySlicesFixtureWithTheStairsProgrammeAndEveryFloorsScheme)
{
    std::map<std::string, bp::Plan> plans { { "building:Tower", Building ("building:Tower", 0, 36, 3) } };
    auto& plan = plans["building:Tower"];
    plan.floors[2].outline = { Chain (0, 0, 36, 36), Chain (12, 12, 24, 24) };
    plan.floors[2].outline[1].xy = { 12, 12, 12, 24, 24, 24, 24, 12 }; // the courtyard, clockwise
    plan.floors[2].contours = { plan.floors[2].outline };
    plan.floors[2].outlineKey = "courtyard";
    plan.saved = { { { 6, 8 } } };
    std::map<std::string, bp::Draft> drafts;
    bp::Reset (plan, drafts["building:Tower"]);
    bp::Planner planner;
    for (int round = 0; round < 2; ++round) {
        bp::WantFloors (planner, plans, drafts, "building:Tower", fp::Default (), 0);
        Settle (planner);
    }
    std::map<int, const fs::Scheme*> schemes;
    for (const auto& floor : plan.floors)
        if (const auto* planned = planner.Latest (bp::FloorId ("building:Tower", floor.story)))
            schemes[floor.story] = &planned->scheme;
    ASSERT_EQ (schemes.size (), 3u);
    const auto file = bp::ExportPlan (plan, drafts["building:Tower"], plan.floors[0], "20261010-120000", schemes);
    EXPECT_EQ (file.name, "Tower-floor0-20261010-120000.json");
    namespace js = evp::nodegraph::json;
    const auto parsed = js::Parse (file.text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    const auto& doc = parsed.value;
    std::string text;
    ASSERT_TRUE (doc.Find ("format")->AsString (text));
    EXPECT_EQ (text, "tapioca.story-slices.2d");
    const auto* slices = doc.Find ("slices")->AsArray ();
    ASSERT_EQ (slices->size (), 3u) << "one slice per floor ring";
    EXPECT_EQ (slices->at (2).Find ("holes")->AsArray ()->size (), 1u) << "the courtyard stays a hole";
    const auto* exported = doc.Find ("plan");
    ASSERT_NE (exported, nullptr);
    EXPECT_EQ (exported->Find ("cores")->AsArray ()->size (), 1u);
    const auto* designs = exported->Find ("designs")->AsArray ();
    ASSERT_EQ (designs->size (), 3u) << "every floor's scheme";
    const auto* flats = designs->front ().Find ("flats")->AsArray ();
    ASSERT_EQ (flats->size (), schemes[0]->flats.size ());
    for (const auto& flat : *flats) {
        double net = 0;
        ASSERT_TRUE (flat.Find ("netM2")->AsDouble (net));
        EXPECT_GT (net, 0);
    }
}
