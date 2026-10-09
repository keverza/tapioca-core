#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "hud_fixture.hpp"
#include <clipper2/clipper.h>
#include <gtest/gtest.h>
#include <cmath>
#include <chrono>

namespace bp = geomsrv::archviz::buildingplan;
namespace hud = geomsrv::archviz::overlayhud;
namespace cp = Clipper2Lib;
namespace {
geomsrv::archviz::SliceChain Ring (double x, double y, double w, double d)
{
    geomsrv::archviz::SliceChain ring;
    ring.closed = true;
    ring.xy = { x, y, x + w, y, x + w, y + d, x, y + d };
    return ring;
}
bp::Floor Floor ()
{
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, 36, 16) } };
    floor.height = 3;
    return floor;
}
cp::PathsD Paths (const std::vector<bp::PlanRegion>& regions)
{
    cp::PathsD paths;
    for (const auto& region : regions)
        for (const auto& ring : region.rings) {
            cp::PathD path;
            for (size_t i = 0; i < ring.Count (); ++i)
                path.emplace_back (ring.xy[i * 2], ring.xy[i * 2 + 1]);
            paths.push_back (std::move (path));
        }
    return paths;
}
double Area (const std::vector<bp::PlanRegion>& regions)
{
    return std::abs (cp::Area (Paths (regions)));
}
bp::Plan Building ()
{
    bp::Plan plan;
    plan.key = "building:Tower";
    auto floor = Floor ();
    plan.floors = { floor, floor, floor };
    for (int i = 0; i < 3; ++i) {
        plan.floors[size_t (i)].story = i;
        plan.floors[size_t (i)].z = i * 3;
    }
    plan.saved = { { 6, 3 } };
    return plan;
}
size_t MultiSeed (const bp::QuickPlan& plan)
{
    for (size_t i = 0; i < plan.seeds.size (); ++i)
        if (std::count_if (plan.seeds.begin (), plan.seeds.end (),
                           [&] (const auto& seed) { return seed.segment == plan.seeds[i].segment; }) > 1)
            return i;
    return plan.seeds.size ();
}
} // namespace

TEST (HudFloorPlan, StairsAutomaticallyProduceBarsCirculationBandsSegmentsAndUnits)
{
    const auto floor = Floor ();
    const auto plan = bp::GenerateQuick (floor, { { 6, 3 } });
    ASSERT_TRUE (plan.ready) << plan.note;
    EXPECT_FALSE (plan.bars.empty ());
    EXPECT_FALSE (plan.corridors.empty ());
    EXPECT_FALSE (plan.bands.empty ());
    EXPECT_FALSE (plan.segments.empty ());
    EXPECT_EQ (plan.units.size (), plan.seeds.size ());
    EXPECT_GT (plan.units.size (), 3u);
    EXPECT_NEAR (Area (plan.units), Area (plan.segments), 1e-4);
    EXPECT_LT (Area (plan.units) + Area (plan.corridors), 36 * 16);
    EXPECT_NEAR (cp::Area (cp::Intersect (Paths (plan.units), Paths (plan.corridors), cp::FillRule::NonZero, 6)), 0,
                 1e-6);
}

TEST (HudFloorPlan, UnitCentresNudgeActualBoundariesAddSplitDeleteRedistributeAndKeepIDs)
{
    auto plan = bp::GenerateQuick (Floor (), { { 6, 3 } });
    ASSERT_TRUE (plan.ready) << plan.note;
    size_t at = 0;
    for (; at < plan.seeds.size (); ++at)
        if (std::count_if (plan.seeds.begin (), plan.seeds.end (),
                           [&] (const auto& seed) { return seed.segment == plan.seeds[at].segment; }) > 1)
            break;
    ASSERT_LT (at, plan.seeds.size ());
    const auto id = plan.seeds[at].id;
    auto center = bp::UnitCenter (plan, plan.seeds[at]);
    const auto original = plan.units[at].rings;
    const auto s = plan.segments[plan.seeds[at].segment];
    const double axisAngle = plan.angle + (s.alongX ? 0 : 1.5707963267948966);
    center.x += std::cos (axisAngle) * 0.9;
    center.y += std::sin (axisAngle) * 0.9;
    ASSERT_TRUE (bp::MoveUnit (plan, at, center));
    EXPECT_EQ (plan.seeds[at].id, id);
    EXPECT_NE (plan.units[at].rings.front ().xy, original.front ().xy);
    const auto count = plan.seeds.size ();
    center = bp::UnitCenter (plan, plan.seeds[at]);
    center.x -= std::cos (axisAngle) * 1.2;
    center.y -= std::sin (axisAngle) * 1.2;
    ASSERT_TRUE (bp::AddUnit (plan, center));
    EXPECT_EQ (plan.seeds.size (), count + 1);
    EXPECT_NEAR (Area (plan.units), Area (plan.segments), 1e-4);
    ASSERT_TRUE (bp::RemoveUnit (plan, size_t (plan.selected)));
    EXPECT_EQ (plan.seeds.size (), count);
    EXPECT_EQ (plan.seeds[at].id, id);
    EXPECT_NEAR (Area (plan.units), Area (plan.segments), 1e-4);
    EXPECT_FALSE (bp::AddUnit (plan, { -10, -10 }));
}

