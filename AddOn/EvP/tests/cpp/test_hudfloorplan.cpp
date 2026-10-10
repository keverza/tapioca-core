#include "ArchViz/HudBuildingPlan.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "hud_fixture.hpp"
#include "NodeGraph/Json.hpp"
#include <cstdlib>
#include <fstream>
#include <clipper2/clipper.h>
#include <gtest/gtest.h>
#include <cmath>
#include <chrono>
#include <sstream>

namespace bp = geomsrv::archviz::buildingplan;
namespace fp = geomsrv::archviz::floorprogramme;
namespace hud = geomsrv::archviz::overlayhud;
namespace cp = Clipper2Lib;
namespace {
constexpr double kNet = 0.05; // net areas converge within a few rounds of the wall deduction
geomsrv::archviz::SliceChain Ring (double x, double y, double w, double d)
{
    geomsrv::archviz::SliceChain ring;
    ring.closed = true;
    ring.xy = { x, y, x + w, y, x + w, y + d, x, y + d };
    return ring;
}
bp::Floor Floor (double w = 36, double d = 16)
{
    bp::Floor floor;
    floor.contours = { { Ring (0, 0, w, d) } };
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
    plan.saved = { { { 6, 2.1 } } };
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
// The flat beside `at` in its band, or the seed count when it has none.
size_t Neighbour (const bp::QuickPlan& plan, size_t at)
{
    size_t best = plan.seeds.size ();
    for (size_t i = 0; i < plan.seeds.size (); ++i)
        if (i != at && plan.seeds[i].segment == plan.seeds[at].segment &&
            (best == plan.seeds.size () || std::abs (plan.seeds[i].along - plan.seeds[at].along) <
                                               std::abs (plan.seeds[best].along - plan.seeds[at].along)))
            best = i;
    return best;
}
size_t ById (const bp::QuickPlan& plan, uint32_t id)
{
    for (size_t i = 0; i < plan.seeds.size (); ++i)
        if (plan.seeds[i].id == id)
            return i;
    return plan.seeds.size ();
}
std::string Diagnostic (const bp::QuickPlan& plan)
{
    std::ostringstream text;
    text << plan.note << " / " << plan.solveNote;
    for (size_t i = 0; i < plan.segments.size (); ++i) {
        const auto& s = plan.segments[i];
        bp::UnitSeed seed;
        seed.segment = i;
        seed.lo = s.lo;
        seed.hi = s.hi;
        const auto t = bp::Traits (plan, seed);
        text << "\nS" << i << " net=" << t.net << " depth=" << t.depth << " width=" << s.hi - s.lo
             << " facade=" << t.facade << " access=" << t.access;
    }
    return text.str ();
}
double CoreArea (const bp::Floor& floor, const std::vector<bp::Core>& cores, double angle)
{
    cp::PathsD rects;
    for (const auto& core : cores) {
        cp::PathD path;
        for (const auto& p : bp::Corners (core, angle))
            path.emplace_back (p.x, p.y);
        rects.push_back (path);
    }
    cp::PathsD outline;
    for (const auto& ring : floor.contours.front ()) {
        cp::PathD path;
        for (size_t i = 0; i < ring.Count (); ++i)
            path.emplace_back (ring.xy[i * 2], ring.xy[i * 2 + 1]);
        outline.push_back (path);
    }
    return std::abs (cp::Area (cp::Intersect (rects, outline, cp::FillRule::NonZero, 6)));
}
} // namespace

TEST (HudFloorPlan, StairsAutomaticallyProduceBarsCirculationBandsSegmentsAndUnits)
{
    const auto floor = Floor ();
    const auto plan = bp::GenerateQuick (floor, { { { 6, 2.1 } } });
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

TEST (HudFloorPlan, NoEmptyFloorFlatsCirculationAndCoresTileEveryOutline)
{
    struct Case {
        std::vector<std::vector<geomsrv::archviz::SliceChain>> contours;
        std::vector<bp::Core> cores;
        double area;
    };
    const std::vector<Case> cases {
        { { { Ring (0, 0, 36, 16) } }, { { { 6, 2.1 } } }, 576 },
        { { { Ring (0, 0, 36, 12) }, { Ring (0, 0, 12, 36) } }, { { { 2.1, 6 } } }, 36 * 12 + 12 * 24 },
        { { { Ring (0, 0, 36, 36), Ring (12, 12, 12, 12) } }, { { { 6, 2.1 } } }, 36 * 36 - 144 },
        { { { Ring (0, 0, 60, 16) } }, { { { 18, 2.1 } }, { { 42, 2.1 } } }, 960 },
    };
    for (const auto& item : cases) {
        bp::Floor floor;
        floor.contours = item.contours;
        const auto plan = bp::GenerateQuick (floor, item.cores);
        ASSERT_TRUE (plan.ready) << Diagnostic (plan);
        EXPECT_TRUE (plan.unassigned.empty ()) << "Empty floor left: " << Area (plan.unassigned);
        EXPECT_NEAR (Area (plan.units) + Area (plan.corridors) + CoreArea (floor, item.cores, plan.angle), item.area,
                     0.05);
    }
}

TEST (HudFloorPlan, BarEndsAcrossTheCorridorBecomeBandDeepCornerFlats)
{
    const auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready) << plan.note;
    ASSERT_EQ (plan.segments.size (), 3u) << "The full-width core landing divides one band; ends stay in flats";
    size_t corners = 0;
    for (const auto& seed : plan.seeds) {
        const auto traits = bp::Traits (plan, seed);
        EXPECT_GT (traits.facade, 1.2) << "Every flat has a window";
        EXPECT_GT (traits.access, 0.9) << "Every flat has a door onto circulation";
        corners += (traits.traits & bp::kCorner) != 0;
    }
    EXPECT_GE (corners, 3u) << "An absorbed small end cap can make one flat own both facade corners";
    for (const auto& seed : plan.seeds)
        if (bp::Traits (plan, seed).traits & bp::kCorner)
            EXPECT_GE (bp::Rooms (plan, seed), 3.0) << "Large flats take the corners";
}

TEST (HudFloorPlan, NetAreasDeductOneWallAcrossTheBandAndTheFacadeWallLikeThePrivateGenerator)
{
    const auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    for (size_t i = 0; i < plan.seeds.size (); ++i) {
        const auto traits = bp::Traits (plan, plan.seeds[i]);
        EXPECT_NEAR (traits.gross, bp::UnitArea (plan.units[i]), 0.01);
        EXPECT_NEAR (traits.net, traits.gross - fp::kWall * traits.depth - fp::kFacade * traits.facade, 1e-9);
        if (!(traits.traits & bp::kCorner)) {
            // A mid-band flat: one facade, its length the flat's width.
            EXPECT_NEAR (traits.facade, plan.seeds[i].hi - plan.seeds[i].lo, 0.31);
        }
    }
}

TEST (HudFloorPlan, ProgrammeFillCountsStayNearThePrivateGeneratorAndTheMix)
{
    // The private generator on one 60 x 16.2 m floor: 12 flats (rough count 13); 36 x 16.2 m: 6 (rough 8).
    struct Case {
        double width;
        std::vector<bp::Core> cores;
        size_t low, high;
    };
    for (const auto& item :
         { Case { 60, { { { 18, 2.1 } }, { { 42, 2.1 } } }, 11, 16 }, Case { 36, { { { 6, 2.1 } } }, 6, 10 } }) {
        const auto plan = bp::GenerateQuick (Floor (item.width, 16.2), item.cores);
        ASSERT_TRUE (plan.ready) << plan.note;
        EXPECT_GE (plan.seeds.size (), item.low);
        EXPECT_LE (plan.seeds.size (), item.high);
        std::vector<int> counts (plan.programme.types.size (), 0);
        for (const auto& seed : plan.seeds)
            ++counts[seed.type];
        const auto want = fp::Counts (plan.programme, int (plan.seeds.size ()));
        int deviation = 0;
        for (size_t t = 0; t < counts.size (); ++t)
            deviation += std::abs (counts[t] - want[t]);
        EXPECT_LE (deviation, int (plan.seeds.size () / 2)) << "Fill follows the programme shares";
        size_t fitting = 0;
        for (const auto& seed : plan.seeds) {
            const auto& type = plan.programme.types[seed.type];
            const double net = bp::Traits (plan, seed).net;
            fitting += net >= type.minM2 - 3 && net <= type.maxM2 + 3;
        }
        EXPECT_GE (fitting * 4, plan.seeds.size () * 3) << "Most flats within 3 m2 of their range";
    }
}

TEST (HudFloorPlan, UnitCentresNudgeActualBoundariesAddSplitDeleteRedistributeAndKeepIDs)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready) << plan.note;
    const size_t at = MultiSeed (plan);
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

