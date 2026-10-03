// ArchViz/HudSection: the building section on the HUD's Selection page (the user, 2026-10-03):
// the massing slabs' floors stacked by storey -- a podium wider than its tower -- each in the
// colour of its usage; floors picked by a press, a shift-press or a drag; a value assigned to
// the picked floors as a range over each slab's own floors of them.

#include "hud_fixture.hpp"

#include "ArchViz/HudSection.hpp"
#include "Metadata/TapiocaMetadata.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace hudtest;

namespace meta = geomsrv::metadata;
namespace hm = geomsrv::archviz::hudmeta;
namespace hs = geomsrv::archviz::hudsection;
namespace shell = geomsrv::archviz::hudshell;

namespace {

namespace slices = geomsrv::archviz::slabslices;

// A slab of `count` floors from storey `first`, each `area` square metres.
hs::Slab SlabOf (const std::string& guid, int first, int count, double area)
{
    hs::Slab slab;
    slab.guid = guid;
    for (int k = 0; k < count; ++k) {
        slices::Floor floor;
        floor.base = 3.0 * double (first + k);
        floor.height = 3.0;
        floor.areaM2 = area;
        slab.floors.push_back (floor);
        slab.storeys.push_back (first + k);
    }
    return slab;
}

geomsrv::archviz::ProjectStoreys Storeys (int count)
{
    geomsrv::archviz::ProjectStoreys storeys;
    for (int s = 0; s < count; ++s) {
        storeys.levels.push_back (3.0 * double (s));
        storeys.names.push_back (s == 0 ? "Ground" : "Level " + std::to_string (s));
        storeys.indices.push_back (s);
    }
    return storeys;
}

// A podium of three floors in commerce, a tower of seven above it, residential over a range.
std::vector<hs::Slab> Building ()
{
    hs::Slab podium = SlabOf ("PODIUM", 0, 3, 1000.0);
    meta::Property commerce;
    commerce.key = "program.usage";
    commerce.value = meta::Value::Option ("commercial");
    meta::SetProperty (podium.meta, commerce);
    hs::Slab tower = SlabOf ("TOWER", 3, 7, 400.0);
    meta::RangeAssignment homes;
    homes.domain = meta::kFloorDomain;
    homes.from = 3.0;
    homes.to = 9.0;
    meta::Property residential;
    residential.key = "program.usage";
    residential.value = meta::Value::Option ("residential");
    homes.properties.push_back (residential);
    meta::AssignRange (tower.meta, homes);
    return { podium, tower };
}

const hs::Floor* FloorAt (const hs::Section& section, int storey)
{
    for (const hs::Floor& floor : section.floors)
        if (floor.storey == storey)
            return &floor;
    return nullptr;
}

} // namespace

// ⚠️ THE USER: floors stacked, the podium's and the tower's widths, coloured by usage.
TEST (HudSection, TheSlabsFloorsAreTheSectionsRowsByStorey)
{
    const hs::Section section = hs::Build (Building (), Storeys (10), meta::DefaultSchema ());
    ASSERT_TRUE (section.known);
    EXPECT_EQ (section.key, "program.usage") << "the first enumeration over floors";
    ASSERT_EQ (section.floors.size (), 10u);
    EXPECT_EQ (section.floors.front ().storey, 0) << "the lowest first";
    EXPECT_EQ (section.floors.front ().label, "Ground");
    EXPECT_DOUBLE_EQ (section.widestM2, 1000.0);
    const hs::Floor* ground = FloorAt (section, 0);
    ASSERT_NE (ground, nullptr);
    ASSERT_EQ (ground->parts.size (), 1u);
    EXPECT_EQ (ground->parts[0].value, "commercial") << "the element's own value where no range says one";
    EXPECT_EQ (ground->parts[0].rgba, 0xE4572EFFu);
    const hs::Floor* fifth = FloorAt (section, 5);
    ASSERT_NE (fifth, nullptr);
    EXPECT_DOUBLE_EQ (fifth->areaM2, 400.0) << "the tower narrower than the podium";
    EXPECT_EQ (fifth->parts[0].value, "residential") << "the range's";
    EXPECT_EQ (fifth->parts[0].rgba, 0xF2C14EFFu);
    ASSERT_EQ (section.spans.size (), 2u);
    EXPECT_EQ (section.spans[0].low, 0);
    EXPECT_EQ (section.spans[0].high, 2);
    EXPECT_EQ (section.spans[1].low, 3);
    EXPECT_EQ (section.spans[1].high, 9);
}

