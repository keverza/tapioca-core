// The floors' schemes on the overlay (OverlayHudFloorPlans.cpp): every building floor planned off
// the UI thread and drawn once it arrives -- each stack stair as one shaft from the lowest floor to
// under the top floor's slab, flats and circulation inside their walls, party walls -- published
// only when something changed, kept through deselection.
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <set>
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
        floor.guids = { "slab-" + std::to_string (k) };
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

TEST (OverlayFloorPlans, ProgrammeColourEditsRepaintImmediatelyWithoutRegeneratingFloors)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewStairs = state->previewUnits = true;
    layers::Layer stairs, units;
    ASSERT_TRUE (Planned (*state, stairs, units, 1));
    const auto revision = state->floorPlanner.Revision ();
    for (size_t i = 0; i < state->massingProgramme.types.size (); ++i)
        ASSERT_TRUE (fp::SetColour (state->massingProgramme, i, 0x123456FFu));
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (std::any_of (units.polylines.begin (), units.polylines.end (),
                              [] (const auto& line) { return line.rgba == 0x123456FFu; }));
    EXPECT_FALSE (state->floorPlanner.Busy ());
    EXPECT_EQ (state->floorPlanner.Revision (), revision);
    EXPECT_FALSE (hud::TakeFloorPlanLayers (*state, stairs, units)) << "One colour edit, one republish";
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

// User, 2026-10-14: "Floor Plan overlay show all stories. units are visible but needs to solid fill
// not just outline." On the floor plan, the units are the floor at the plan's storey, filled under
// their outlines; 3D keeps every floor's outlines.
TEST (OverlayFloorPlans, ThePlanShowsItsStoreysUnitsFilledAnd3DShowsEveryFloor)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewUnits = true;
    layers::Layer stairs, units, onPlan;
    ASSERT_TRUE (Planned (*state, stairs, units, 0));
    const auto levels = [] (const layers::Layer& layer) {
        std::set<long long> out;
        for (const auto& line : layer.polylines)
            for (size_t i = 2; i < line.points.size (); i += 3)
                out.insert (std::llround (line.points[i] * 10));
        for (const auto& mesh : layer.meshes)
            for (size_t i = 2; i < mesh.points.size (); i += 3)
                out.insert (std::llround (mesh.points[i] * 10));
        return out;
    };
    hud::SetPlanStorey (*state, 1);
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    EXPECT_EQ (onPlan.name, bp::kPlanUnitsLayer);
    EXPECT_EQ (onPlan.views, layers::Views::TwoD);
    EXPECT_EQ (units.views, layers::Views::ThreeD);
    EXPECT_EQ (levels (onPlan), (std::set<long long> { 30 })) << "the plan's storey alone, at 3 m";
    EXPECT_EQ (levels (units).size (), 3u) << "3D: every floor";
    ASSERT_FALSE (onPlan.meshes.empty ()) << "flats filled";
    for (const auto& mesh : onPlan.meshes)
        EXPECT_EQ (mesh.rgba & 0xFFu, 0x59u) << "a fill shows the plan through it";
    EXPECT_TRUE (layers::Validate (onPlan).empty ());
    // Another storey on the plan: its floor instead.
    hud::SetPlanStorey (*state, 2);
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    EXPECT_EQ (levels (onPlan), (std::set<long long> { 60 }));
    // The plan overlay not running: nothing on the plan.
    hud::SetPlanStorey (*state, INT_MIN);
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    EXPECT_TRUE (onPlan.polylines.empty () && onPlan.meshes.empty ());
}

// User, 2026-10-14: "Deselected with escape slab loses ability to edit in plan overlay and have to
// exit the locked mode to reselect." The floors come from the selected massing; locked, Archicad
// cannot be clicked to select again, so a deselection keeps the floors and their designs. Unlocked,
// a deselection that brings no floors leaves none, as before.
TEST (OverlayFloorPlans, LockedADeselectionKeepsTheFloorsBeingEdited)
{
    hudtest::Watched gui;
    hud::OwnPages selected;
    const auto plan = Tower ();
    selected.floorPlansKnown = true;
    selected.floorPlans = { plan };
    selected.selection.known = true;
    selected.selection.count = 1;
    gui.engine.SetOwnPages (selected);
    gui.state->buildingPlans[plan.key].designs.unique = { 1 };
    hud::SetEditLocked (*gui.state, true);
    hud::OwnPages none; // what the pages say once the slab is deselected: no massing, no floors
    none.selection.known = true;
    none.selection.count = 0;
    gui.engine.SetOwnPages (none);
    EXPECT_EQ (gui.state->floorPlanSnapshots.size (), 1u) << "locked: the floors stay to edit";
    EXPECT_EQ (gui.state->buildingPlans.at (plan.key).designs.unique.size (), 1u);
    // A new selection while locked (a building picked elsewhere) is taken as it comes.
    auto other = plan;
    other.key = "building:Other";
    selected.floorPlans = { other };
    gui.engine.SetOwnPages (selected);
    EXPECT_TRUE (gui.state->floorPlanSnapshots.contains (other.key));
    // Unlocked, a deselection with no floors leaves none.
    hud::SetEditLocked (*gui.state, false);
    gui.engine.SetOwnPages (none);
    EXPECT_TRUE (gui.state->floorPlanSnapshots.empty ());
}

