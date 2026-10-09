#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include <clipper2/clipper.h>
#include <gtest/gtest.h>
#include <cmath>

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
    EXPECT_EQ (stairs.polylines.size (), 18u); // Bottom/top rectangles plus four vertical edges on each floor.
    EXPECT_GT (units.polylines.size (), 3u);
    EXPECT_DOUBLE_EQ (stairs.polylines[0].points[2], 0);
    EXPECT_DOUBLE_EQ (stairs.polylines[6].points[2], 3);
    EXPECT_DOUBLE_EQ (stairs.polylines[12].points[2], 6);
    EXPECT_FALSE (hud::TakeFloorPlanLayers (*state, stairs, units));
    auto& draft = state->buildingPlans.at (plan.key);
    ASSERT_TRUE (bp::RemoveUnit (bp::QuickFor (plan, draft, plan.floors[0]), 0));
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    state->previewStairs = state->previewUnits = false;
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (stairs.polylines.empty ());
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

TEST (HudFloorPlan, RoomWeightChoicesChangeAllocationWithoutMovingCentresOrChangingTotalArea)
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
        EXPECT_EQ (bp::UnitCenter (plan, plan.seeds[at]), center);
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
    ASSERT_TRUE (bp::SetUnitRooms (shared, 0, 1.5));
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[2]).seeds[0].rooms, 1.5);
    bp::MakeUnique (plan, draft, plan.floors[1]);
    ASSERT_TRUE (bp::SetUnitRooms (bp::QuickFor (plan, draft, plan.floors[1]), 0, 4));
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds[0].rooms, 1.5);
    bp::ResetQuick (plan, draft, plan.floors[1]);
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[1]).seeds[0].rooms, 1.5);
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
    ASSERT_TRUE (bp::SetUnitRooms (quick, 0, 4));
    ASSERT_TRUE (hud::TakeFloorPlanLayers (*state, stairs, units));
    EXPECT_TRUE (std::any_of (units.polylines.begin (), units.polylines.end (),
                              [] (const auto& line) { return line.rgba == bp::UnitColour (4); }));
    draft.original.clear (); // Dirty local stair draft; the next snapshot acknowledges its saved points.
    ASSERT_TRUE (bp::Dirty (draft));
    state->floorPlanSnapshots[plan.key] = plan;
    hud::TakeFloorPlanLayers (*state, stairs, units);
    EXPECT_FALSE (bp::Conflict (plan, draft));
    EXPECT_FALSE (bp::Dirty (draft));
    EXPECT_DOUBLE_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds[0].rooms, 4);
}
