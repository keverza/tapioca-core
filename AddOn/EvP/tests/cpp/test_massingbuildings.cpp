#include "ArchViz/MassingBuildings.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "Geometry/Primitives.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <set>

namespace mb = geomsrv::archviz::massingbuildings;
namespace ms = geomsrv::archviz::massingslices;
namespace hs = geomsrv::archviz::hudsection;
namespace meta = geomsrv::metadata;
namespace hud = geomsrv::archviz::overlayhud;
namespace {
ms::Input Slab (const char* guid, const char* id, double bottom = 0, double top = 3, double x = 0)
{
    ms::Input input;
    input.slab.guid = guid;
    input.slab.bottom = bottom;
    input.slab.top = top;
    input.slab.outer.xy = { x, 0, x + 10, 0, x + 10, 10, x, 10 };
    meta::Property property;
    property.key = "massing.buildingId";
    property.value = meta::Value::Text (id);
    meta::SetProperty (input.metadata, property);
    geomsrv::Mesh body;
    std::string error;
    EXPECT_TRUE (geomsrv::engine::MakeBox ({ x + 5, 5, (bottom + top) / 2 }, 10, 10, top - bottom, body, error));
    input.facadeBody = std::make_shared<const geomsrv::Mesh> (std::move (body));
    return input;
}
std::vector<mb::Record> Records (const ms::Result& result)
{
    std::vector<mb::Record> records;
    for (const auto& surface : result.buildingSurfaces)
        records.push_back (surface.record);
    return records;
}
ms::Input AreaSlab (const char* guid, const char* id, double area, double bottom = 0, double top = 3, double x = 0)
{
    auto input = Slab (guid, id, bottom, top, x);
    input.slab.outer.xy = { x, 0, x + area / 10, 0, x + area / 10, 10, x, 10 };
    return input;
}
} // namespace

TEST (MassingBuildings, ExactIdsGroupSlabsAndMissingIdsNeverMerge)
{
    const std::vector<mb::Record> records { { "a", "Tower" }, { "b", "Tower" }, { "c", "tower" },
                                            { "d", "" },      { "e", "" },      { "a", "Tower" } };
    const auto groups = mb::Groups (records);
    ASSERT_EQ (groups.size (), 4u);
    EXPECT_EQ (groups[0].guids, std::vector<std::string> ({ "a", "b" }));
    EXPECT_EQ (mb::Members (records, { "b" }), std::vector<std::string> ({ "a", "b" }));
    EXPECT_EQ (mb::Members (records, { "d" }), std::vector<std::string> ({ "d" }));
    EXPECT_EQ (mb::Members (records, { "unknown" }), std::vector<std::string> ({ "unknown" }));
    EXPECT_TRUE (mb::Members (records, {}).empty ());
}

TEST (MassingBuildings, OneSelectedSlabPreviewsEveryFloorAndTargetsAllItsBuildingMembers)
{
    auto lower = Slab ("lower", "Tower"), upper = Slab ("upper", "Tower", 3, 9);
    auto other = Slab ("other", "Other", 0, 6, 20);
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ lower, upper, other }, {}, nullptr, result, error)) << error;
    const auto previews = mb::Previews (result.section, Records (result), result.heightControls, { "upper" });
    ASSERT_EQ (previews.size (), 1u);
    EXPECT_EQ (previews[0].building.id, "Tower");
    ASSERT_EQ (previews[0].section.floors.size (), 3u);
    EXPECT_EQ (hs::TotalArea (previews[0].section), 300);
    EXPECT_EQ (previews[0].heights.size (), 2u);
    const auto edits = hs::RunEdits (previews[0].section, { 0, 2 }, "commercial", false);
    ASSERT_EQ (edits.size (), 2u);
    EXPECT_EQ (edits[0].element, "lower");
    EXPECT_EQ (edits[0].from, 0);
    EXPECT_EQ (edits[0].to, 0);
    EXPECT_EQ (edits[1].element, "upper");
    EXPECT_EQ (edits[1].from, 0); // Upper slab retains its own authored floor-domain indices.
    EXPECT_EQ (edits[1].to, 1);
    geomsrv::archviz::overlaylayers::Layer highlighted;
    ASSERT_TRUE (ms::FloorHighlight (result, "building:Tower", { 2, 2 }, highlighted, error)) << error;
    ASSERT_EQ (highlighted.meshes.size (), 1u);
    for (size_t i = 2; i < highlighted.meshes[0].points.size (); i += 3) {
        EXPECT_GE (highlighted.meshes[0].points[i], 6);
        EXPECT_LE (highlighted.meshes[0].points[i], 9);
    }
}