TEST (HudFloorPlan, DeletingTheLastFlatOfABandIsRefusedRatherThanLeavingEmptyFloor)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    const size_t segment = plan.seeds.front ().segment;
    while (std::count_if (plan.seeds.begin (), plan.seeds.end (),
                          [&] (const auto& seed) { return seed.segment == segment; }) > 1) {
        const auto victim = std::find_if (plan.seeds.begin (), plan.seeds.end (),
                                          [&] (const auto& seed) { return seed.segment == segment; });
        ASSERT_TRUE (bp::RemoveUnit (plan, size_t (victim - plan.seeds.begin ())));
    }
    const auto last = std::find_if (plan.seeds.begin (), plan.seeds.end (),
                                    [&] (const auto& seed) { return seed.segment == segment; });
    EXPECT_FALSE (bp::RemoveUnit (plan, size_t (last - plan.seeds.begin ())));
    EXPECT_FALSE (plan.solveNote.empty ());
    EXPECT_NEAR (Area (plan.units), Area (plan.segments), 1e-4);
}

TEST (HudFloorPlan, InFlightUnitDragCancelsWithoutMetadataOrSeedCountChanges)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    plan.selected = int (MultiSeed (plan));
    plan.dragOriginal = plan.seeds[size_t (plan.selected)].along;
    plan.dragging = true;
    plan.owner = 42;
    auto p = bp::UnitCenter (plan, plan.seeds[size_t (plan.selected)]);
    p.x += 0.9 * std::cos (plan.angle);
    p.y += 0.9 * std::sin (plan.angle);
    ASSERT_TRUE (bp::MoveUnit (plan, size_t (plan.selected), p));
    const auto revision = plan.revision;
    EXPECT_FALSE (bp::MoveUnit (plan, size_t (plan.selected), p));
    EXPECT_EQ (plan.revision, revision) << "Idle held-pointer frames must not repeatedly solve the same input";
    EXPECT_FALSE (bp::RemoveUnit (plan, 0));
    bp::CancelUnits (plan);
    EXPECT_DOUBLE_EQ (plan.seeds[size_t (plan.selected)].along, plan.dragOriginal);
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
    ASSERT_TRUE (bp::RemoveUnit (first, MultiSeed (first)));
    const size_t sharedCount = first.seeds.size ();
    bp::MakeUnique (plan, draft, plan.floors[0]); // The shared representative itself becomes unique.
    auto& unique = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (bp::RemoveUnit (unique, MultiSeed (unique)));
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[1]).seeds.size (), sharedCount);
    EXPECT_EQ (unique.seeds.size (), sharedCount - 1);
    bp::ResetQuick (plan, draft, plan.floors[0]);
    EXPECT_FALSE (draft.uniqueFloors.contains (0));
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds.size (), sharedCount);
    EXPECT_EQ (&bp::QuickFor (plan, draft, plan.floors[0]), &bp::QuickFor (plan, draft, plan.floors[2]));
    EXPECT_FALSE (bp::Dirty (draft));
    EXPECT_TRUE (bp::Edits (plan, draft).empty ()) << "Unit editing must never persist stair metadata.";
}

