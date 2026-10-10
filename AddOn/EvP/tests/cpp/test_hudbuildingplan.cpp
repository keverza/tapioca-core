#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Geometry/Primitives.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <chrono>
#include <clipper2/clipper.h>
#include "imgui.h"
#include "imgui_internal.h"

namespace bp = geomsrv::archviz::buildingplan;
namespace ms = geomsrv::archviz::massingslices;
namespace mb = geomsrv::archviz::massingbuildings;
namespace hm = geomsrv::archviz::hudmeta;
namespace meta = geomsrv::metadata;
namespace hud = geomsrv::archviz::overlayhud;
namespace {
ms::Input Slab (const char* guid, double base, double x, double width = 10)
{
    ms::Input input;
    input.slab.guid = guid;
    input.slab.bottom = base;
    input.slab.top = base + 3;
    input.slab.outer.xy = { x, 0, x + width, 0, x + width, 10, x, 10 };
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>> {
             { "massing.buildingId", "Tower" }, { "tapioca.role", "MassingSlab" } }) {
        meta::Property property;
        property.key = key;
        property.value = meta::Value::Text (value);
        meta::SetProperty (input.metadata, property);
    }
    return input;
}
mb::Preview Preview (const ms::Result& slices)
{
    std::vector<mb::Record> records;
    for (const auto& surface : slices.buildingSurfaces)
        records.push_back (surface.record);
    auto preview = mb::Previews (slices.section, records, {}, { "lower" }).front ();
    preview.plan = bp::Build (slices, preview);
    return preview;
}
geomsrv::archviz::SliceChain Ring (double x, double y, double width, double height)
{
    geomsrv::archviz::SliceChain chain;
    chain.closed = true;
    chain.xy = { x, y, x + width, y, x + width, y + height, x, y + height };
    return chain;
}
} // namespace

TEST (HudBuildingPlan, LowestPhysicalFloorIsDefaultAndSectionFloorIdsMapToOwnSourceRows)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("upper", 3, 20), Slab ("lower", -3, 700000) }, {}, nullptr, result, error))
        << error;
    auto preview = Preview (result);
    ASSERT_EQ (preview.plan.floors.size (), 2u);
    bp::Draft draft;
    bp::Reset (preview.plan, draft);
    const auto* floor = bp::Displayed (preview.plan, draft);
    ASSERT_NE (floor, nullptr);
    EXPECT_EQ (floor->z, -3);
    EXPECT_TRUE (bp::Contains (*floor, { 700005, 5 }));
    EXPECT_FALSE (bp::Contains (*floor, { 25, 5 }));
    draft.story = preview.plan.floors.back ().story;
    EXPECT_TRUE (bp::Contains (*bp::Displayed (preview.plan, draft), { 25, 5 }));
    EXPECT_EQ (bp::Displayed (preview.plan, draft)->z, 3);
    draft.story = 1000;
    EXPECT_EQ (bp::Displayed (preview.plan, draft)->z, -3);
    result.complete = false;
    EXPECT_TRUE (bp::Build (result, preview).floors.empty ());
    result.complete = true;
    result.rows.pop_back ();
    EXPECT_TRUE (bp::Build (result, preview).floors.empty ());
}

TEST (HudBuildingPlan, CourtyardsDisconnectedIslandsAndOverlappingSourcePartsKeepTheirTopology)
{
    bp::Floor floor;
    floor.contours = { { Ring (700000, 6000000, 10, 10), Ring (700002, 6000002, 6, 6), Ring (700020, 6000020, 10, 10) },
                       { Ring (700005, 6000000, 10, 10) } };
    EXPECT_TRUE (bp::Contains (floor, { 700001, 6000001 }));
    EXPECT_FALSE (bp::Contains (floor, { 700003, 6000003 }));
    EXPECT_TRUE (bp::Contains (floor, { 700006, 6000003 })) << "Other slab can cover this slab's courtyard";
    EXPECT_TRUE (bp::Contains (floor, { 700009, 6000001 })) << "Source overlaps are a union, not parity cancellation";
    EXPECT_TRUE (bp::Contains (floor, { 700025, 6000025 }));
    EXPECT_FALSE (bp::Contains (floor, { 700015, 6000015 }));
    EXPECT_FALSE (bp::Contains (floor, { 700000, 6000005 }));
    EXPECT_FALSE (bp::Contains (floor, { 700002, 6000005 }));
}

