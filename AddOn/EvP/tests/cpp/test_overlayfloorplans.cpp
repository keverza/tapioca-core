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
#include <map>
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

// User, 2026-10-15: "Replace Show units and Show main walls with Show Cell Complex ... Show proposed
// stair cores becomes only higlight stair core cell." Every floor's cells are solids from the floor to under the next
// slab, the stack's stair cells highlighted apart from the rest; published once each floor is planned, not again until
// something changed.
TEST (OverlayFloorPlans, EveryFloorsCellsAreDrawnOnceItIsPlannedAndNotRebuiltUntilSomethingChanges)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewStairs = state->previewCells = true;
    layers::Layer stairs, units;
    ASSERT_TRUE (Planned (*state, stairs, units, 1));
    EXPECT_EQ (stairs.name, bp::kStairsLayer);
    EXPECT_EQ (units.name, bp::kUnitsLayer);
    // The stair's cell on each floor, on its centre lines, from the floor's level to under its slab.
    const auto& cores = stairs.meshes.front ();
    ASSERT_EQ (cores.vertexRgba.size (), cores.points.size () / 3);
    for (const uint32_t rgba : cores.vertexRgba)
        EXPECT_EQ (rgba, 0x8E3B2FFFu) << "the inspector's stair colour";
    EXPECT_EQ (cores.style.edgeRgba, 0x2448C9FFu) << "highlighted";
    std::set<long long> levels;
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (size_t i = 0; i < cores.points.size (); i += 3) {
        levels.insert (std::llround (cores.points[i + 2] * 100));
        x0 = (std::min) (x0, cores.points[i]), x1 = (std::max) (x1, cores.points[i]);
        y0 = (std::min) (y0, cores.points[i + 1]), y1 = (std::max) (y1, cores.points[i + 1]);
    }
    EXPECT_EQ (levels, (std::set<long long> { 2, 270, 302, 570, 602, 870 })) << "three floors, each under its slab";
    EXPECT_NEAR ((std::min) (x1 - x0, y1 - y0), 4.5 - 0.2, 1e-3) << "inside its walls";
    // The rest: one lit solid mesh, each cell its space's colour, inside the massing.
    ASSERT_EQ (units.meshes.size (), 1u);
    const auto& cells = units.meshes.front ();
    EXPECT_EQ (cells.style.shading, layers::Shading::Lit);
    EXPECT_EQ (cells.style.edgeRgba, 0x1D242759u) << "ink edges at 35 %";
    EXPECT_GT (std::count (cells.vertexRgba.begin (), cells.vertexRgba.end (), 0xE4E6E6FFu), 0)
        << "circulation neutral beside the flats (unit mix)";
    EXPECT_EQ (std::count (cells.vertexRgba.begin (), cells.vertexRgba.end (), 0x8E3B2FFFu), 0)
        << "the stair is highlighted, not among them";
    for (size_t i = 0; i < cells.points.size (); i += 3) {
        EXPECT_GE (cells.points[i], -1e-6);
        EXPECT_LE (cells.points[i], 36 + 1e-6);
        EXPECT_GE (cells.points[i + 1], -1e-6);
        EXPECT_LE (cells.points[i + 1], 16 + 1e-6);
    }
    EXPECT_TRUE (layers::Validate (stairs).empty ());
    EXPECT_TRUE (layers::Validate (units).empty ());
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
    // The stairs alone: only their cells.
    state->previewCells = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_EQ (stairs.meshes.size (), 1u);
    EXPECT_TRUE (units.meshes.empty ());
    // Cells without the highlight: the stair among them, in its colour.
    state->previewCells = true;
    state->previewStairs = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (stairs.meshes.empty ());
    ASSERT_EQ (units.meshes.size (), 1u);
    const auto& all = units.meshes.front ().vertexRgba;
    EXPECT_GT (std::count (all.begin (), all.end (), 0xB8BCBEFFu), 0) << "neutral among the flats";
    state->previewCells = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (stairs.meshes.empty () && units.meshes.empty ());
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