TEST (HudFloorPlan, CacheIdentityRetainsWorldPlacementAndChangesOnHolesCoresProgrammeNotFloorIndex)
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
    EXPECT_NE (bp::QuickSignature (a, { { { 6, 3 } } }), bp::QuickSignature (a, { { { 7, 3 } } }));
    EXPECT_NE (bp::QuickSignature (a, { { { 6, 3 } } }), bp::QuickSignature (a, { { { 6, 3 }, 9, 2.5 } }));
    const auto programme = fp::Default ();
    auto other = programme;
    fp::SetShare (other, 0, 0.1);
    EXPECT_NE (bp::QuickSignature (a, {}, &programme), bp::QuickSignature (a, {}, &other));
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
    EXPECT_FALSE (bp::GenerateQuick (floor, { { { 1, 1 } } }).ready);
    EXPECT_FALSE (bp::GenerateQuick (floor, { { { 6, 3 } }, { { 7, 3 } } }).ready);
    floor.contours.push_back ({ Ring (50, 0, 36, 16) });
    plan = bp::GenerateQuick (floor, { { { 6, 2.1 } } });
    EXPECT_FALSE (plan.ready);
    EXPECT_TRUE (plan.units.empty ());
    EXPECT_NE (plan.note.find ("disconnected"), std::string::npos);
}