TEST (HudFloorPlan, InFlightUnitDragCancelsWithoutMetadataOrSeedCountChanges)
{
    auto plan = bp::GenerateQuick (Floor (), { { 6, 3 } });
    ASSERT_TRUE (plan.ready);
    plan.selected = int (plan.seeds.size () - 1);
    plan.dragOriginal = plan.seeds.back ().along;
    plan.dragging = true;
    plan.owner = 42;
    auto p = bp::UnitCenter (plan, plan.seeds.back ());
    p.x += 0.9 * std::cos (plan.angle);
    p.y += 0.9 * std::sin (plan.angle);
    ASSERT_TRUE (bp::MoveUnit (plan, size_t (plan.selected), p));
    EXPECT_FALSE (bp::RemoveUnit (plan, 0));
    bp::CancelUnits (plan);
    EXPECT_DOUBLE_EQ (plan.seeds.back ().along, plan.dragOriginal);
    EXPECT_FALSE (plan.dragging);
    EXPECT_EQ (plan.owner, 0u);
}

TEST (HudFloorPlan, SameWorldOutlineSharesAcrossFloorHeightsButUniqueAndResetAreLocal)
{
    auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& first = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (first.ready);
    auto& other = bp::QuickFor (plan, draft, plan.floors[2]);
    EXPECT_EQ (&first, &other);
    ASSERT_TRUE (bp::RemoveUnit (first, 0));
    const size_t sharedCount = first.seeds.size ();
    bp::MakeUnique (plan, draft, plan.floors[0]); // The shared representative itself becomes unique.
    auto& unique = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (bp::RemoveUnit (unique, 0));
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[1]).seeds.size (), sharedCount);
    EXPECT_EQ (unique.seeds.size (), sharedCount - 1);
    bp::ResetQuick (plan, draft, plan.floors[0]);
    EXPECT_FALSE (draft.uniqueFloors.contains (0));
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds.size (), sharedCount);
    EXPECT_EQ (&bp::QuickFor (plan, draft, plan.floors[0]), &bp::QuickFor (plan, draft, plan.floors[2]));
    EXPECT_FALSE (bp::Dirty (draft));
    EXPECT_TRUE (bp::Edits (plan, draft).empty ()) << "Unit editing must never persist stair metadata.";
}

TEST (HudFloorPlan, CacheIdentityRetainsWorldPlacementAndChangesOnHolesCoresNotFloorIndex)
{
    auto a = Floor ();
    a.outlineKnown = true;
    a.outline = a.contours.front ();
    auto b = a;
    b.story = 77;
    b.z = 100;
    auto& ring = b.outline.front ().xy;
    std::rotate (ring.begin (), ring.begin () + 2, ring.end ());
    EXPECT_EQ (bp::QuickSignature (a, {}), bp::QuickSignature (b, {}));
    b.outline.front ().xy = { 0, 0, 18, 0, 36, 0, 36, 16, 0, 16 };
    EXPECT_EQ (bp::QuickSignature (a, {}), bp::QuickSignature (b, {}))
        << "Collinear source seams are not shape changes";
    b.outline.push_back (Ring (10, 5, 4, 4));
    EXPECT_NE (bp::QuickSignature (a, {}), bp::QuickSignature (b, {}));
    EXPECT_NE (bp::QuickSignature (a, { { 6, 3 } }), bp::QuickSignature (a, { { 7, 3 } }));
    b = a;
    for (auto& x : b.outline.front ().xy)
        x += 100;
    EXPECT_NE (bp::QuickSignature (a, {}), bp::QuickSignature (b, {}));
}

