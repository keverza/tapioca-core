// Every building floor planned off the UI thread among its neighbours (FloorPlanner.hpp): a
// building's own silhouette, blind walls against the others, stairs that stack, newer inputs
// replacing older ones.
#include "ArchViz/FloorPlanner.hpp"
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