// A run across both slabs is two edits, each clipped to its slab's own floors.
TEST (HudSection, ARunIsAssignedToEachSlabOverItsOwnFloors)
{
    const hs::Section section = hs::Build (Building (), Storeys (10), meta::DefaultSchema ());
    const std::vector<hm::Edit> edits = hs::RunEdits (section, { 2, 4 }, "office", false);
    ASSERT_EQ (edits.size (), 2u);
    EXPECT_EQ (edits[0].element, "PODIUM");
    EXPECT_DOUBLE_EQ (edits[0].from, 2.0);
    EXPECT_DOUBLE_EQ (edits[0].to, 2.0);
    EXPECT_EQ (edits[1].element, "TOWER");
    EXPECT_DOUBLE_EQ (edits[1].from, 3.0);
    EXPECT_DOUBLE_EQ (edits[1].to, 4.0);
    EXPECT_EQ (edits[1].domain, meta::kFloorDomain);
    EXPECT_TRUE (hs::RunEdits (section, hs::Run {}, "office", false).empty ()) << "nothing picked";

    // Laid on the tower: floors 3-4 office, 5-9 still residential.
    std::vector<hs::Slab> building = Building ();
    std::string error;
    ASSERT_TRUE (hm::Apply (building[1].meta, edits[1], meta::DefaultSchema (), 0, error)) << error;
    EXPECT_EQ (meta::RangeValue (building[1].meta, meta::kFloorDomain, 4.0, "program.usage")->value.s, "office");
    EXPECT_EQ (meta::RangeValue (building[1].meta, meta::kFloorDomain, 5.0, "program.usage")->value.s, "residential");
    EXPECT_TRUE (meta::Validate (building[1].meta, meta::DefaultSchema ()).empty ());
    hm::Edit clear = edits[1];
    clear.action = hm::Edit::Action::Clear;
    ASSERT_TRUE (hm::Apply (building[1].meta, clear, meta::DefaultSchema (), 0, error)) << error;
    EXPECT_EQ (meta::RangeValue (building[1].meta, meta::kFloorDomain, 3.0, "program.usage"), nullptr);
}

// Through the HUD: a press picks a floor, a shift-press runs to another, a drag picks the
// floors it crosses.
TEST (HudSection, FloorsArePickedByAPressAShiftPressAndADrag)
{
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 1;
    pages.section = hs::Build ({ SlabOf ("A", 0, 3, 500.0) }, Storeys (3), meta::DefaultSchema ());
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, shell::kSelectionKey);
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    ASSERT_GT (out.host.height, 0.0f);

    // The section's top edge: the first hand down the host's middle, below the tab row.
    const float x = 16.0f + out.host.width * 0.5f;
    float top = 0.0f;
    for (float y = 52.0f; y < 16.0f + out.host.height; y += 1.0f)
        if (hud.Lay ({}, At (x, y)).hand) {
            top = y;
            break;
        }
    ASSERT_GT (top, 0.0f) << "the section is on the page";
    // Three rows of floor (13 x 1.2) px and a pixel apart; the highest floor on top.
    const float pitch = std::floor (13.0f * 1.2f) + 1.0f;
    const float highest = top + 4.0f, lowest = top + 2.0f * pitch + 4.0f;

    hud.Click ({}, x, highest);
    EXPECT_EQ (hud::PickedFloors (*hud.state), (hs::Run { 2, 2 }));
    hud::Input shifted = At (x, lowest);
    shifted.shift = true;
    hud.Lay ({}, shifted);
    shifted.buttons = { { 0, true } };
    hud.Lay ({}, shifted);
    shifted.buttons = { { 0, false } };
    hud.Lay ({}, shifted);
    EXPECT_EQ (hud::PickedFloors (*hud.state), (hs::Run { 0, 2 })) << "a shift-press runs from the floor picked";
    ASSERT_FALSE (hud.heard.empty ());
    EXPECT_EQ (hud.heard.back ().kind, "floors");
    EXPECT_EQ (hud.heard.back ().text, "0-2");

    hud.Click ({}, x, lowest);
    EXPECT_EQ (hud::PickedFloors (*hud.state), (hs::Run { 0, 0 }));
    hud.Lay ({}, At (x, top + pitch + 4.0f, { { 0, true } }));
    hud.Lay ({}, At (x, highest));
    hud.Lay ({}, At (x, highest, { { 0, false } }));
    EXPECT_EQ (hud::PickedFloors (*hud.state), (hs::Run { 1, 2 })) << "dragged from the middle floor up";
}