TEST (HudFloorPlan, MissingOverlappingOutsideCoresAndUnservedIslandsRefuseApartmentGeneration)
{
    auto floor = Floor ();
    auto plan = bp::GenerateQuick (floor, {});
    EXPECT_FALSE (plan.ready);
    EXPECT_FALSE (plan.bars.empty ());
    EXPECT_FALSE (bp::GenerateQuick (floor, { { 1, 1 } }).ready);
    EXPECT_FALSE (bp::GenerateQuick (floor, { { 6, 3 }, { 7, 3 } }).ready);
    floor.contours.push_back ({ Ring (50, 0, 36, 16) });
    plan = bp::GenerateQuick (floor, { { 6, 3 } });
    EXPECT_FALSE (plan.ready);
    EXPECT_TRUE (plan.units.empty ());
    EXPECT_NE (plan.note.find ("disconnected"), std::string::npos);
}

TEST (HudFloorPlan, StaleOutlineOrCoreChangesResetUnitEditsButStairSaveAcknowledgementRetainsThem)
{
    auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (quick.ready);
    const size_t original = quick.seeds.size ();
    ASSERT_TRUE (bp::RemoveUnit (quick, 0));
    bp::Reset (plan, draft); // Fresh Save acknowledgement: same points and membership.
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds.size (), original - 1);
    draft.points[0].x += 0.9;
    auto& changed = bp::QuickFor (plan, draft, plan.floors[0]);
    EXPECT_NE (changed.note.find ("reset"), std::string::npos);
    EXPECT_EQ (changed.seeds.size (), original);
}

TEST (HudFloorPlan, OverlayTogglesPublishEveryFloorAndLocalUnitEditsWithoutRepeatedRebuilds)
{
    auto state = hud::NewState ();
    const auto plan = Building ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewStairs = true;
    state->previewUnits = true;
    geomsrv::archviz::overlaylayers::Layer stairs, units;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_EQ (stairs.name, bp::kStairsLayer);
    EXPECT_EQ (units.name, bp::kUnitsLayer);
    EXPECT_TRUE (stairs.polylines.empty ());
    ASSERT_EQ (stairs.meshes.size (), 3u);
    EXPECT_GT (units.polylines.size (), 3u);
    for (size_t i = 0; i < stairs.meshes.size (); ++i) {
        EXPECT_DOUBLE_EQ (stairs.meshes[i].points[2], double (i) * 3);
        EXPECT_DOUBLE_EQ (stairs.meshes[i].points[14], double (i + 1) * 3);
        EXPECT_EQ (stairs.meshes[i].rgba, 0x969696FFu);
        EXPECT_EQ (stairs.meshes[i].indices.size (), 36u);
    }
    EXPECT_TRUE (geomsrv::archviz::overlaylayers::Validate (stairs).empty ());
    EXPECT_FALSE (hud::TakeFloorPlanLayers (*state, stairs, units));
    auto& draft = state->buildingPlans.at (plan.key);
    ASSERT_TRUE (bp::RemoveUnit (bp::QuickFor (plan, draft, plan.floors[0]), 0));
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    state->previewStairs = state->previewUnits = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (stairs.polylines.empty ());
    EXPECT_TRUE (stairs.meshes.empty ());
    EXPECT_TRUE (units.polylines.empty ());
    state->floorPlanSnapshots.clear ();
    EXPECT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
}

TEST (HudFloorPlan, UnsupportedSlantedOutlineRefusesRatherThanSubstitutingBoundingRectangle)
{
    auto floor = Floor ();
    floor.contours.front ().front ().xy = { 0, 0, 36, 0, 30, 16, 0, 16 };
    const auto plan = bp::GenerateQuick (floor, { { 6, 3 } });
    EXPECT_FALSE (plan.ready);
    EXPECT_TRUE (plan.units.empty ());
    EXPECT_NE (plan.note.find ("orthogonal"), std::string::npos);
}