TEST (MassingBuildings, SeparateBuildingsDoNotCombineAndMissingMembersRefusePartialPreview)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("a", "A"), Slab ("b", "B", 0, 6, 20) }, {}, nullptr, result, error)) << error;
    auto records = Records (result);
    auto previews = mb::Previews (result.section, records, result.heightControls, { "a", "b" });
    ASSERT_EQ (previews.size (), 2u);
    EXPECT_EQ (hs::TotalArea (previews[0].section), 100);
    EXPECT_EQ (hs::TotalArea (previews[1].section), 200);
    records.push_back ({ "missing", "A" });
    previews = mb::Previews (result.section, records, result.heightControls, { "a" });
    ASSERT_EQ (previews.size (), 1u);
    EXPECT_TRUE (previews[0].section.floors.empty ());
    EXPECT_FALSE (previews[0].section.note.empty ());
}

TEST (MassingBuildings, WholeBuildingInspectionUsesEveryOperatedBodyAndUniqueColoursFollowIds)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("a", "Tower"), Slab ("b", "Tower", 3, 6), Slab ("c", "Other", 0, 3, 20) }, {},
                            nullptr, result, error))
        << error;
    geomsrv::archviz::overlaylayers::Layer all, selected, reordered;
    ASSERT_TRUE (mb::Inspect (result.buildingSurfaces, {}, all, error)) << error;
    ASSERT_EQ (all.meshes.size (), 3u);
    EXPECT_NE (all.meshes[0].rgba, all.meshes[1].rgba);
    EXPECT_EQ (all.meshes[1].rgba, all.meshes[2].rgba);
    ASSERT_TRUE (mb::Inspect (result.buildingSurfaces, "building:Tower", selected, error)) << error;
    ASSERT_EQ (selected.meshes.size (), 2u);
    EXPECT_EQ (selected.meshes[0].points, result.buildingSurfaces[0].body->vertices);
    EXPECT_EQ (selected.meshes[1].points, result.buildingSurfaces[1].body->vertices);
    auto surfaces = result.buildingSurfaces;
    std::reverse (surfaces.begin (), surfaces.end ());
    ASSERT_TRUE (mb::Inspect (surfaces, {}, reordered, error));
    EXPECT_EQ (reordered.meshes[0].rgba, all.meshes[0].rgba);
    EXPECT_EQ (reordered.meshes[1].rgba, all.meshes[1].rgba);
    surfaces[0].body.reset ();
    selected.name = "unchanged";
    EXPECT_FALSE (mb::Inspect (surfaces, {}, selected, error));
    EXPECT_EQ (selected.name, "unchanged");
}

TEST (MassingBuildings, InspectionHasDistinctColoursAcrossTheFullSlabBudget)
{
    const auto input = Slab ("base", "base");
    std::vector<mb::Surface> surfaces;
    for (int i = 0; i < 128; ++i)
        surfaces.push_back ({ { std::to_string (i), std::to_string (i) }, input.facadeBody });
    geomsrv::archviz::overlaylayers::Layer layer;
    std::string error;
    ASSERT_TRUE (mb::Inspect (surfaces, {}, layer, error)) << error;
    std::set<uint32_t> colours;
    for (const auto& mesh : layer.meshes)
        colours.insert (mesh.rgba);
    EXPECT_EQ (colours.size (), 128u);
    surfaces.push_back (surfaces.back ());
    EXPECT_FALSE (mb::Inspect (surfaces, {}, layer, error));
}