TEST (HudBuildingPlan, SavedLocationsRoundTripAsOneListAndTargetEveryExactBuildingMember)
{
    auto lower = Slab ("lower", 0, 0), upper = Slab ("upper", 3, 0);
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ lower, upper }, {}, nullptr, result, error)) << error;
    const auto preview = Preview (result);
    bp::Draft draft;
    bp::Reset (preview.plan, draft);
    draft.cores = { { { 500, 4 } } };
    EXPECT_TRUE (bp::Edits (preview.plan, draft).empty ()) << "Save refuses a stair on no floor of the building";
    draft.cores = { { { 2.25, 2.1 } }, { { 5, 8.75 }, 9.0, 2.5 } };
    auto edits = bp::Edits (preview.plan, draft);
    ASSERT_EQ (edits.size (), 2u);
    const auto schema = meta::DefaultSchema ();
    EXPECT_EQ (edits[0].expectedBuildingKey, "building:Tower");
    EXPECT_EQ (edits[0].element, "lower");
    EXPECT_EQ (edits[1].element, "upper");
    ASSERT_TRUE (bp::Matches (lower.metadata, edits[0]));
    ASSERT_TRUE (hm::Apply (lower.metadata, edits[0], schema, 42, error)) << error;
    EXPECT_TRUE (meta::Validate (lower.metadata, schema).empty ());
    EXPECT_EQ (bp::Read (lower.metadata).cores, draft.cores) << "Sizes travel with the locations";
    EXPECT_FALSE (bp::Matches (lower.metadata, edits[0])) << "Do not overwrite metadata changed before deferred write";
    auto persisted = lower.metadata;
    ASSERT_TRUE (meta::FromJson (meta::ToJson (lower.metadata), persisted, error));
    EXPECT_EQ (bp::Read (persisted).cores, draft.cores);
    ASSERT_TRUE (hm::Apply (upper.metadata, edits[1], schema, 42, error));
    ASSERT_TRUE (ms::Build ({ lower, upper }, {}, nullptr, result, error));
    auto saved = Preview (result).plan;
    EXPECT_FALSE (saved.mixed);
    EXPECT_EQ (saved.saved.size (), 2u);
    bp::Reset (saved, draft);
    draft.cores.clear ();
    edits = bp::Edits (saved, draft);
    ASSERT_EQ (edits.size (), 2u);
    EXPECT_EQ (edits[0].action, hm::Edit::Action::Clear);
    EXPECT_TRUE (hm::Apply (lower.metadata, edits[0], schema, 43, error));
    EXPECT_FALSE (bp::Read (lower.metadata).invalid);
    EXPECT_TRUE (bp::Read (lower.metadata).cores.empty ());
    EXPECT_EQ (meta::FindProperty (lower.metadata, bp::kShapes), nullptr) << "Clear removes the sizes too";
}

TEST (HudBuildingPlan, MixedSavedValuesAndChangedMembershipRequireExplicitDraftResolution)
{
    auto lower = Slab ("lower", 0, 0), upper = Slab ("upper", 3, 0);
    hm::Edit edit;
    edit.id = bp::kLocations;
    edit.type = meta::ValueType::List;
    edit.numbers = { 1, 1 };
    std::string error;
    ASSERT_TRUE (hm::Apply (lower.metadata, edit, meta::DefaultSchema (), 1, error));
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ lower, upper }, {}, nullptr, result, error));
    auto plan = Preview (result).plan;
    ASSERT_TRUE (plan.mixed);
    EXPECT_TRUE (plan.saved.empty ());
    bp::Draft draft;
    bp::Reset (plan, draft);
    EXPECT_FALSE (bp::Dirty (draft));
    EXPECT_TRUE (bp::Edits (plan, draft).empty ());
    draft.cores = { { { 2, 2 } } };
    draft.changed = true;
    EXPECT_EQ (bp::Edits (plan, draft).size (), 2u);
    plan.guids.push_back ("new-member");
    EXPECT_TRUE (bp::Conflict (plan, draft));
    EXPECT_TRUE (bp::Edits (plan, draft).empty ());
}

