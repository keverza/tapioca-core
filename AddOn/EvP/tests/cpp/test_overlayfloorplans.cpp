// The floors' schemes on the overlay (OverlayHudFloorPlans.cpp): every building floor planned off
// the UI thread and drawn once it arrives -- each stack stair as one shaft from the lowest floor to
// under the top floor's slab, flats and circulation inside their walls, party walls -- published
// only when something changed, kept through deselection.
#include "ArchViz/OverlayHudEngine.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace bp = geomsrv::archviz::buildingplan;
namespace fp = geomsrv::archviz::floorprogramme;
namespace layers = geomsrv::archviz::overlaylayers;
namespace hud = geomsrv::archviz::overlayhud;

namespace {
geomsrv::archviz::SliceChain Chain (double x0, double y0, double x1, double y1)
{
    geomsrv::archviz::SliceChain c;
    c.closed = true;
    c.xy = { x0, y0, x1, y0, x1, y1, x0, y1 };
    return c;
}
// A tower of three 36 x 16 floors with one saved stair.
bp::Plan Tower ()
{
    bp::Plan plan;
    plan.key = "building:Tower";
    for (int k = 0; k < 3; ++k) {
        bp::Floor floor;
        floor.story = k;
        floor.z = 3.0 * k;
        floor.height = 3;
        floor.areaM2 = 576;
        floor.contours = { { Chain (0, 0, 36, 16) } };
        floor.outlineKey = "tower";
        plan.floors.push_back (floor);
    }
    plan.saved = { { { 6, 8 } } };
    return plan;
}
// Layers taken until every floor is planned and `shafts` stairs are published (at most two seconds).
bool Planned (hud::State& state, layers::Layer& stairs, layers::Layer& units, size_t shafts)
{
    for (int n = 0; n < 400; ++n) {
        hud::TakeFloorPlanLayers (state, stairs, units);
        bool all = true;
        for (const auto& [key, plan] : state.floorPlanSnapshots)
            for (const auto& floor : plan.floors)
                all = all && state.floorPlanner.Latest (bp::FloorId (key, floor.story));
        if (all && stairs.meshes.size () == shafts && !state.floorPlanner.Busy ())
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    return false;
}
} // namespace

TEST (OverlayFloorPlans, EveryFloorsSchemeIsDrawnOnceItIsPlannedAndNotRebuiltUntilSomethingChanges)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewStairs = state->previewUnits = true;
    layers::Layer stairs, units;
    ASSERT_TRUE (Planned (*state, stairs, units, 1));
    EXPECT_EQ (stairs.name, bp::kStairsLayer);
    EXPECT_EQ (units.name, bp::kUnitsLayer);
    // The one stair, a shaft from the ground floor's level to under the top floor's 0.3 m slab,
    // 0.1 m inside its walls (a 4.5 m stair shows 4.3 m).
    const auto& shaft = stairs.meshes.front ();
    ASSERT_EQ (shaft.points.size (), 24u);
    EXPECT_DOUBLE_EQ (shaft.points[2], 0.0);
    EXPECT_NEAR (shaft.points[14], 3 * 3 - fp::kSlab, 1e-9);
    EXPECT_EQ (shaft.indices.size (), 36u);
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (size_t i = 0; i < shaft.points.size (); i += 3)
        x0 = (std::min) (x0, shaft.points[i]), x1 = (std::max) (x1, shaft.points[i]),
        y0 = (std::min) (y0, shaft.points[i + 1]), y1 = (std::max) (y1, shaft.points[i + 1]);
    const double w = x1 - x0, d = y1 - y0;
    EXPECT_NEAR ((std::min) (w, d), 4.5 - 0.2, 1e-3) << "inside the stair's walls, not on their centre lines";
    // Flats inside their walls: no outline reaches the massing's edge (the facade is 0.5 m thick).
    for (const auto& line : units.polylines)
        for (size_t i = 0; i < line.points.size (); i += 3) {
            EXPECT_GE (line.points[i], 0.5 - 1e-6);
            EXPECT_LE (line.points[i], 36 - 0.5 + 1e-6);
            EXPECT_GE (line.points[i + 1], 0.5 - 1e-6);
            EXPECT_LE (line.points[i + 1], 16 - 0.5 + 1e-6);
        }
    EXPECT_TRUE (layers::Validate (stairs).empty ());
    EXPECT_GT (units.polylines.size (), 3u);
    EXPECT_FALSE (hud::TakeFloorPlanLayers (*state, stairs, units)) << "nothing changed: nothing published";
    // A programme edit on the Massing tab reaches every building's floors.
    ASSERT_TRUE (fp::SetShare (state->massingProgramme, 1, 0.5));
    bool republished = false;
    for (int n = 0; n < 400 && !republished; ++n) {
        republished = hud::TakeFloorPlanLayers (*state, stairs, units);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    EXPECT_TRUE (republished);
    EXPECT_EQ (state->buildingPlans.at (plan.key).programme, state->massingProgramme);
    state->previewStairs = state->previewUnits = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (stairs.meshes.empty ());
    EXPECT_TRUE (units.polylines.empty ());
}

TEST (OverlayFloorPlans, DesignsSurviveDeselectionButClearWhenTheBuildingGoes)
{
    hudtest::Watched gui;
    hud::OwnPages pages;
    const auto plan = Tower ();
    pages.floorPlansKnown = true;
    pages.floorPlans = { plan };
    gui.engine.SetOwnPages (pages);
    gui.state->previewStairs = true;
    layers::Layer stairs, units;
    ASSERT_TRUE (Planned (*gui.state, stairs, units, 1));
    auto& draft = gui.state->buildingPlans.at (plan.key);
    draft.designs.unique = { 1 };
    pages.selection.known = true;
    pages.selection.count = 0;
    gui.engine.SetOwnPages (pages);
    EXPECT_EQ (gui.state->floorPlanSnapshots.size (), 1u);
    EXPECT_EQ (gui.state->buildingPlans.at (plan.key).designs.unique.size (), 1u) << "kept through deselection";
    pages.floorPlans.clear ();
    gui.engine.SetOwnPages (pages);
    EXPECT_TRUE (hud::TakeFloorPlanLayers (*gui.state, stairs, units));
    EXPECT_TRUE (stairs.meshes.empty ());
    EXPECT_TRUE (gui.state->buildingPlans.empty ());
}

TEST (OverlayFloorPlans, ProgrammeFromTheProjectIsAdoptedUnlessAnUnsavedEditIsWaiting)
{
    hudtest::Watched gui;
    hud::OwnPages pages;
    pages.massing.known = true;
    auto stored = fp::Default ();
    ASSERT_TRUE (fp::SetShare (stored, 1, 0.5));
    pages.massing.programmeStored = true;
    pages.massing.programme = stored;
    gui.engine.SetOwnPages (pages);
    EXPECT_EQ (gui.state->massingProgramme, stored);
    EXPECT_EQ (gui.state->massingProgrammeSaved, stored);
    // A typed answer is applied and queued for the owner to store, once.
    hud::State& state = *gui.state;
    std::string error;
    ASSERT_TRUE (hud::AnswerProgrammeText (state, { state.massingProgramme, -1, {} },
                                           "50% 2 room 40-45m2; 50% 3 room 60-70m2", error))
        << error;
    fp::Programme queued;
    ASSERT_TRUE (hud::TakeProgrammeSave (state, queued));
    EXPECT_EQ (queued, state.massingProgramme);
    EXPECT_FALSE (hud::TakeProgrammeSave (state, queued));
    // Pages read before the write still carry the old programme: the edit is kept.
    gui.engine.SetOwnPages (pages);
    EXPECT_EQ (state.massingProgramme, queued);
    // The write acknowledged: saved and shown agree again.
    pages.massing.programme = queued;
    gui.engine.SetOwnPages (pages);
    EXPECT_EQ (state.massingProgrammeSaved, queued);
    EXPECT_EQ (state.massingProgramme, queued);
    // Another project (or an Undo seen on reread) with no edit pending is followed.
    pages.massing.programmeStored = false;
    pages.massing.programme = fp::Default ();
    gui.engine.SetOwnPages (pages);
    EXPECT_EQ (state.massingProgramme, fp::Default ());
}
