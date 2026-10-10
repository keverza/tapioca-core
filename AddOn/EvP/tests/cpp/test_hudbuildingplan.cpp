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

TEST (HudBuildingPlan, PlacementIsLocalSupportsSeveralStairsAndRefusesOutsideHolesAndDuplicatePoints)
{
    bp::Plan plan;
    bp::Floor floor;
    floor.story = -1;
    floor.contours = { { Ring (0, 0, 30, 30), Ring (10, 10, 10, 10) } };
    plan.floors = { floor };
    bp::Draft draft;
    bp::Reset (plan, draft);
    EXPECT_FALSE (bp::Place (floor, draft, { 1, 1 }));
    draft.placing = true;
    EXPECT_FALSE (bp::Place (floor, draft, { 15, 15 }));
    EXPECT_TRUE (draft.placing);
    ASSERT_TRUE (bp::Place (floor, draft, { 4, 4 }));
    EXPECT_TRUE (bp::Dirty (draft));
    EXPECT_TRUE (plan.saved.empty ());
    draft.selected = -1;
    draft.placing = true;
    EXPECT_FALSE (bp::Place (floor, draft, { 4, 4 }));
    EXPECT_TRUE (bp::Place (floor, draft, { 26, 26 }));
    ASSERT_EQ (draft.cores.size (), 2u);
    draft.selected = 0;
    draft.placing = true;
    EXPECT_TRUE (bp::Place (floor, draft, { 4, 26 }));
    EXPECT_EQ (draft.cores[0].center, (bp::Point { 2.25, 26 }));
    EXPECT_EQ (draft.cores[1].center, (bp::Point { 27.75, 26 }));
    bp::Reset (plan, draft);
    EXPECT_FALSE (bp::Dirty (draft));
    EXPECT_TRUE (draft.cores.empty ());
    EXPECT_FALSE (draft.placing);
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
    draft.placing = true;
    ASSERT_TRUE (bp::Place (plan.floors.front (), draft, { 2, 2 }));
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

TEST (HudBuildingPlan, TwoStairsCheckUsesUnroundedCombinedDisplayedGrossAreaStrictlyAbove500)
{
    geomsrv::archviz::massingareas::Coefficients coefficients;
    coefficients.grossFactor = 1;
    EXPECT_FALSE (bp::NeedsTwoStairs (500, coefficients));
    EXPECT_TRUE (bp::NeedsTwoStairs (500.00001, coefficients));
    EXPECT_FALSE (bp::NeedsTwoStairs (499.99999, coefficients));
    coefficients.grossFactor = 0.78;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("lower", 0, 0, 35), Slab ("same-floor", 0, 35, 35) }, {}, nullptr, result, error));
    const auto plan = Preview (result).plan;
    ASSERT_EQ (plan.floors.size (), 1u);
    EXPECT_EQ (plan.floors[0].areaM2, 700);
    EXPECT_TRUE (bp::NeedsTwoStairs (plan.floors[0].areaM2, coefficients));
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
    EXPECT_FALSE (bp::NeedsTwoStairs (floor.areaM2, {}));
    EXPECT_NE (floor.contours[0][0].xy, floor.physical[0][0].xy);
    result.rows[0].chains[0].closed = false;
    EXPECT_TRUE (bp::Build (result, preview).floors.empty ());
}

TEST (HudBuildingPlan, MixedFingerprintChangesConflictAndPointLimitDoesNotPreventMovingExistingStair)
{
    bp::Plan plan;
    plan.mixed = true;
    plan.sources.push_back ({ "lower", {}, "before" });
    bp::Draft draft;
    bp::Reset (plan, draft);
    plan.sources[0].fingerprint = "after";
    EXPECT_TRUE (bp::Conflict (plan, draft));
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 100, 100) } };
    for (size_t i = 0; i < bp::kMaxStairs; ++i)
        draft.cores.push_back ({ { double (i) + 1, 1 } });
    draft.selected = -1;
    draft.placing = true;
    EXPECT_FALSE (bp::Place (floor, draft, { 50, 50 }));
    draft.selected = 0;
    EXPECT_TRUE (bp::Place (floor, draft, { 50, 50 }));
    EXPECT_EQ (draft.cores.size (), bp::kMaxStairs);
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