TEST (HudBuildingPlan, StairwellSchemaAndGuardsRefuseInvalidCoordinatesRolesAndBuildingIdentity)
{
    auto source = Slab ("lower", 0, 0);
    hm::Edit edit;
    edit.id = bp::kLocations;
    edit.type = meta::ValueType::List;
    std::string error;
    const auto schema = meta::DefaultSchema ();
    for (const auto& coordinates : std::vector<std::vector<double>> {
             {}, { 1 }, { 1, std::numeric_limits<double>::quiet_NaN () }, { 1, 1e10 }, std::vector<double> (66, 0) }) {
        edit.numbers = coordinates;
        EXPECT_FALSE (hm::Apply (source.metadata, edit, schema, 0, error));
        EXPECT_EQ (meta::FindProperty (source.metadata, bp::kLocations), nullptr);
    }
    edit.numbers = { 1, 1 };
    edit.expectedBuildingKey = "building:Tower";
    edit.element = "lower";
    edit.expectedPropertyJson = bp::Fingerprint (source.metadata);
    ASSERT_TRUE (bp::Matches (source.metadata, edit));
    meta::RemoveProperty (source.metadata, "tapioca.role");
    EXPECT_FALSE (bp::Matches (source.metadata, edit));
    source = Slab ("lower", 0, 0);
    meta::RemoveProperty (source.metadata, "massing.buildingId");
    EXPECT_FALSE (bp::Matches (source.metadata, edit));
    edit.expectedBuildingKey = "slab:lower";
    EXPECT_TRUE (bp::Matches (source.metadata, edit));
    meta::Property malformed;
    malformed.key = bp::kLocations;
    malformed.value.type = meta::ValueType::List;
    malformed.value.elementType = meta::ValueType::Length;
    malformed.value.list = { meta::Value::Number (1, meta::ValueType::Length) };
    meta::SetProperty (source.metadata, malformed);
    EXPECT_TRUE (bp::Read (source.metadata).invalid);
    EXPECT_FALSE (meta::Validate (source.metadata, schema).empty ());
    // Sizes: one 2-12 m pair per location, else refused; a legacy list without sizes reads as 4.5 x 4.2 m.
    source = Slab ("lower", 0, 0);
    edit.numbers = { 1, 1, 5, 5 };
    for (const auto& shapes : std::vector<std::vector<double>> { { 4.5 }, { 4.5, 4.2, 1, 3 }, { 4.5, 4.2, 13, 3 } }) {
        edit.shapes = shapes;
        EXPECT_FALSE (hm::Apply (source.metadata, edit, schema, 0, error));
    }
    edit.shapes = { 4.5, 4.2, 9, 2.5 };
    ASSERT_TRUE (hm::Apply (source.metadata, edit, schema, 0, error)) << error;
    EXPECT_TRUE (meta::Validate (source.metadata, schema).empty ());
    EXPECT_EQ (bp::Read (source.metadata).cores[1], (bp::Core { { 5, 5 }, 9, 2.5 }));
    edit.shapes.clear ();
    ASSERT_TRUE (hm::Apply (source.metadata, edit, schema, 0, error));
    EXPECT_EQ (meta::FindProperty (source.metadata, bp::kShapes), nullptr);
    EXPECT_EQ (bp::Read (source.metadata).cores[1], (bp::Core { { 5, 5 } }));
}