// User, 2026-10-14: "Floor Plan overlay show all stories. units are visible but needs to solid fill
// not just outline." On the floor plan, the cells are the floor's at the plan's storey, filled under
// their outlines; 3D keeps every floor.
TEST (OverlayFloorPlans, ThePlanShowsItsStoreysCellsFilledAnd3DShowsEveryFloor)
{
    auto state = hud::NewState ();
    const auto plan = Tower ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewCells = true;
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
    EXPECT_EQ (stairs.views, layers::Views::ThreeD);
    EXPECT_EQ (levels (onPlan), (std::set<long long> { 30 })) << "the plan's storey alone, at 3 m";
    EXPECT_EQ (levels (units), (std::set<long long> { 0, 27, 30, 57, 60, 87, 90 }))
        << "3D: every floor, and its slab's wireframe to the top";
    EXPECT_TRUE (units.solid) << "its solids hide each other";
    ASSERT_FALSE (onPlan.meshes.empty ()) << "cells filled";
    for (const auto& mesh : onPlan.meshes)
        EXPECT_EQ (mesh.rgba & 0xFFu, 0x99u) << "a fill shows the plan through it";
    EXPECT_TRUE (layers::Validate (onPlan).empty ());
    // Another storey on the plan: its floor instead.
    hud::SetPlanStorey (*state, 2);
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    EXPECT_EQ (levels (onPlan), (std::set<long long> { 60 }));
    // The stairs highlighted: their cell on the plan stronger, in the accent's outline.
    state->previewStairs = true;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    EXPECT_TRUE (std::any_of (onPlan.polylines.begin (), onPlan.polylines.end (),
                              [] (const auto& line) { return line.rgba == 0x2448C9FFu; }));
    EXPECT_TRUE (std::any_of (onPlan.meshes.begin (), onPlan.meshes.end (),
                              [] (const auto& mesh) { return mesh.rgba == 0x8E3B2FE6u; }));
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
    gui.state->previewCells = true;
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
    // A flat selected in another building before: one selection at a time (user, 2026-10-16).
    namespace hf = geomsrv::archviz::hudfloorscheme;
    auto& other = gui.state->planEditors["building:Other"];
    other.reset (new hf::Editor);
    other->key = "building:Other";
    other->scheme = planned->scheme;
    other->selected = { hf::Kind::Flat, 0, {}, {} };
    ASSERT_TRUE (hf::Selected (other).has_value ());
    gui.Lay ({}, at (float (x), float (y), { { 0, true } }));
    gui.Lay ({}, at (float (x), float (y), { { 0, false } }));
    EXPECT_FALSE (hf::Selected (other).has_value ()) << "the other building's flat deselected";
    EXPECT_TRUE (hf::Selected (gui.state->planEditors.at ("building:Tower")).has_value ());
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
    state->previewCells = true;
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

// User, 2026-10-16: "When exit locked mode selection should clear up. ESC button should also clear
// selection." Locked, Archicad's slabs deselected (its Escape) deselect the flat too; Deselect keeps
// the one it is told to and nothing held.
TEST (OverlayFloorPlans, ArchicadsEscapeWhileLockedDeselectsTheFlat)
{
    namespace hf = geomsrv::archviz::hudfloorscheme;
    hudtest::Watched gui;
    hud::OwnPages selected;
    selected.floorPlansKnown = true;
    selected.floorPlans = { Tower () };
    selected.selection.known = true;
    selected.selection.count = 1;
    gui.engine.SetOwnPages (selected);
    hud::SetEditLocked (*gui.state, true);
    const auto flat = [] (const std::string& key) {
        hf::EditorPtr e (new hf::Editor);
        e->key = key;
        e->scheme.flats.resize (1);
        e->scheme.flats[0].shape = { { 0, 0 }, { 1, 0 }, { 1, 1 } };
        e->selected = { hf::Kind::Flat, 0, {}, {} };
        return e;
    };
    auto& editors = gui.state->planEditors;
    editors["a"] = flat ("a");
    editors["b"] = flat ("b");
    hf::Deselect (editors, "a");
    EXPECT_TRUE (hf::Selected (editors["a"]).has_value ());
    EXPECT_FALSE (hf::Selected (editors["b"]).has_value ());
    editors["b"] = flat ("b");
    editors["b"]->menuOpen = true;
    hf::Deselect (editors);
    EXPECT_FALSE (hf::Selected (editors["a"]).has_value ());
    EXPECT_TRUE (hf::Selected (editors["b"]).has_value ()) << "its menu open: held";
    editors["b"]->menuOpen = false;
    editors["a"] = flat ("a");
    hud::OwnPages none;
    none.selection.known = true;
    none.selection.count = 0;
    gui.engine.SetOwnPages (none);
    EXPECT_EQ (gui.state->floorPlanSnapshots.size (), 1u) << "locked: the floors stay";
    EXPECT_FALSE (hf::Selected (editors["a"]).has_value ());
    EXPECT_FALSE (hf::Selected (editors["b"]).has_value ());
}

// User, 2026-10-16: "Preview should be similar to ... Wireframe of main slab visible and for cells
// different graphics override presets"; "in floor plan view show unit as one color and inner walls
// thin lines similar like in mini GUI". Each preset colours the flats by what it names; the plan
// fills each flat one colour and draws its inner walls thin; the slabs stand as a wireframe.
TEST (OverlayFloorPlans, TheCellsAreColouredByTheMassingTabsPreset)
{
    auto state = hud::NewState ();
    auto tower = Tower (), low = Tower ();
    low.key = "building:Low";
    for (auto& floor : low.floors) {
        for (auto& contour : floor.contours)
            for (auto& chain : contour)
                for (size_t i = 0; i < chain.xy.size (); i += 2)
                    chain.xy[i] += 60;
        floor.outlineKey = "low";
        floor.programme = "Commercial";
        floor.programmeRgba = 0x336699FFu;
    }
    low.floors.pop_back (); // two floors
    state->floorPlanSnapshots[tower.key] = tower;
    state->floorPlanSnapshots[low.key] = low;
    state->previewCells = true;
    layers::Layer stairs, units, onPlan;
    ASSERT_TRUE (Planned (*state, stairs, units, 0));
    hud::SetPlanStorey (*state, 0);
    // The colours of the flats of a building, through the preset.
    const auto colours = [&] (hud::CellColours by) {
        state->cellColours = by;
        EXPECT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
        std::map<int, std::set<uint32_t>> out; // by mesh: one per building in key order, Low first
        for (size_t m = 0; m < units.meshes.size (); ++m)
            for (const uint32_t rgba : units.meshes[m].vertexRgba)
                if (rgba != 0xE4E6E6FFu && rgba != 0xB8BCBEFFu)
                    out[int (m)].insert (rgba);
        return out;
    };
    auto by = colours (hud::CellColours::Buildings);
    ASSERT_EQ (by.size (), 2u);
    EXPECT_EQ (by[0].size (), 1u) << "one building, one colour";
    EXPECT_NE (by[0], by[1]);
    by = colours (hud::CellColours::Programs);
    EXPECT_EQ (by[0], (std::set<uint32_t> { 0x336699FFu })) << "the section's use";
    EXPECT_EQ (by[1], (std::set<uint32_t> { 0xA9BCD0FFu })) << "none assigned: one neutral use colour";
    by = colours (hud::CellColours::StoryHeights);
    EXPECT_EQ (by[0], by[1]) << "the same floor height, the same colour";
    by = colours (hud::CellColours::UniqueUnits);
    EXPECT_GE (by[0].size (), 1u);
    by = colours (hud::CellColours::UnitMix);
    const auto* first = state->floorPlanner.Latest (bp::FloorId (tower.key, 0));
    ASSERT_NE (first, nullptr);
    ASSERT_FALSE (first->scheme.flats.empty ());
    const auto& flat = first->scheme.flats.front ();
    EXPECT_TRUE (by[1].contains (fp::Colour (state->massingProgramme, flat.type, flat.rooms) | 0xFFu))
        << "a flat in its programme type's colour";
    by = colours (hud::CellColours::SpaceTypes);
    EXPECT_TRUE (std::any_of (units.meshes.back ().vertexRgba.begin (), units.meshes.back ().vertexRgba.end (),
                              [] (uint32_t rgba) { return rgba == 0xEAD9A6FFu; }))
        << "the corridor in its own colour";
    // The plan: each flat filled, its outline, and its inner walls thin where its rooms are known.
    state->cellColours = hud::CellColours::UnitMix;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units, &onPlan));
    const auto* planned = state->floorPlanner.Latest (bp::FloorId (tower.key, 0));
    ASSERT_NE (planned, nullptr);
    const bool rooms = std::any_of (planned->scheme.flats.begin (), planned->scheme.flats.end (),
                                    [] (const auto& f) { return f.roomList.size () > 1; });
    const auto inner = std::count_if (onPlan.polylines.begin (), onPlan.polylines.end (),
                                      [] (const auto& line) { return line.rgba == 0x1D24276Eu; });
    if (rooms)
        EXPECT_GT (inner, 0) << "inner walls thin";
    for (const auto& line : onPlan.polylines)
        if (line.rgba == 0x1D24276Eu)
            EXPECT_FLOAT_EQ (line.widthPixels, 1.0f);
    // The slabs' wireframe: each floor's outline at its level and top, its corners between.
    const auto wire = std::count_if (units.polylines.begin (), units.polylines.end (),
                                     [] (const auto& line) { return line.rgba == 0x3C4248B0u; });
    EXPECT_GE (wire, (3 + 2) * (2 + 4)) << "five floors, two outlines and four corners each";
    EXPECT_TRUE (layers::Validate (units).empty ());
    EXPECT_TRUE (layers::Validate (onPlan).empty ());
}