TEST (HudFloorPlan, StaleOutlineOrCoreChangesRegenerateUnitEditsButStairSaveAcknowledgementRetainsThem)
{
    auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (quick.ready);
    const size_t original = quick.seeds.size ();
    ASSERT_TRUE (bp::RemoveUnit (quick, MultiSeed (quick)));
    bp::Reset (plan, draft); // Fresh Save acknowledgement: same points and membership.
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds.size (), original - 1);
    draft.cores[0].center.x += 0.9;
    auto& changed = bp::QuickFor (plan, draft, plan.floors[0]);
    EXPECT_NE (changed.note.find ("regenerated"), std::string::npos);
    EXPECT_EQ (changed.seeds.size (),
               bp::GenerateQuick (plan.floors[0], draft.cores, draft.programme, plan.angle).seeds.size ());
}

TEST (HudFloorPlan, UnsupportedSlantedOutlineRefusesRatherThanSubstitutingBoundingRectangle)
{
    auto floor = Floor ();
    floor.contours.front ().front ().xy = { 0, 0, 36, 0, 30, 16, 0, 16 };
    const auto plan = bp::GenerateQuick (floor, { { { 6, 2.1 } } });
    EXPECT_FALSE (plan.ready);
    EXPECT_TRUE (plan.units.empty ());
    EXPECT_NE (plan.note.find ("orthogonal"), std::string::npos);
}

TEST (HudFloorPlan, ProgrammeTypesMeetNetTargetsAndRelaxCentresWithoutChangingTotalArea)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    const size_t at = MultiSeed (plan);
    ASSERT_LT (at, plan.seeds.size ());
    const auto id = plan.seeds[at].id;
    const double total = Area (plan.units);
    const auto start = plan.seeds[at].type;
    for (size_t type = 0; type < plan.programme.types.size (); ++type) {
        if (type == plan.seeds[at].type)
            continue;
        const auto center = bp::UnitCenter (plan, plan.seeds[at]);
        ASSERT_TRUE (bp::SetUnitType (plan, at, type)) << plan.solveNote;
        EXPECT_NEAR (bp::Traits (plan, plan.seeds[at]).net, fp::Target (plan.programme.types[type]), kNet);
        EXPECT_NE (bp::UnitCenter (plan, plan.seeds[at]), center);
        EXPECT_EQ (plan.seeds[at].id, id);
        EXPECT_NEAR (Area (plan.units), total, 1e-4);
    }
    EXPECT_DOUBLE_EQ (bp::TargetArea (plan, plan.seeds[at]), 85) << "5R exists now";
    EXPECT_FALSE (bp::SetUnitType (plan, at, plan.programme.types.size ()));
    EXPECT_TRUE (bp::SetUnitType (plan, at, start));
}

TEST (HudFloorPlan, SharedTypesCopyIntoUniqueFloorsAndResetRejoinsTheOriginalType)
{
    const auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& shared = bp::QuickFor (plan, draft, plan.floors[0]);
    const size_t at = MultiSeed (shared);
    ASSERT_LT (at, shared.seeds.size ());
    const size_t small = 0, large = 4;
    ASSERT_TRUE (bp::SetUnitType (shared, at, shared.seeds[at].type == small ? 1 : small));
    const auto chosen = shared.seeds[at].type;
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[2]).seeds[at].type, chosen);
    bp::MakeUnique (plan, draft, plan.floors[1]);
    ASSERT_TRUE (bp::SetUnitType (bp::QuickFor (plan, draft, plan.floors[1]), at, large));
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[0]).seeds[at].type, chosen);
    bp::ResetQuick (plan, draft, plan.floors[1]);
    EXPECT_EQ (bp::QuickFor (plan, draft, plan.floors[1]).seeds[at].type, chosen);
    EXPECT_FALSE (bp::Dirty (draft));
}