TEST (HudBuildingPlan, RectangularFootprintMustFitTheCountedUnionIncludingCourtyardInteriors)
{
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 20, 20), Ring (6, 6, 2, 2) } };
    EXPECT_TRUE (bp::Fits (floor, { 3, 3 }));
    EXPECT_TRUE (bp::Fits (floor, { bp::kStairWidth / 2, bp::kStairDepth / 2 }));
    EXPECT_TRUE (bp::Contains (floor, { 5, 7 }));
    EXPECT_FALSE (bp::Fits (floor, { 5, 7 })); // Centre fits, but rectangle enters the hole.
    EXPECT_FALSE (bp::Fits (floor, { 7, 7 })); // A hole wholly inside the rectangle must also fail.
    EXPECT_FALSE (bp::Fits (floor, { 1, 3 }));
    floor.contours.push_back ({ Ring (6, 6, 2, 2) });
    EXPECT_TRUE (bp::Fits (floor, { 5, 7 })); // Another source fills the courtyard.
    floor.contours = { { Ring (700000, 6000000, 20, 20) } };
    EXPECT_TRUE (bp::Fits (floor, { 700005, 6000005 }));
    EXPECT_FALSE (bp::Fits (floor, { std::numeric_limits<double>::quiet_NaN (), 0 }));
}

TEST (HudBuildingPlan, RectangleSnapsToUnionEdgesAndCornersWithinHalfAMetreNotInternalSeams)
{
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 20, 20) } };
    EXPECT_NEAR (bp::Snap (floor, { 2.4, 8 }).x, 2.25, 1e-6);
    EXPECT_DOUBLE_EQ (bp::Snap (floor, { 2.4, 8 }).y, 8);
    const auto corner = bp::Snap (floor, { 2, 2 });
    EXPECT_NEAR (corner.x, 2.25, 1e-6);
    EXPECT_NEAR (corner.y, bp::kStairDepth / 2, 1e-6);
    EXPECT_TRUE (bp::Fits (floor, { corner }));
    EXPECT_EQ (bp::Snap (floor, { 3, 8 }), (bp::Point { 2.25, 8 }));
    floor.contours.push_back ({ Ring (10, 0, 20, 20) });
    EXPECT_EQ (bp::Snap (floor, { 12.4, 8 }), (bp::Point { 12.4, 8 }));
    // A slanted boundary still snaps by translation; project-axis dimensions stay fixed.
    geomsrv::archviz::SliceChain triangle;
    triangle.closed = true;
    triangle.xy = { 0, 0, 30, 0, 0, 30 };
    floor.contours = { { triangle } };
    const auto slanted = bp::Snap (floor, { 12.7, 12.7 });
    EXPECT_NEAR (slanted.x + slanted.y + bp::kStairWidth / 2 + bp::kStairDepth / 2, 30, 1e-6);
    EXPECT_TRUE (bp::Fits (floor, { slanted }));
}

TEST (HudBuildingPlan, MoveIsAPressHoldDragGestureWithGrabOffsetValidationAndCancelRollback)
{
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 30, 30), Ring (15, 15, 5, 5) } };
    bp::Draft draft;
    draft.known = true;
    draft.original = draft.cores = { { { 5, 5 } } };
    draft.selected = 0;
    EXPECT_FALSE (bp::BeginDrag (draft, { 5, 5 }));
    draft.moving = true;
    EXPECT_FALSE (bp::BeginDrag (draft, { 10, 10 }));
    ASSERT_TRUE (bp::BeginDrag (draft, { 6, 5 }));
    EXPECT_EQ (draft.cores[0].center, (bp::Point { 5, 5 })); // Press alone never teleports.
    ASSERT_TRUE (bp::Drag (floor, draft, { 9, 8 }));
    EXPECT_EQ (draft.cores[0].center, (bp::Point { 8, 8 }));
    EXPECT_FALSE (bp::Drag (floor, draft, { 18, 17 }));
    EXPECT_EQ (draft.cores[0].center, (bp::Point { 8, 8 }));
    bp::Cancel (draft);
    EXPECT_EQ (draft.cores[0].center, (bp::Point { 5, 5 }));
    EXPECT_FALSE (bp::Dirty (draft));
    draft.moving = true;
    ASSERT_TRUE (bp::BeginDrag (draft, { 5, 5 }));
    ASSERT_TRUE (bp::Drag (floor, draft, { 10, 10 }));
    bp::EndDrag (draft);
    EXPECT_EQ (draft.cores[0].center, (bp::Point { 10, 10 }));
    EXPECT_TRUE (bp::Dirty (draft));
    EXPECT_FALSE (draft.dragging);
    EXPECT_FALSE (draft.moving);
}