TEST (HudFloorPlan, RoomChoicesMeetActualTargetsAndRelaxCentresWithoutChangingTotalArea)
{
    auto plan = bp::GenerateQuick (Floor (), { { 6, 3 } });
    ASSERT_TRUE (plan.ready);
    size_t at = 0;
    for (; at < plan.seeds.size (); ++at)
        if (std::count_if (plan.seeds.begin (), plan.seeds.end (),
                           [&] (const auto& seed) { return seed.segment == plan.seeds[at].segment; }) > 1)
            break;
    ASSERT_LT (at, plan.seeds.size ());
    const auto center = bp::UnitCenter (plan, plan.seeds[at]);
    const auto id = plan.seeds[at].id;
    const double total = Area (plan.units);
    double previous = 0;
    for (double rooms : { 1.0, 1.5, 2.0, 3.0, 4.0 }) {
        ASSERT_TRUE (bp::SetUnitRooms (plan, at, rooms));
        EXPECT_GT (bp::UnitArea (plan.units[at]), previous);
        previous = bp::UnitArea (plan.units[at]);
        EXPECT_NE (bp::UnitCenter (plan, plan.seeds[at]), center);
        EXPECT_NEAR (bp::UnitArea (plan.units[at]), bp::UnitTargetArea (rooms), 1e-4);
        EXPECT_EQ (plan.seeds[at].id, id);
        EXPECT_NEAR (Area (plan.units), total, 1e-4);
    }
    EXPECT_DOUBLE_EQ (bp::UnitTargetArea (1), 34);
    EXPECT_DOUBLE_EQ (bp::UnitTargetArea (1.5), 41);
    EXPECT_DOUBLE_EQ (bp::UnitTargetArea (2), 48);
    EXPECT_DOUBLE_EQ (bp::UnitTargetArea (3), 65);
    EXPECT_DOUBLE_EQ (bp::UnitTargetArea (4), 82);
    EXPECT_FALSE (bp::SetUnitRooms (plan, at, 5));
    EXPECT_FALSE (bp::SetUnitRooms (plan, at, std::numeric_limits<double>::quiet_NaN ()));
}

TEST (HudFloorPlan, SharedRoomWeightsCopyIntoUniqueFloorsAndResetRejoinsTheOriginalWeight)
{
    const auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& shared = bp::QuickFor (plan, draft, plan.floors[0]);
    const size_t at = MultiSeed (shared);
    ASSERT_LT (at, shared.seeds.size ());
    ASSERT_TRUE (bp::SetUnitRooms (shared, at, 1.5));
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[2]).seeds[at].rooms, 1.5);
    bp::MakeUnique (plan, draft, plan.floors[1]);
    ASSERT_TRUE (bp::SetUnitRooms (bp::QuickFor (plan, draft, plan.floors[1]), at, 4));
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds[at].rooms, 1.5);
    bp::ResetQuick (plan, draft, plan.floors[1]);
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[1]).seeds[at].rooms, 1.5);
    EXPECT_FALSE (bp::Dirty (draft));
}

TEST (HudFloorPlan, OrthogonalCornerAndCourtyardBarsJoinWithoutFillingTheCourtyard)
{
    auto floor = Floor ();
    floor.contours = { { Ring (0, 0, 36, 12) }, { Ring (0, 0, 12, 36) } };
    auto plan = bp::GenerateQuick (floor, { { 6, 3 } });
    ASSERT_TRUE (plan.ready) << plan.note;
    EXPECT_GT (plan.bars.size (), 1u);
    floor.contours = { { Ring (0, 0, 36, 36), Ring (12, 12, 12, 12) } };
    plan = bp::GenerateQuick (floor, { { 6, 3 } });
    ASSERT_TRUE (plan.ready) << plan.note;
    const cp::PathD hole { { 12, 12 }, { 24, 12 }, { 24, 24 }, { 12, 24 } };
    EXPECT_NEAR (cp::Area (cp::Intersect (Paths (plan.units), { hole }, cp::FillRule::NonZero, 6)), 0, 1e-6);
    EXPECT_NEAR (cp::Area (cp::Intersect (Paths (plan.corridors), { hole }, cp::FillRule::NonZero, 6)), 0, 1e-6);
}