TEST (HudFloorPlan, OrthogonalCornerAndCourtyardBarsJoinWithoutFillingTheCourtyard)
{
    auto floor = Floor ();
    floor.contours = { { Ring (0, 0, 36, 12) }, { Ring (0, 0, 12, 36) } };
    auto plan = bp::GenerateQuick (floor, { { { 2.1, 6 } } });
    ASSERT_TRUE (plan.ready) << plan.note;
    EXPECT_GT (plan.bars.size (), 1u);
    floor.contours = { { Ring (0, 0, 36, 36), Ring (12, 12, 12, 12) } };
    plan = bp::GenerateQuick (floor, { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready) << plan.note;
    const cp::PathD hole { { 12, 12 }, { 24, 12 }, { 24, 24 }, { 12, 24 } };
    EXPECT_NEAR (cp::Area (cp::Intersect (Paths (plan.units), { hole }, cp::FillRule::NonZero, 6)), 0, 1e-6);
    EXPECT_NEAR (cp::Area (cp::Intersect (Paths (plan.corridors), { hole }, cp::FillRule::NonZero, 6)), 0, 1e-6);
}

TEST (HudFloorPlan, RotatedAndGeoreferencedOutlinesKeepExactWorldContoursAndTurnTheCores)
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
    const auto plan = bp::GenerateQuick (floor, { { world ({ 6, 2.1 }) } });
    ASSERT_TRUE (plan.ready) << plan.note;
    EXPECT_NEAR (std::remainder (plan.angle - angle, 3.141592653589793), 0, 1e-9) << "Either long edge";
    EXPECT_NEAR (Area (plan.bars), 576, 1e-3);
    for (const auto& seed : plan.seeds)
        EXPECT_TRUE (bp::Contains (floor, bp::UnitCenter (plan, seed)));
    // A 9 m straight stair along the facade fits only turned with the building.
    const bp::Core linear { world ({ 8, 1.25 }), 9.0, 2.5 };
    EXPECT_TRUE (bp::Fits (floor, linear, angle));
    EXPECT_FALSE (bp::Fits (floor, linear, 0));
    const auto straight = bp::GenerateQuick (floor, { linear });
    EXPECT_TRUE (straight.ready) << Diagnostic (straight);
}

TEST (HudFloorPlan, LockedAreasSurviveOtherTypeChangesDraggingAddDeleteAndCancellation)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    const size_t at = MultiSeed (plan);
    ASSERT_LT (at, plan.seeds.size ());
    if (plan.seeds[at].type != 0)
        ASSERT_TRUE (bp::SetUnitType (plan, at, 0));
    ASSERT_TRUE (bp::SetUnitLocked (plan, at, true));
    const auto id = plan.seeds[at].id;
    const size_t neighbour = Neighbour (plan, at);
    ASSERT_LT (neighbour, plan.seeds.size ());
    ASSERT_TRUE (bp::SetUnitType (plan, neighbour, plan.seeds[neighbour].type == 2 ? 3 : 2));
    EXPECT_NEAR (bp::Traits (plan, plan.seeds[at]).net, 33, kNet);
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
    EXPECT_NEAR (bp::Traits (plan, plan.seeds[at]).net, 33, kNet);
    bp::CancelUnits (plan);
    for (size_t i = 0; i < seeds.size (); ++i) {
        EXPECT_DOUBLE_EQ (plan.seeds[i].along, seeds[i].along);
        EXPECT_EQ (plan.units[i].rings.front ().xy, units[i].rings.front ().xy);
    }
    ASSERT_TRUE (bp::RemoveUnit (plan, neighbour));
    const size_t kept = ById (plan, id);
    ASSERT_LT (kept, plan.seeds.size ());
    EXPECT_NEAR (bp::Traits (plan, plan.seeds[kept]).net, 33, kNet);
}

TEST (HudFloorPlan, InfeasibleLocksRejectAtomicallyRatherThanShowWrongSizes)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    const size_t segment = plan.seeds[MultiSeed (plan)].segment;
    // Lock every flat of one band as a 5R until the band cannot hold the next one.
    bool refused = false;
    for (size_t i = 0; i < plan.seeds.size () && !refused; ++i) {
        if (plan.seeds[i].segment != segment)
            continue;
        const auto before = plan.seeds;
        const auto rings = plan.units[i].rings.front ().xy;
        if ((plan.seeds[i].type == 5 || bp::SetUnitType (plan, i, 5)) && bp::SetUnitLocked (plan, i, true))
            continue;
        refused = true;
        EXPECT_FALSE (plan.solveNote.empty ());
        if (plan.seeds[i].type == before[i].type) {
            EXPECT_FALSE (plan.seeds[i].locked) << "A refused lock leaves the flat as it was";
            EXPECT_EQ (plan.units[i].rings.front ().xy, rings);
        }
    }
    EXPECT_TRUE (refused);
}