TEST (MassingBuildings, FloorHighlightIncludesAllSameIdPartsButNeitherOtherFloorsNorOtherBuildings)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("a", "Tower", 0, 9), Slab ("b", "Tower", 0, 9, 10), Slab ("c", "Other", 0, 9, 20) },
                            {}, nullptr, result, error))
        << error;
    geomsrv::archviz::overlaylayers::Layer layer;
    ASSERT_TRUE (ms::FloorHighlight (result, "building:Tower", { 1, 1 }, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 2u);
    for (const auto& mesh : layer.meshes)
        for (size_t i = 0; i < mesh.points.size (); i += 3) {
            EXPECT_GE (mesh.points[i + 2], 3);
            EXPECT_LE (mesh.points[i + 2], 6);
            EXPECT_LE (mesh.points[i], 20);
        }
    ASSERT_TRUE (ms::FloorHighlight (result, "building:Tower", {}, layer, error)) << error;
    EXPECT_TRUE (layer.meshes.empty ());
}

TEST (MassingBuildings, HoverTemporarilyOverridesClickedFloorAndSharedStateClearsWithProject)
{
    auto state = hud::NewState ();
    EXPECT_FALSE (hud::UniqueBuildings (*state));
    state->uniqueBuildings = true;
    state->highlightedBuilding = "building:Tower";
    state->pickedFloorBuilding = "building:Tower";
    state->floors = { 1, 2 };
    state->hoveredFloorBuilding = "building:Other";
    state->hoveredFloors = { 3, 3 };
    EXPECT_EQ (hud::BuildingFloorKey (*state), "building:Other");
    EXPECT_EQ (hud::BuildingFloors (*state).first, 3);
    state->hoveredFloors = {};
    EXPECT_EQ (hud::BuildingFloorKey (*state), "building:Tower");
    EXPECT_EQ (hud::BuildingFloors (*state).last, 2);
    hud::ClearState (*state);
    EXPECT_FALSE (hud::UniqueBuildings (*state));
    EXPECT_TRUE (hud::HighlightedBuilding (*state).empty ());
    EXPECT_TRUE (hud::BuildingFloors (*state).Empty ());
}

TEST (MassingBuildings, RealPreviewFloorHoverAndClickEmitHighlightChangesButNoMetadataEdits)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("lower", "Tower"), Slab ("upper", "Tower", 3, 9) }, {}, nullptr, result, error));
    hudtest::Watched gui;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 1;
    pages.buildings = mb::Previews (result.section, Records (result), result.heightControls, { "upper" });
    gui.engine.SetOwnPages (pages);
    hud::SelectKey (*gui.state, geomsrv::archviz::hudshell::kSelectionKey);
    const auto layout = gui.Lay ({}, hudtest::At (600, 600));
    const float x = 16 + layout.host.width * 0.6f;
    float rowY = 0;
    for (float y = 60; y < 16 + layout.host.height; y += 2) {
        gui.Lay ({}, hudtest::At (x, y));
        if (!gui.state->hoveredFloors.Empty ()) {
            rowY = y + 4;
            break;
        }
    }
    ASSERT_GT (rowY, 0);
    gui.Lay ({}, hudtest::At (x, rowY));
    EXPECT_EQ (hud::BuildingFloorKey (*gui.state), "building:Tower");
    EXPECT_EQ (hud::BuildingFloors (*gui.state).first, 2);
    EXPECT_TRUE (hud::PickedFloors (*gui.state).Empty ()) << "hover does not change persistent picks";
    gui.Lay ({}, hudtest::At (600, 600));
    EXPECT_TRUE (hud::BuildingFloors (*gui.state).Empty ());
    gui.Click ({}, x, rowY);
    gui.Lay ({}, hudtest::At (600, 600));
    EXPECT_EQ (hud::BuildingFloorKey (*gui.state), "building:Tower");
    EXPECT_EQ (hud::PickedFloors (*gui.state), (hs::Run { 2, 2 }));
    gui.Lay ({}, hudtest::At (x, rowY, { { 1, true } }));
    gui.Lay ({}, hudtest::At (x, rowY, { { 1, false } }));
    EXPECT_TRUE (hud::TakeMetadataEdits (*gui.state).empty ());
    EXPECT_TRUE (std::any_of (gui.heard.begin (), gui.heard.end (),
                              [] (const auto& change) { return change.kind == "buildingFloorHover"; }));
    pages.selection.count = 0;
    pages.buildings.clear ();
    gui.engine.SetOwnPages (pages);
    gui.Lay ({}, hudtest::At (600, 600));
    EXPECT_TRUE (hud::BuildingFloors (*gui.state).Empty ()) << "selection change clears stale building picks";
}