TEST (HudFloorPlan, RotatedAndGeoreferencedOutlinesKeepExactWorldContours)
{
    auto floor = Floor ();
    const double angle = 0.35;
    const auto world = [&] (bp::Point p) {
        return bp::Point { 700000 + p.x * std::cos (angle) - p.y * std::sin (angle),
                           6000000 + p.x * std::sin (angle) + p.y * std::cos (angle) };
    };
    auto& ring = floor.contours.front ().front ();
    for (size_t i = 0; i < ring.Count (); ++i) {
        const auto p = world ({ ring.xy[i * 2], ring.xy[i * 2 + 1] });
        ring.xy[i * 2] = p.x;
        ring.xy[i * 2 + 1] = p.y;
    }
    const auto plan = bp::GenerateQuick (floor, { world ({ 6, 4 }) });
    ASSERT_TRUE (plan.ready) << plan.note;
    EXPECT_NEAR (Area (plan.bars), 576, 1e-3);
    for (const auto& seed : plan.seeds)
        EXPECT_TRUE (bp::Contains (floor, bp::UnitCenter (plan, seed)));
}

TEST (HudFloorPlan, OverlayAcknowledgesStairSaveWhenSelectionTabIsNotDrawingAndRoomEditsInvalidateCache)
{
    auto state = hud::NewState ();
    auto plan = Building ();
    state->floorPlanSnapshots[plan.key] = plan;
    state->previewUnits = true;
    geomsrv::archviz::overlaylayers::Layer stairs, units;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    auto& draft = state->buildingPlans[plan.key];
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    const size_t at = MultiSeed (quick);
    ASSERT_LT (at, quick.seeds.size ());
    ASSERT_TRUE (bp::SetUnitRooms (quick, at, 4));
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (std::any_of (units.polylines.begin (), units.polylines.end (),
                              [] (const auto& line) { return line.rgba == bp::UnitColour (4); }));
    draft.original.clear (); // Dirty local stair draft; the next snapshot acknowledges its saved points.
    ASSERT_TRUE (bp::Dirty (draft));
    state->floorPlanSnapshots[plan.key] = plan;
    hud::TakeFloorPlanLayers (*state, stairs, units);
    EXPECT_FALSE (bp::Conflict (plan, draft));
    EXPECT_FALSE (bp::Dirty (draft));
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds[at].rooms, 4);
}

TEST (HudFloorPlan, LockedAreasSurviveOtherRoomChangesDraggingAddDeleteAndCancellation)
{
    auto plan = bp::GenerateQuick (Floor (), { { 6, 3 } });
    const size_t at = MultiSeed (plan);
    ASSERT_LT (at, plan.seeds.size ());
    ASSERT_TRUE (bp::SetUnitRooms (plan, at, 1));
    ASSERT_TRUE (bp::SetUnitLocked (plan, at, true));
    const auto id = plan.seeds[at].id;
    size_t neighbour = at + 1;
    ASSERT_LT (neighbour, plan.seeds.size ());
    ASSERT_EQ (plan.seeds[at].segment, plan.seeds[neighbour].segment);
    ASSERT_TRUE (bp::SetUnitRooms (plan, neighbour, 3));
    EXPECT_NEAR (bp::UnitArea (plan.units[at]), 34, 1e-4);
    const auto seeds = plan.seeds;
    const auto units = plan.units;
    plan.selected = int (neighbour);
    plan.dragOriginal = plan.seeds[neighbour].along;
    plan.dragSeeds = seeds;
    plan.dragUnits = units;
    plan.dragging = true;
    auto centre = bp::UnitCenter (plan, plan.seeds[neighbour]);
    centre.x += 1.2 * std::cos (plan.angle);
    centre.y += 1.2 * std::sin (plan.angle);
    ASSERT_TRUE (bp::MoveUnit (plan, neighbour, centre));
    EXPECT_NEAR (bp::UnitArea (plan.units[at]), 34, 1e-4);
    bp::CancelUnits (plan);
    for (size_t i = 0; i < seeds.size (); ++i) {
        EXPECT_DOUBLE_EQ (plan.seeds[i].along, seeds[i].along);
        EXPECT_EQ (plan.units[i].rings.front ().xy, units[i].rings.front ().xy);
    }
    ASSERT_TRUE (bp::RemoveUnit (plan, neighbour));
    EXPECT_EQ (plan.seeds[at].id, id);
    EXPECT_NEAR (bp::UnitArea (plan.units[at]), 34, 1e-4);
}