TEST (HudFloorPlan, FinalCorridorsNeverTouchExternalOrCourtyardFacadesAndFillsRespectHoles)
{
    auto floor = Floor ();
    floor.contours = { { Ring (0, 0, 36, 36), Ring (12, 12, 12, 12) } };
    const auto plan = bp::GenerateQuick (floor, { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready) << Diagnostic (plan);
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

TEST (HudFloorPlan, InfeasibleTargetEitherMovesTheCoreAtomicallyOrLeavesTheDraftUntouched)
{
    auto building = Building ();
    bp::Draft draft;
    bp::Reset (building, draft);
    auto& quick = bp::QuickFor (building, draft, building.floors[0]);
    ASSERT_TRUE (quick.ready);
    const size_t at = MultiSeed (quick);
    for (size_t i = 0; i < quick.seeds.size (); ++i)
        if (i != at && quick.seeds[i].segment == quick.seeds[at].segment)
            ASSERT_TRUE (bp::SetUnitLocked (quick, i, true));
    const auto cores = draft.cores;
    const auto seeds = quick.seeds;
    if (!bp::ChangeUnitTarget (building, draft, building.floors[0], at, 5, true)) {
        EXPECT_EQ (draft.cores, cores);
        EXPECT_EQ (bp::QuickFor (building, draft, building.floors[0]).seeds.size (), seeds.size ());
        return;
    }
    const auto& after = bp::QuickFor (building, draft, building.floors[0]);
    ASSERT_TRUE (after.ready);
    // With every flat of the band locked they stretch to fill it (no empty floor), never shrink.
    for (const auto& seed : after.seeds)
        if (seed.locked)
            EXPECT_GE (bp::Traits (after, seed).net, bp::TargetArea (after, seed) - kNet);
}

TEST (HudFloorPlan, CachedSpringEditsAreBoundedAndLockedAreaDoesNotDrift)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    const size_t at = MultiSeed (plan);
    ASSERT_LT (at, plan.seeds.size ());
    ASSERT_TRUE (bp::SetUnitLocked (plan, at, true));
    const double target = bp::TargetArea (plan, plan.seeds[at]);
    const size_t other = Neighbour (plan, at);
    ASSERT_LT (other, plan.seeds.size ());
    const auto start = std::chrono::steady_clock::now ();
    for (int n = 0; n < 100; ++n) {
        auto p = bp::UnitCenter (plan, plan.seeds[other]);
        p.x += (n % 2 ? -0.9 : 0.9) * std::cos (plan.angle);
        p.y += (n % 2 ? -0.9 : 0.9) * std::sin (plan.angle);
        bp::MoveUnit (plan, other, p);
        EXPECT_NEAR (bp::Traits (plan, plan.seeds[at]).net, target, kNet);
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
    const auto id = quick.seeds[at].id;
    const double target = bp::TargetArea (quick, quick.seeds[at]);
    draft.cores[0].center = { -10, -10 };
    const auto& invalid = bp::QuickFor (plan, draft, plan.floors[0]);
    EXPECT_FALSE (invalid.ready);
    EXPECT_TRUE (invalid.units.empty ());
    EXPECT_TRUE (invalid.corridors.empty ());
    EXPECT_TRUE (invalid.seeds[at].locked);
    draft.cores = plan.saved;
    const auto& recovered = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (recovered.ready);
    const size_t kept = ById (recovered, id);
    ASSERT_LT (kept, recovered.seeds.size ());
    EXPECT_TRUE (recovered.seeds[kept].locked);
    EXPECT_NEAR (bp::Traits (recovered, recovered.seeds[kept]).net, target, kNet);
}

TEST (HudFloorPlan, SemanticLocksKeepCornerAndSizeThroughRegenerateAndProgrammeEdits)
{
    const auto plan = Building ();
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    size_t corner = quick.seeds.size ();
    for (size_t i = 0; i < quick.seeds.size () && corner == quick.seeds.size (); ++i)
        if ((bp::Traits (quick, quick.seeds[i]).traits & bp::kCorner) && Neighbour (quick, i) < quick.seeds.size ())
            corner = i;
    ASSERT_LT (corner, quick.seeds.size ());
    ASSERT_TRUE (bp::SetUnitLocked (quick, corner, true));
    EXPECT_TRUE (quick.seeds[corner].keep & bp::kCorner) << "Locking keeps the traits it has";
    const auto id = quick.seeds[corner].id;
    const auto type = quick.seeds[corner].type;
    ASSERT_TRUE (bp::Regenerate (plan, draft, plan.floors[0]));
    auto& fresh = bp::QuickFor (plan, draft, plan.floors[0]);
    size_t kept = ById (fresh, id);
    ASSERT_LT (kept, fresh.seeds.size ());
    EXPECT_EQ (fresh.seeds[kept].type, type);
    EXPECT_TRUE (bp::Traits (fresh, fresh.seeds[kept]).traits & bp::kCorner);
    EXPECT_NEAR (bp::Traits (fresh, fresh.seeds[kept]).net, bp::TargetArea (fresh, fresh.seeds[kept]), kNet);
    // A programme edit regenerates the unlocked flats; the locked one keeps its place and the
    // type whose target is nearest its own.
    auto programme = draft.programme;
    ASSERT_TRUE (fp::SetShare (programme, 1, 0.45));
    bp::UseProgramme (draft, programme);
    auto& edited = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (edited.ready);
    kept = ById (edited, id);
    ASSERT_LT (kept, edited.seeds.size ());
    EXPECT_TRUE (edited.seeds[kept].locked);
    EXPECT_TRUE (bp::Traits (edited, edited.seeds[kept]).traits & bp::kCorner);
    ASSERT_TRUE (bp::SetUnitKeep (edited, kept, bp::kCorner | bp::kStraightFacade));
    EXPECT_GT (bp::Score (edited, edited.seeds), 0) << "A kept trait the flat lacks costs score";
}

TEST (HudFloorPlan, OptimiseImprovesTheScoreAndMovesACoreOutOfADeadEndOnlyWhenAllowed)
{
    bp::Plan plan;
    plan.key = "building:Slab";
    auto floor = Floor (60, 12);
    plan.floors = { floor };
    plan.saved = { { { 6, 2.1 } } };
    bp::Draft draft;
    bp::Reset (plan, draft);
    auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (quick.ready) << quick.note;
    ASSERT_FALSE (quick.egress.invalid.empty ()) << "One stair at a 60 m slab's end: a dead end over 25 m";
    const double before = quick.score;
    const size_t invalid = quick.egress.invalid.size ();
    draft.moveCores = false;
    ASSERT_TRUE (bp::Optimise (plan, draft, plan.floors[0]));
    EXPECT_EQ (draft.cores, plan.saved);
    EXPECT_LE (bp::QuickFor (plan, draft, plan.floors[0]).score, before + 0.01);
    draft.moveCores = true;
    ASSERT_TRUE (bp::Optimise (plan, draft, plan.floors[0]));
    EXPECT_NE (draft.cores, plan.saved);
    EXPECT_GT (draft.cores[0].center.x, 6) << "Towards the middle of the corridor";
    EXPECT_TRUE (bp::Dirty (draft)) << "A moved core waits for Save stairwells";
    const auto& after = bp::QuickFor (plan, draft, plan.floors[0]);
    ASSERT_TRUE (after.ready);
    EXPECT_LT (after.egress.invalid.size (), invalid);
    EXPECT_LT (after.score, before);
}

TEST (HudFloorPlan, EgressWalksTheCorridorLikeThePrivateGenerator)
{
    // 36 x 16: corridor 3.4..32.6 at mid depth; one stair at the low end reaches every cell.
    const auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    EXPECT_GT (plan.egress.cells, 100u);
    // Up the connector from the core (y 5.1) to the spine (y 8), then along it to x 32.6, on the grid.
    EXPECT_NEAR (plan.egress.longest, (8.9 - 4.2) + (32.6 - 8.25), 0.61);
    EXPECT_FALSE (plan.egress.invalid.empty ()) << "Beyond 25 m from the only stair";
    const auto two = bp::GenerateQuick (Floor (), { { { 6, 2.1 } }, { { 30, 2.1 } } });
    ASSERT_TRUE (two.ready);
    EXPECT_TRUE (two.egress.invalid.empty ()) << "Two stairs: within 40 m of one, a second way out";
}

TEST (HudFloorPlan, ProgrammeFromTheProjectIsAdoptedUnlessAnUnsavedEditIsWaiting)
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

TEST (HudFloorPlan, EveryEditAndRegeneratedAlternativeKeepsNetAreaDepthFacadeAndEntranceFloors)
{
    for (double depth : { 11.0, 12.0, 16.0 }) {
        auto plan = Building ();
        plan.floors = { Floor (36, depth) };
        bp::Draft draft;
        bp::Reset (plan, draft);
        for (int alternative = 0; alternative < 5; ++alternative) {
            auto& quick = bp::QuickFor (plan, draft, plan.floors[0]);
            ASSERT_TRUE (quick.ready) << Diagnostic (quick);
            for (const auto& seed : quick.seeds) {
                const auto t = bp::Traits (quick, seed);
                EXPECT_GE (t.net, 25 - 1e-4);
                EXPECT_GE (t.depth, bp::kMinUnitDepth - 0.01);
                EXPECT_GE (t.access, 0.9 - 0.01);
                EXPECT_GE (t.facade, 1.2 - 0.01);
            }
            const auto oldUnits = Paths (quick.units);
            const auto oldCount = quick.seeds.size ();
            ASSERT_TRUE (bp::Regenerate (plan, draft, plan.floors[0])) << quick.solveNote;
            const auto& next = bp::QuickFor (plan, draft, plan.floors[0]);
            // Retyping alone is not enough: actual cuts, polygons or count must change.
            EXPECT_TRUE (next.seeds.size () != oldCount || Paths (next.units) != oldUnits);
        }
    }
}

TEST (HudFloorPlan, ShortCoreConnectorIsAFullWidthLandingAndDoesNotInventJaggedCirculation)
{
    const auto plan = bp::GenerateQuick (Floor (36, 12), { { { 18, 2.1 } } }, fp::Default (), 0);
    ASSERT_TRUE (plan.ready) << Diagnostic (plan);
    const cp::PathD landing { { 15.75, 4.2 }, { 20.25, 4.2 }, { 20.25, 5.1 }, { 15.75, 5.1 } };
    EXPECT_NEAR (cp::Area (cp::Difference ({ landing }, Paths (plan.corridors), cp::FillRule::NonZero, 6)), 0, 1e-6);
    const cp::PathD main { { 3.4, 5.1 }, { 32.6, 5.1 }, { 32.6, 6.9 }, { 3.4, 6.9 } };
    const auto intended = cp::Union ({ landing, main }, cp::FillRule::NonZero, 6);
    EXPECT_NEAR (cp::Area (cp::Xor (Paths (plan.corridors), intended, cp::FillRule::NonZero, 6)), 0, 1e-5)
        << "Only the straight spine and rectangular core landing are circulation";
    EXPECT_EQ (bp::UnitAt (plan, { 18, 5.5 }), -1);
    EXPECT_EQ (bp::UnitAt (plan, { 18, 2.1 }), -1);
    for (size_t i = 0; i < plan.units.size (); ++i)
        EXPECT_EQ (bp::UnitAt (plan, plan.units[i].center), int (i));
}

TEST (HudFloorPlan, BelowMinimumProgrammeCannotForceAnUndersizedApartmentAndAreaColoursIgnoreType)
{
    auto plan = bp::GenerateQuick (Floor (), { { { 6, 2.1 } } });
    ASSERT_TRUE (plan.ready);
    const size_t at = MultiSeed (plan);
    ASSERT_LT (at, plan.seeds.size ());
    const auto old = plan.units[at].rings.front ().xy;
    plan.programme.types.push_back ({ 1, 15, 20, 0 });
    EXPECT_FALSE (bp::SetUnitType (plan, at, plan.programme.types.size () - 1));
    EXPECT_EQ (plan.units[at].rings.front ().xy, old);
    EXPECT_NE (bp::AreaColour (33), bp::AreaColour (60));
    EXPECT_EQ (bp::AreaColour (60), bp::AreaColour (70));
    EXPECT_NE (bp::AreaColour (70), bp::AreaColour (80));
}

TEST (HudFloorPlan, OffsetOutlineKeepsParallelSpinesAlignedInsteadOfZigzaggingAcrossSweepSeams)
{
    auto floor = Floor ();
    floor.contours = { { Ring (0, 0, 18, 14) }, { Ring (18, 0, 12, 10) } };
    const auto plan = bp::GenerateQuick (floor, { { { 9, 2.1 } } }, fp::Default (), 0);
    ASSERT_TRUE (plan.ready) << Diagnostic (plan);
    const cp::PathD expected { { 3.4, 4.1 }, { 26.6, 4.1 }, { 26.6, 5.9 }, { 3.4, 5.9 } };
    const cp::PathD core { { 6.75, 0.0 }, { 11.25, 0.0 }, { 11.25, 4.2 }, { 6.75, 4.2 } };
    const auto spine = cp::Difference ({ expected }, { core }, cp::FillRule::NonZero, 6);
    EXPECT_NEAR (cp::Area (cp::Difference (spine, Paths (plan.corridors), cp::FillRule::NonZero, 6)), 0, 1e-5);
    const cp::PathD end { { 26.6, 0.0 }, { 30.0, 0.0 }, { 30.0, 10.0 }, { 26.6, 10.0 } };
    EXPECT_NEAR (cp::Area (cp::Intersect ({ end }, Paths (plan.corridors), cp::FillRule::NonZero, 6)), 0, 1e-5);
}