TEST (MassingBuildings, WholeBuildingCheckboxIsMouseAccessible)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ Slab ("slab", "Tower") }, {}, nullptr, result, error));
    hudtest::Watched gui;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.selection.known = true;
    pages.selection.count = 1;
    pages.buildings = mb::Previews (result.section, Records (result), result.heightControls, { "slab" });
    gui.engine.SetOwnPages (pages);
    hud::SelectKey (*gui.state, geomsrv::archviz::hudshell::kSelectionKey);
    const auto layout = gui.Lay ({}, hudtest::At (600, 600));
    const float x = 16 + layout.host.width * 0.5f;
    float buttonY = 0;
    int islands = 0;
    bool wasHand = false;
    for (float y = 60; y < 16 + layout.host.height; y += 2) {
        const bool hand = gui.Lay ({}, hudtest::At (x, y)).hand;
        if (hand && !wasHand && ++islands == 2) { // Editor header, then the retained building checkbox.
            buttonY = y + 4;
            break;
        }
        wasHand = hand;
    }
    ASSERT_GT (buttonY, 0);
    gui.Click ({}, x, buttonY);
    EXPECT_EQ (hud::HighlightedBuilding (*gui.state), "building:Tower");
    EXPECT_TRUE (hud::TakeMetadataEdits (*gui.state).empty ());
    gui.Click ({}, x, buttonY);
    EXPECT_TRUE (hud::HighlightedBuilding (*gui.state).empty ());
}

TEST (MassingBuildings, LargeFloorMarksUseStrictUnroundedGrossThresholdAndSharedCoefficient)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ AreaSlab ("below", "Below", 499.999), AreaSlab ("equal", "Equal", 500, 0, 3, 100),
                              AreaSlab ("above", "Above", 500.001, 0, 3, 200) },
                            {}, nullptr, result, error))
        << error;
    geomsrv::archviz::massingareas::Coefficients coefficients;
    coefficients.grossFactor = 1;
    geomsrv::archviz::overlaylayers::Layer layer;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), coefficients, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 1u);
    EXPECT_EQ (layer.name, ms::kLargeFloorsLayer);
    EXPECT_EQ (layer.views, geomsrv::archviz::overlaylayers::Views::Both);
    EXPECT_EQ (layer.occlusion, geomsrv::archviz::overlaylayers::Behind::Fade);
    EXPECT_NE (layer.meshes[0].hoverTitle.find ("Building Above"), std::string::npos);
    coefficients.grossFactor = 0.78;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), coefficients, layer, error));
    EXPECT_TRUE (layer.meshes.empty ()) << "500 m2 total is only 390 m2 gross at the default factor";
    coefficients.grossFactor = 0;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), coefficients, layer, error));
    EXPECT_TRUE (layer.meshes.empty ());
}

TEST (MassingBuildings, LargeFloorMarksCombineSameIdPartsWithoutMergingOtherBuildingsOrStackedFloors)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ AreaSlab ("a", "Tower", 350), AreaSlab ("b", "Tower", 350, 0, 3, 40),
                              AreaSlab ("upper", "Tower", 300, 3, 9), AreaSlab ("other", "Other", 350, 0, 3, 100),
                              AreaSlab ("empty1", "", 350, 0, 3, 150), AreaSlab ("empty2", "", 350, 0, 3, 200) },
                            {}, nullptr, result, error))
        << error;
    geomsrv::archviz::overlaylayers::Layer layer;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), {}, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 2u) << "700 x 0.78 = 546 gross on the lower combined floor only";
    for (const auto& mesh : layer.meshes) {
        EXPECT_NE (mesh.hoverTitle.find ("546.00"), std::string::npos);
        for (size_t i = 0; i < mesh.points.size (); i += 3) {
            EXPECT_LE (mesh.points[i], 75);
            EXPECT_LE (mesh.points[i + 2], 3);
        }
    }
}