TEST (HudFloorPlan, InfeasibleLocksRejectAtomicallyRatherThanShowWrongSizes)
{
    auto plan = bp::GenerateQuick (Floor (), { { 6, 3 } });
    ASSERT_TRUE (plan.ready);
    auto at = std::min_element (plan.units.begin (), plan.units.end (),
                                [] (const auto& a, const auto& b) { return bp::UnitArea (a) < bp::UnitArea (b); }) -
              plan.units.begin ();
    const double total = bp::UnitArea (plan.segments[plan.seeds[size_t (at)].segment]);
    ASSERT_LT (total, 82);
    const auto before = plan.seeds;
    const auto rings = plan.units[size_t (at)].rings.front ().xy;
    EXPECT_FALSE (bp::SetUnitRooms (plan, size_t (at), 4));
    EXPECT_EQ (plan.seeds[size_t (at)].rooms, before[size_t (at)].rooms);
    EXPECT_EQ (plan.units[size_t (at)].rings.front ().xy, rings);
    EXPECT_FALSE (plan.solveNote.empty ());
}

TEST (HudFloorPlan, FinalCorridorsNeverTouchExternalOrCourtyardFacadesAndFillsRespectHoles)
{
    auto floor = Floor ();
    floor.contours = { { Ring (0, 0, 36, 36), Ring (12, 12, 12, 12) } };
    const auto plan = bp::GenerateQuick (floor, { { 6, 3 } });
    ASSERT_TRUE (plan.ready) << plan.note;
    const cp::PathD outer { { 0, 0 }, { 36, 0 }, { 36, 36 }, { 0, 36 } };
    const cp::PathD hole { { 12, 12 }, { 24, 12 }, { 24, 24 }, { 12, 24 } };
    const auto safe = cp::Difference (cp::InflatePaths ({ outer }, -0.29, cp::JoinType::Miter, cp::EndType::Polygon),
                                      cp::InflatePaths ({ hole }, 0.29, cp::JoinType::Miter, cp::EndType::Polygon),
                                      cp::FillRule::NonZero, 6);
    EXPECT_NEAR (cp::Area (cp::Difference (Paths (plan.corridors), safe, cp::FillRule::NonZero, 6)), 0, 1e-5);
    for (const auto& corridor : plan.corridors)
        EXPECT_FALSE (corridor.triangles.empty ());
    for (const auto& unit : plan.units) {
        cp::PathsD triangles;
        for (size_t i = 0; i + 2 < unit.triangles.size (); i += 3)
            triangles.push_back ({ { unit.triangles[i].x, unit.triangles[i].y },
                                   { unit.triangles[i + 1].x, unit.triangles[i + 1].y },
                                   { unit.triangles[i + 2].x, unit.triangles[i + 2].y } });
        EXPECT_NEAR (cp::Area (cp::Intersect (triangles, { hole }, cp::FillRule::NonZero, 6)), 0, 1e-5);
        EXPECT_NEAR (std::abs (cp::Area (triangles)), bp::UnitArea (unit), 1e-4);
    }
}

TEST (HudFloorPlan, AllBuildingSnapshotsKeepSolidCorePreviewsAndEditsOnDeselectionButClearOnRemoval)
{
    hudtest::Watched gui;
    hud::OwnPages pages;
    const auto plan = Building ();
    pages.floorPlansKnown = true;
    pages.floorPlans = { plan };
    gui.engine.SetOwnPages (pages);
    gui.state->previewStairs = true;
    geomsrv::archviz::overlaylayers::Layer stairs, units;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*gui.state, stairs, units));
    ASSERT_EQ (stairs.meshes.size (), 3u);
    auto& draft = gui.state->buildingPlans.at (plan.key);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (bp::SetUnitLocked (quick, MultiSeed (quick), true));
    const auto count = draft.quickPlans.size ();
    pages.selection.known = true;
    pages.selection.count = 0;
    gui.engine.SetOwnPages (pages);
    EXPECT_EQ (gui.state->floorPlanSnapshots.size (), 1u);
    EXPECT_EQ (gui.state->buildingPlans.at (plan.key).quickPlans.size (), count);
    EXPECT_FALSE (hud::TakeFloorPlanLayers (*gui.state, stairs, units));
    EXPECT_EQ (stairs.meshes.size (), 3u);
    pages.floorPlans.clear ();
    gui.engine.SetOwnPages (pages);
    EXPECT_TRUE (hud::TakeFloorPlanLayers (*gui.state, stairs, units));
    EXPECT_TRUE (stairs.meshes.empty ());
    EXPECT_TRUE (gui.state->buildingPlans.empty ());
}