// ⚠️ THE RIGHT CLICK IS THE SECTION'S: it opens the values to assign, not the HUD's own menu,
// and the first one pressed is one edit per slab with a floor in the run.
TEST (HudSection, ARightClickAssignsAValueToThePickedFloors)
{
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 2;
    pages.section =
        hs::Build ({ SlabOf ("A", 0, 2, 800.0), SlabOf ("B", 2, 2, 300.0) }, Storeys (4), meta::DefaultSchema ());
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, shell::kSelectionKey);
    const hud::Layout out = hud.Lay ({}, At (600.0f, 600.0f));
    const float x = 16.0f + out.host.width * 0.5f;
    float top = 0.0f;
    for (float y = 52.0f; y < 16.0f + out.host.height; y += 1.0f)
        if (hud.Lay ({}, At (x, y)).hand) {
            top = y;
            break;
        }
    ASSERT_GT (top, 0.0f);
    const float pitch = std::floor (13.0f * 1.2f) + 1.0f;
    // Floors 3 down to 0, top first: pick floors 1-2 by a drag, then right-click on floor 2.
    const float floor2 = top + pitch + 4.0f, floor1 = top + 2.0f * pitch + 4.0f;
    hud.Lay ({}, At (x, floor1, { { 0, true } }));
    hud.Lay ({}, At (x, floor2));
    hud.Lay ({}, At (x, floor2, { { 0, false } }));
    ASSERT_EQ (hud::PickedFloors (*hud.state), (hs::Run { 1, 2 }));
    hud.Lay ({}, At (x, floor2, { { 1, true } }));
    hud.Lay ({}, At (x, floor2, { { 1, false } }));
    // The menu at the pointer: below the click, the first row the hand shows over is the first
    // value -- Residential.
    float option = 0.0f;
    for (float y = floor2 + 2.0f; y < floor2 + 120.0f; y += 1.0f)
        if (hud.Lay ({}, At (x + 30.0f, y)).hand) {
            option = y + 3.0f;
            break;
        }
    ASSERT_GT (option, 0.0f) << "the section's menu is open";
    hud.Click ({}, x + 30.0f, option);
    const std::vector<hm::Edit> edits = hud::TakeMetadataEdits (*hud.state);
    ASSERT_EQ (edits.size (), 2u) << "one per slab in the run";
    EXPECT_EQ (edits[0].element, "A");
    EXPECT_DOUBLE_EQ (edits[0].from, 1.0);
    EXPECT_DOUBLE_EQ (edits[0].to, 1.0);
    EXPECT_EQ (edits[1].element, "B");
    EXPECT_DOUBLE_EQ (edits[1].from, 2.0);
    EXPECT_DOUBLE_EQ (edits[1].to, 2.0);
    EXPECT_EQ (edits[0].text, "residential");
    EXPECT_EQ (edits[0].action, hm::Edit::Action::Set);
}