TEST (HudBuildingPlan, CountedPlacementAndAreaRemainClippedWhilePhysicalOutlineIsRetained)
{
    const auto source = Slab ("lower", 0, 0, 100);
    geomsrv::Mesh body;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 25, 5, 3 }, 50, 10, 12, body, error));
    geomsrv::archviz::massingcalculation::Result envelope;
    envelope.hasEnvelope = true;
    geomsrv::archviz::overlaylayers::Mesh shell;
    shell.points = body.vertices;
    shell.indices = body.triangles;
    envelope.layer.meshes.push_back (shell);
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ source }, {}, &envelope, result, error)) << error;
    auto preview = Preview (result);
    ASSERT_EQ (preview.plan.floors.size (), 1u);
    const auto& floor = preview.plan.floors.front ();
    EXPECT_NEAR (floor.areaM2, 500, 1e-6);
    EXPECT_TRUE (bp::Contains (floor, { 25, 5 }));
    EXPECT_FALSE (bp::Contains (floor, { 75, 5 }));
    EXPECT_NE (floor.contours[0][0].xy, floor.physical[0][0].xy);
    result.rows[0].chains[0].closed = false;
    EXPECT_TRUE (bp::Build (result, preview).floors.empty ());
}

TEST (HudBuildingPlan, MixedFingerprintChangesConflict)
{
    bp::Plan plan;
    plan.mixed = true;
    plan.sources.push_back ({ "lower", {}, "before" });
    bp::Draft draft;
    bp::Reset (plan, draft);
    plan.sources[0].fingerprint = "after";
    EXPECT_TRUE (bp::Conflict (plan, draft));
}

TEST (HudBuildingPlan, NativeSelectionPlanDefaultsToLowestThenClickSwitchesButHoverDoesNot)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("lower", 0, 0), Slab ("upper", 3, 20) }, {}, nullptr, result, error));
    hudtest::Watched gui;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 1;
    pages.buildings = { Preview (result) };
    gui.engine.SetOwnPages (pages);
    hud::SelectKey (*gui.state, geomsrv::archviz::hudshell::kSelectionKey);
    const auto layout = gui.Lay ({}, hudtest::At (600, 600));
    auto& draft = gui.state->buildingPlans.at ("building:Tower");
    const auto& plan = pages.buildings[0].plan;
    ASSERT_TRUE (draft.known);
    EXPECT_EQ (bp::Displayed (plan, draft)->z, 0);
    const float x = 16 + layout.host.width * 0.6f;
    float rowY = 0;
    for (float y = 60; y < 16 + layout.host.height; y += 2) {
        gui.Lay ({}, hudtest::At (x, y));
        if (!gui.state->hoveredFloors.Empty ()) {
            rowY = y + 3;
            break;
        }
    }
    ASSERT_GT (rowY, 0);
    EXPECT_EQ (bp::Displayed (plan, draft)->z, 0);
    gui.Click ({}, x, rowY);
    EXPECT_EQ (bp::Displayed (plan, draft)->z, 3);
    EXPECT_TRUE (hud::TakeMetadataEdits (*gui.state).empty ());
    pages.selection.count = 0;
    pages.buildings.clear ();
    gui.engine.SetOwnPages (pages);
    EXPECT_TRUE (gui.state->buildingPlans.empty ());
}

TEST (HudBuildingPlan, SelectionHasOneEditableSection)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("lower", 0, 0), Slab ("upper", 3, 20) }, {}, nullptr, result, error));
    hudtest::Watched gui;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 1;
    pages.buildings = { Preview (result) };
    gui.engine.SetOwnPages (pages);
    hud::SelectKey (*gui.state, geomsrv::archviz::hudshell::kSelectionKey);
    const auto normal = gui.Lay ({}, hudtest::At (600, 600));
    int bands = 0;
    bool wasHover = false;
    int previous = (std::numeric_limits<int>::min) ();
    for (float y = 60; y < 16 + normal.host.height; y += 1) {
        gui.Lay ({}, hudtest::At (16 + normal.host.width * 0.6f, y));
        const bool hover = !gui.state->hoveredFloors.Empty ();
        bands += hover && (!wasHover || gui.state->hoveredFloors.first != previous);
        previous = gui.state->hoveredFloors.first;
        wasHover = hover;
    }
    EXPECT_EQ (bands, 2); // Two floors, once each; no read-only second diagram.
}