// User, 2026-10-14: "In 3d locked mode units are selectable, but should have solid transparent fill
// not just contour." In 3D the floor each building's Plan view shows -- the one edited -- is filled;
// the other floors stay outlines.
TEST (OverlayFloorPlans, In3DTheEditedFloorIsFilled)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewUnits = true;
    layers::Layer stairs, units, onPlan;
    ASSERT_TRUE (Planned (*state, stairs, units, 0));
    const auto filled = [] (const layers::Layer& layer) {
        std::set<long long> out;
        for (const auto& mesh : layer.meshes)
            for (size_t i = 2; i < mesh.points.size (); i += 3)
                out.insert (std::llround (mesh.points[i] * 10));
        return out;
    };
    for (const int story : { 2, 0 }) {
        state->buildingPlans[plan.key].story = story; // picked in the building section
        ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan)) << story;
        EXPECT_EQ (filled (units), (std::set<long long> { story * 30 })) << "only the edited floor";
        for (const auto& mesh : units.meshes)
            EXPECT_EQ (mesh.rgba & 0xFFu, 0x59u) << "transparent";
    }
}

// User, 2026-10-15: "Selecting a unit in overlay locked mode open up selection tab same as if
// selecting slab"; later: "still only switched tab, but does not open editing options for the
// slab." A flat clicked on the locked plan opens the HUD on the Selection tab, where its building's
// plan shows it selected, and asks Archicad to select its floor's slabs -- the tab then edits them.
TEST (OverlayFloorPlans, AFlatPickedOnTheLockedPlanOpensTheSelectionTab)
{
    hudtest::Watched gui;
    hud::OwnPages own;
    own.standalone = true;
    own.floorPlansKnown = true;
    own.floorPlans = { Tower () };
    own.selection.known = true;
    own.selection.count = 1;
    gui.engine.SetOwnPages (own);
    hud::SetEditLocked (*gui.state, true);
    hud::SetHudOpen (*gui.state, false);
    hud::SelectKey (*gui.state, hud::kStatsKey);
    gui.state->previewUnits = true;
    layers::Layer stairs, units;
    ASSERT_TRUE (Planned (*gui.state, stairs, units, 0));
    // The plan: 10 pixels a metre, y up, its origin at (100, 600).
    const auto at = [] (float x, float y, std::vector<hud::Input::Button> buttons = {}) {
        auto input = hudtest::At (100 + 10 * x, 600 - 10 * y, std::move (buttons));
        input.planar = true;
        const double m[6] = { 10, 0, 100, 0, -10, 600 };
        std::copy (std::begin (m), std::end (m), input.plan);
        return input;
    };
    const auto* planned = gui.state->floorPlanner.Latest (bp::FloorId ("building:Tower", 0));
    ASSERT_NE (planned, nullptr);
    ASSERT_FALSE (planned->scheme.flats.empty ());
    double x = 0, y = 0;
    for (const auto& p : planned->scheme.flats.front ().shape)
        x += p.x / planned->scheme.flats.front ().shape.size (),
            y += p.y / planned->scheme.flats.front ().shape.size ();
    for (int n = 0; n < 3; ++n)
        gui.Lay ({}, at (float (x), float (y)));
    EXPECT_NE (hud::SelectedKey (*gui.state), hud::kSelectionKey) << "hovering picks nothing";
    gui.Lay ({}, at (float (x), float (y), { { 0, true } }));
    gui.Lay ({}, at (float (x), float (y), { { 0, false } }));
    EXPECT_EQ (hud::SelectedKey (*gui.state), hud::kSelectionKey);
    EXPECT_TRUE (hud::HudOpen (*gui.state));
    const auto asked = std::count_if (gui.heard.begin (), gui.heard.end (), [] (const hud::Change& c) {
        return c.kind == "selectSlabs" && c.text == "slab-0";
    });
    EXPECT_EQ (asked, 1) << "its floor's slab selected in Archicad, once";
}