TEST (MassingBuildings, LargeFloorMarksRetainCourtyardGeometryAndDoNotMutateFunctionColours)
{
    auto input = AreaSlab ("large", "Tower", 800);
    input.slab.holes.push_back ({ { 10, 2, 30, 2, 30, 8, 10, 8 }, {} }); // 120 m2 hole
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    const auto original = result.layer.meshes[0].rgba;
    geomsrv::archviz::overlaylayers::Layer layer;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), {}, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 1u);
    const auto& mesh = layer.meshes[0];
    EXPECT_NE (mesh.hoverTitle.find ("530.40"), std::string::npos);
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        double x = 0, y = 0;
        for (size_t j = 0; j < 3; ++j) {
            x += mesh.points[mesh.indices[i + j] * 3] / 3;
            y += mesh.points[mesh.indices[i + j] * 3 + 1] / 3;
        }
        EXPECT_FALSE (x > 10 + 1e-7 && x < 30 - 1e-7 && y > 2 + 1e-7 && y < 8 - 1e-7);
    }
    EXPECT_EQ (result.layer.meshes[0].rgba, original);
    EXPECT_EQ (result.rows[0].function, "residential");
}

TEST (MassingBuildings, LargeFloorMarksMatchAllowedSectionGrossRatherThanRawExcessOutsideEnvelope)
{
    auto input = AreaSlab ("large", "Tower", 1000);
    geomsrv::Mesh body;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 25, 5, 1.5 }, 50, 10, 6, body, error));
    geomsrv::archviz::massingcalculation::Result envelope;
    envelope.hasEnvelope = true;
    geomsrv::archviz::overlaylayers::Mesh shell;
    shell.points = body.vertices;
    shell.indices = body.triangles;
    envelope.layer.meshes.push_back (shell);
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, &envelope, result, error)) << error;
    EXPECT_NEAR (hs::TotalArea (result.section), 500, 1e-6);
    geomsrv::archviz::overlaylayers::Layer layer;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), {}, layer, error)) << error;
    EXPECT_TRUE (layer.meshes.empty ()) << "Raw 1000 x 0.78 is not the displayed allowed gross area";
}

TEST (MassingBuildings, LargeFloorMarksRefuseIncompleteMembershipAndInvalidCoefficientsAtomically)
{
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ AreaSlab ("large", "Tower", 800) }, {}, nullptr, result, error));
    auto records = Records (result);
    records.push_back ({ "missing", "Tower" });
    geomsrv::archviz::overlaylayers::Layer layer;
    layer.name = "unchanged";
    EXPECT_FALSE (ms::LargeFloorHighlight (result, records, {}, layer, error));
    EXPECT_EQ (layer.name, "unchanged");
    EXPECT_FALSE (error.empty ());
    EXPECT_FALSE (ms::LargeFloorHighlight (result, {}, {}, layer, error));
    EXPECT_EQ (layer.name, "unchanged");
    geomsrv::archviz::massingareas::Coefficients coefficients;
    coefficients.grossFactor = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (ms::LargeFloorHighlight (result, Records (result), coefficients, layer, error));
    EXPECT_EQ (layer.name, "unchanged");
    result = {};
    ASSERT_TRUE (ms::LargeFloorHighlight (result, {}, {}, layer, error));
    EXPECT_TRUE (layer.meshes.empty ());
}

TEST (MassingBuildings, LargeFloorMarksUseOperatedSlicesRatherThanTheUncutSlabPolygon)
{
    auto input = AreaSlab ("seo", "Tower", 900, 0, 6);
    geomsrv::Mesh operated;
    std::string error;
    ASSERT_TRUE (geomsrv::engine::MakeBox ({ 30, 5, 3 }, 60, 10, 6, operated, error));
    input.body = std::make_shared<const geomsrv::Mesh> (operated);
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    geomsrv::archviz::overlaylayers::Layer layer;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), {}, layer, error)) << error;
    EXPECT_TRUE (layer.meshes.empty ()) << "Operated 600 x 0.78 = 468, not the uncut polygon's 702 gross";
    geomsrv::archviz::massingareas::Coefficients coefficients;
    coefficients.grossFactor = 1;
    ASSERT_TRUE (ms::LargeFloorHighlight (result, Records (result), coefficients, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 2u);
    for (const auto& mesh : layer.meshes)
        for (size_t i = 0; i < mesh.points.size (); i += 3)
            EXPECT_LE (mesh.points[i], 60) << "No prism substitute in the highlight";
}
