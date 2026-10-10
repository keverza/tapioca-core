// The floors' schemes on the overlay (OverlayHudFloorPlans.cpp): every building floor planned off
// the UI thread and drawn once it arrives -- stairs as boxes a floor high, flats, circulation and
// party walls -- published only when something changed, kept through deselection.
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
// Layers taken until a call publishes stairs on every floor (at most two seconds).
bool Planned (hud::State& state, layers::Layer& stairs, layers::Layer& units, size_t floors)
{
    for (int n = 0; n < 400; ++n) {
        hud::TakeFloorPlanLayers (state, stairs, units);
        if (stairs.meshes.size () == floors && !state.floorPlanner.Busy ())
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
    ASSERT_TRUE (Planned (*state, stairs, units, 3));
    EXPECT_EQ (stairs.name, bp::kStairsLayer);
    EXPECT_EQ (units.name, bp::kUnitsLayer);
    for (size_t i = 0; i < stairs.meshes.size (); ++i) {
        EXPECT_DOUBLE_EQ (stairs.meshes[i].points[2], double (i) * 3);
        EXPECT_DOUBLE_EQ (stairs.meshes[i].points[14], double (i + 1) * 3);
        EXPECT_EQ (stairs.meshes[i].indices.size (), 36u);
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
    ASSERT_TRUE (Planned (*gui.state, stairs, units, 3));
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