// User, 2026-10-16: "In locked mode 3d view selection should select cell and higlight it, not the 2d
// contour. Because click through floor in 3D selection is BAD UI. So make cells selectable on any
// floor"; "add a toggle to show/hide cell complex hidden lines". Seen from above, the pointer is over
// a flat of every floor: the top one's solid is picked, its building turns to that floor, and its
// solid is lit and edged in the accent; a corridor in front picks nothing. Hidden lines switch off.
TEST (OverlayFloorPlans, In3DTheNearestCellIsPickedOnAnyFloorAndItsSolidHighlighted)
{
    namespace hf = geomsrv::archviz::hudfloorscheme;
    hudtest::Watched gui;
    hud::OwnPages own;
    own.standalone = true;
    own.floorPlansKnown = true;
    own.floorPlans = { Tower () };
    own.selection.known = true;
    own.selection.count = 1;
    gui.engine.SetOwnPages (own);
    gui.state->previewCells = true;
    layers::Layer stairs, units;
    ASSERT_TRUE (Planned (*gui.state, stairs, units, 0));
    hud::TakeFloorPlanLayers (*gui.state, stairs, units);
    // From above: 10 pixels a metre, y up, the eye looking down.
    const auto view = [] (float x, float y, std::vector<hud::Input::Button> buttons = {}) {
        auto input = hudtest::At (100 + 10 * x, 600 - 10 * y, std::move (buttons));
        input.planar = false;
        input.hasCameraDirection = true;
        input.cameraDirection[2] = -1;
        input.project = [] (double px, double py, double, float& sx, float& sy) {
            sx = float (100 + 10 * px), sy = float (600 - 10 * py);
            return true;
        };
        return input;
    };
    const auto* planned = gui.state->floorPlanner.Latest (bp::FloorId ("building:Tower", 2));
    ASSERT_NE (planned, nullptr);
    const auto& flat = planned->scheme.flats.front ();
    double x = 0, y = 0;
    for (const auto& p : flat.shape)
        x += p.x / flat.shape.size (), y += p.y / flat.shape.size ();
    const auto hit =
        hud::PickCell (*gui.state, view (float (x), float (y)), 100 + 10 * float (x), 600 - 10 * float (y));
    ASSERT_TRUE (hit.has_value ());
    EXPECT_EQ (hit->key, "building:Tower");
    EXPECT_EQ (hit->story, 2) << "the top floor's solid is nearest from above";
    // On the corridor, between the flats: nothing to pick there.
    const auto* corridor = planned->scheme.corridors.empty () ? nullptr : &planned->scheme.corridors.front ();
    if (corridor) {
        double cx = 0, cy = 0;
        for (const auto& p : corridor->shape)
            cx += p.x / corridor->shape.size (), cy += p.y / corridor->shape.size ();
        EXPECT_FALSE (
            hud::PickCell (*gui.state, view (float (cx), float (cy)), 100 + 10 * float (cx), 600 - 10 * float (cy))
                .has_value ());
    }
    // Locked, a click there selects that flat on that floor.
    hud::SetEditLocked (*gui.state, true);
    gui.state->buildingPlans["building:Tower"].story = 0;
    gui.Lay ({}, view (float (x), float (y)));
    gui.Lay ({}, view (float (x), float (y), { { 0, true } }));
    gui.Lay ({}, view (float (x), float (y), { { 0, false } }));
    const auto selected = hf::Selected (gui.state->planEditors["building:Tower"]);
    ASSERT_TRUE (selected.has_value ());
    EXPECT_EQ (selected->story, 2);
    EXPECT_EQ (gui.state->buildingPlans["building:Tower"].story, 2) << "its building turned to that floor";
    // Its solid lit and edged in the accent; no white contour in 3D.
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*gui.state, stairs, units));
    EXPECT_TRUE (std::any_of (units.meshes.begin (), units.meshes.end (), [] (const auto& mesh) {
        return mesh.style.edgeRgba == 0x2448C9FFu && mesh.style.edgeWidthPixels == 3.0f;
    }));
    for (const auto& line : units.polylines)
        EXPECT_NE (line.rgba, 0xFFFFFFFFu);
    // Hidden lines off: nothing of the cells drawn behind other cells.
    gui.state->cellHiddenLines = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*gui.state, stairs, units));
    for (const auto& mesh : units.meshes)
        EXPECT_EQ (mesh.style.behind, layers::Behind::Hide);
    for (const auto& line : units.polylines)
        EXPECT_EQ (line.behind, layers::Behind::Hide);
}