TEST (HudFloorPlan, ExactRoomTargetCanMoveCoreAtomicallyAcrossSharedAndUniqueFloors)
{
    bool moved = false;
    for (const auto core : { bp::Point { 6, 3 }, bp::Point { 12, 4 }, bp::Point { 18, 8 }, bp::Point { 28, 8 } }) {
        auto building = Building ();
        building.saved = { core };
        bp::Draft base;
        bp::Reset (building, base);
        const auto original = bp::QuickFor (building, base, building.floors[0]);
        if (!original.ready)
            continue;
        for (size_t i = 0; i < original.seeds.size () && !moved; ++i) {
            auto draft = base;
            auto direct = original;
            if (bp::SetUnitRooms (direct, i, 4))
                continue;
            bp::MakeUnique (building, draft, building.floors[1]);
            auto& unique = bp::QuickFor (building, draft, building.floors[1]);
            const size_t lock = MultiSeed (unique);
            ASSERT_LT (lock, unique.seeds.size ());
            ASSERT_TRUE (bp::SetUnitLocked (unique, lock, true));
            if (!bp::ChangeUnitTarget (building, draft, building.floors[0], i, 4, true)) {
                EXPECT_EQ (draft.points, base.points);
                continue;
            }
            moved = true;
            EXPECT_NE (draft.points, base.points);
            EXPECT_TRUE (bp::Dirty (draft));
            for (const auto& floor : building.floors) {
                EXPECT_TRUE (bp::Fits (floor, draft.points[0]));
                const auto& quick = bp::QuickFor (building, draft, floor);
                ASSERT_TRUE (quick.ready);
                for (size_t n = 0; n < quick.seeds.size (); ++n)
                    if (quick.seeds[n].locked)
                        EXPECT_NEAR (bp::UnitArea (quick.units[n]), bp::UnitTargetArea (quick.seeds[n].rooms), 1e-3);
            }
            EXPECT_NEAR (bp::UnitArea (bp::QuickFor (building, draft, building.floors[0]).units[i]), 82, 1e-3);
        }
        if (moved)
            break;
    }
    EXPECT_TRUE (moved) << "At least one infeasible initial segment should be solved by local core motion";
}

TEST (HudFloorPlan, CachedSpringEditsAreBoundedAndLockedAreaDoesNotDrift)
{
    auto plan = bp::GenerateQuick (Floor (), { { 6, 3 } });
    const size_t at = MultiSeed (plan);
    ASSERT_LT (at, plan.seeds.size ());
    ASSERT_TRUE (bp::SetUnitLocked (plan, at, true));
    const auto start = std::chrono::steady_clock::now ();
    for (int n = 0; n < 100; ++n) {
        auto p = bp::UnitCenter (plan, plan.seeds[at + 1]);
        p.x += (n % 2 ? -0.9 : 0.9) * std::cos (plan.angle);
        p.y += (n % 2 ? -0.9 : 0.9) * std::sin (plan.angle);
        bp::MoveUnit (plan, at + 1, p);
        EXPECT_NEAR (bp::UnitArea (plan.units[at]), 48, 1e-4);
    }
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
    RecordProperty ("meanEditMs", ms / 100);
    EXPECT_LT (ms, 5000) << "Offline guard against unbounded per-frame optimisation";
}

TEST (HudFloorPlan, BadCoreChangeWithholdsStaleLockedPlansAndRestoringCoreRecoversLocks)
{
    const auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    const size_t at = MultiSeed (quick);
    ASSERT_LT (at, quick.seeds.size ());
    ASSERT_TRUE (bp::SetUnitLocked (quick, at, true));
    draft.points[0] = { -10, -10 };
    const auto& invalid = bp::QuickFor (plan, draft, plan.floors[0]);
    EXPECT_FALSE (invalid.ready);
    EXPECT_TRUE (invalid.units.empty ());
    EXPECT_TRUE (invalid.corridors.empty ());
    EXPECT_TRUE (invalid.seeds[at].locked);
    draft.points = plan.saved;
    const auto& recovered = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (recovered.ready);
    EXPECT_TRUE (recovered.seeds[at].locked);
    EXPECT_NEAR (bp::UnitArea (recovered.units[at]), 48, 1e-4);
}