// User, 2026-10-15: "Selected slabs story and actual archicad story should be linked." Archicad's
// storey changing on the plan turns every building with a floor there to it; a building with none
// there keeps its own.
TEST (OverlayFloorPlans, ArchicadsStoreyIsTheFloorEveryBuildingShows)
{
    auto state = hud::NewState ();
    const auto tower = Tower ();
    auto low = Tower ();
    low.key = "building:Low";
    low.floors.resize (1);
    state->floorPlanSnapshots[tower.key] = tower;
    state->floorPlanSnapshots[low.key] = low;
    state->buildingPlans[low.key].story = 0;
    hud::SetPlanStorey (*state, 2);
    EXPECT_EQ (state->buildingPlans[tower.key].story, 2);
    EXPECT_EQ (state->buildingPlans[low.key].story, 0) << "no floor on storey 2";
    // Picked in the section afterwards, the floor holds until Archicad's storey changes again.
    state->buildingPlans[tower.key].story = 1;
    hud::SetPlanStorey (*state, 2);
    EXPECT_EQ (state->buildingPlans[tower.key].story, 1);
    hud::SetPlanStorey (*state, 0);
    EXPECT_EQ (state->buildingPlans[tower.key].story, 0);
    EXPECT_EQ (state->buildingPlans[low.key].story, 0);
}

// User, 2026-10-15: "if zoomed in in overlay show segment length alligned to line like we have for
// area display"; later: "for whole floor of units not just one selection ... Text label should be
// black - same color as are label." With a flat selected, every flat of its floor carries its
// sides' lengths on the plan and in 3D: black, lying on the floor along each side, hidden until
// zoomed in.
TEST (OverlayFloorPlans, TheSelectedFloorsFlatsCarryTheirSidesLengthsWhenZoomedIn)
{
    namespace hf = geomsrv::archviz::hudfloorscheme;
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewUnits = true;
    layers::Layer stairs, units, onPlan;
    ASSERT_TRUE (Planned (*state, stairs, units, 0));
    const auto* planned = state->floorPlanner.Latest (bp::FloorId (plan.key, 0));
    ASSERT_NE (planned, nullptr);
    auto& editor = state->planEditors[plan.key];
    editor.reset (new hf::Editor);
    editor->key = plan.key;
    editor->story = 0;
    editor->scheme = planned->scheme;
    editor->selected = { hf::Kind::Flat, 0, {}, {} };
    hud::SetPlanStorey (*state, 0);
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    ASSERT_GE (planned->scheme.flats.size (), 2u);
    ASSERT_GE (onPlan.texts.size (), 4 * planned->scheme.flats.size ()) << "a length on each side of every flat";
    for (const auto& t : onPlan.texts) {
        EXPECT_EQ (t.rgba, 0x202020FFu) << "black, as the areas are";
        EXPECT_TRUE (t.planar);
        EXPECT_GT (t.minProjectedPixels, 0) << "only zoomed in";
        EXPECT_NEAR (t.at[2], 0.05, 1e-9) << "on its floor";
        EXPECT_GT (std::stod (t.text), 0.6);
        EXPECT_GE (t.direction[0], -1e-9) << "reads left to right";
    }
    EXPECT_EQ (units.texts.size (), onPlan.texts.size ()) << "and in 3D";
    EXPECT_TRUE (layers::Validate (onPlan).empty ());
    // Nothing selected: no lengths.
    editor->selected = {};
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    EXPECT_TRUE (onPlan.texts.empty ());
}

// User, 2026-10-15: "GridLines may be shown in overlay through show toggle in Massing Tab." With
// the toggle on, the plan shows each main wall's axis and number at its storey, and 3D each main wall
// standing from the lowest floor to the top; off, nothing of them.
TEST (OverlayFloorPlans, TheMainWallsShowOnTheOverlayWhenToggled)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewGrid = true;
    layers::Layer stairs, units, onPlan;
    ASSERT_TRUE (Planned (*state, stairs, units, 0));
    hud::SetPlanStorey (*state, 1);
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    const uint32_t grid = 0xC44A40E0u;
    size_t axes = 0, frames = 0;
    for (const auto& line : onPlan.polylines)
        if (line.rgba == grid && !line.closed) {
            ++axes;
            EXPECT_NEAR (line.points[2], 3.06, 1e-9) << "at the plan's storey";
        }
    for (const auto& line : units.polylines)
        if (line.rgba == grid) {
            ++frames;
            ASSERT_EQ (line.points.size (), 12u);
            EXPECT_NEAR (line.points[2], 0, 1e-9) << "from the lowest floor";
            EXPECT_NEAR (line.points[8], 9, 1e-9) << "to the top";
        }
    EXPECT_GE (axes, 1u);
    EXPECT_EQ (frames, axes) << "one standing wall per axis";
    EXPECT_EQ (onPlan.texts.size (), axes) << "each numbered";
    EXPECT_EQ (onPlan.texts.front ().text, "1");
    EXPECT_TRUE (layers::Validate (onPlan).empty ());
    EXPECT_TRUE (layers::Validate (units).empty ());
    state->previewGrid = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    for (const auto* layer : { &onPlan, &units })
        for (const auto& line : layer->polylines)
            EXPECT_NE (line.rgba, grid);
}