TEST (HudBuildingPlan, SelectionHasOneEditableSectionAndPlacementDoesNotPushTheLayoutDown)
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
    auto& draft = gui.state->buildingPlans.at ("building:Tower");
    draft.placing = true;
    const auto placing = gui.Lay ({}, hudtest::At (600, 600));
    EXPECT_FLOAT_EQ (normal.host.height, placing.host.height);
    bp::Cancel (draft);
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

TEST (HudBuildingPlan, CoresSnapAcrossForbiddenFacadeGapButInteriorAndCourtyardClearancesStayValid)
{
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 30, 30), Ring (12, 12, 6, 6) } };
    EXPECT_FALSE (bp::CoreAllowed (floor, { { 8, 4 } }));
    const bp::Core flush { bp::Snap (floor, { { 8, 4 } }) };
    EXPECT_NEAR (flush.center.y, 2.1, 1e-6);
    EXPECT_TRUE (bp::CoreAllowed (floor, flush));
    EXPECT_TRUE (bp::CoreAllowed (floor, { { 7, 5.1 } })); // Exactly 3 m from the bottom.
    EXPECT_FALSE (bp::CoreAllowed (floor, { { 9, 14 } })) << "Too close to the courtyard wall";
    bp::Draft draft;
    draft.known = draft.placing = true;
    ASSERT_TRUE (bp::Place (floor, draft, { 8, 4 }));
    EXPECT_TRUE (bp::CoreAllowed (floor, draft.cores[0]));
}

TEST (HudBuildingPlan, FastFootprintChecksMatchBooleanContainmentForHolesConcavityRotationAndGeoreference)
{
    namespace cp = Clipper2Lib;
    bp::Floor floor;
    floor.outlineKnown = true;
    // Unioned winding, including a hole and an inward notch.
    floor.outline = { Ring (0, 0, 30, 30), Ring (12, 12, 6, 6) };
    floor.outline[0].xy = { 0, 0, 30, 0, 30, 30, 20, 30, 20, 24, 0, 24 };
    cp::PathsD source;
    for (const auto& ring : floor.outline) {
        cp::PathD path;
        for (size_t i = 0; i < ring.Count (); ++i)
            path.emplace_back (ring.xy[i * 2], ring.xy[i * 2 + 1]);
        source.push_back (path);
    }
    source = cp::Union (source, cp::FillRule::EvenOdd, 6);
    floor.outline.clear ();
    for (const auto& path : source) {
        geomsrv::archviz::SliceChain ring;
        ring.closed = true;
        for (const auto& p : path)
            ring.xy.insert (ring.xy.end (), { p.x, p.y });
        floor.outline.push_back (ring);
    }
    for (double angle : { 0.0, 0.37 })
        for (int x = 0; x <= 30; ++x)
            for (int y = 0; y <= 30; ++y) {
                const bp::Core core { { x + 0.07, y + 0.13 } };
                cp::PathD rectangle;
                for (const auto& p : bp::Corners (core, angle))
                    rectangle.emplace_back (p.x, p.y);
                const bool expected =
                    std::abs (cp::Area (cp::Difference ({ rectangle }, source, cp::FillRule::NonZero, 6))) < 1e-4;
                EXPECT_EQ (bp::Fits (floor, core, angle), expected) << x << "," << y << " @ " << angle;
            }
    for (auto& ring : floor.outline)
        for (size_t i = 0; i < ring.Count (); ++i) {
            ring.xy[i * 2] += 700000;
            ring.xy[i * 2 + 1] += 6000000;
        }
    EXPECT_TRUE (bp::Fits (floor, { { 700007, 6000007 } }, 0.37));
    EXPECT_FALSE (bp::Fits (floor, { { 700015, 6000015 } }, 0.37));
}

TEST (HudBuildingPlan, NarrowPlanControlsFitAndBodyDraggingAndPlacementHoverDoNotRegenerateThePlan)
{
    // Exercise Draw directly so the narrow content bounds and un-clipped control extents
    // can be asserted, rather than mistaking clipped-off buttons for a fitting layout.
    auto* previous = ImGui::GetCurrentContext ();
    auto* context = ImGui::CreateContext ();
    const auto cleanup = [previous] (ImGuiContext* value) {
        ImGui::DestroyContext (value);
        ImGui::SetCurrentContext (previous);
    };
    std::unique_ptr<ImGuiContext, decltype (cleanup)> held (context, cleanup);
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 2000 };
    io.DeltaTime = 1.0f / 60;
    ASSERT_NE (io.Fonts->AddFontFromFileTTF (EVP_SCENE_TEXT_FONT, 14), nullptr);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    bp::Plan plan;
    plan.key = "building:Narrow";
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 36, 16) } };
    floor.outlineKnown = true;
    floor.outline = floor.contours.front ();
    floor.outlineKey = bp::QuickSignature (floor, {});
    plan.floors = { floor };
    plan.saved = { { { 18, 2.1 } } };
    bp::Draft draft;
    bp::Reset (plan, draft);
    const auto frame = [&] (ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 240, 1800 });
        ImGui::Begin ("Plan controls", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        bp::Draw (plan, draft, 1, {});
        EXPECT_LE (ImGui::GetCurrentWindow ()->DC.CursorMaxPos.x, 240 - ImGui::GetStyle ().WindowPadding.x + 1);
        ImVec4 bounds { 1e6f, 1e6f, -1e6f, -1e6f };
        for (const auto& vertex : ImGui::GetWindowDrawList ()->VtxBuffer)
            if (vertex.col == IM_COL32 (74, 144, 217, 255)) {
                bounds.x = (std::min) (bounds.x, vertex.pos.x);
                bounds.y = (std::min) (bounds.y, vertex.pos.y);
                bounds.z = (std::max) (bounds.z, vertex.pos.x);
                bounds.w = (std::max) (bounds.w, vertex.pos.y);
            }
        ImGui::End ();
        ImGui::Render ();
        return bounds;
    };
    const auto bounds = frame ({ 900, 900 }, false);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (quick.ready) << quick.note;
    const size_t at = 0;
    auto p = quick.units[at].center;
    p.x += 0.6; // Inside the polygon, deliberately away from the removed centre handle.
    const float factor = (bounds.z - bounds.x) / 36;
    const ImVec2 mouse { bounds.x + float (p.x) * factor, bounds.w - float (p.y) * factor };
    frame (mouse, false);
    frame (mouse, true);
    EXPECT_TRUE (quick.dragging);
    EXPECT_EQ (quick.selected, int (at));
    frame (mouse, false);
    draft.placing = true;
    const auto revision = quick.revision;
    const auto start = std::chrono::steady_clock::now ();
    for (int n = 0; n < 100; ++n)
        frame ({ bounds.x + (n % 30 + 2) * factor, (bounds.y + bounds.w) / 2 }, false);
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
    RecordProperty ("meanPlacementHoverMs", ms / 100);
    EXPECT_EQ (quick.revision, revision);
    EXPECT_EQ (draft.cores, plan.saved);
    EXPECT_LT (ms, 3000) << "Guard against Boolean geometry / generation on every hover";
}
