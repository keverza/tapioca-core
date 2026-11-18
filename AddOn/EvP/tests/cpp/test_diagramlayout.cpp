#include "ArchViz/DiagramLayout.hpp"
#include "ArchViz/FloorPlanner.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>

namespace dg = geomsrv::archviz::diagram;
static_assert (dg::kStoreyTolerance == geomsrv::archviz::buildingplan::kElevationTolerance,
               "storey binding tolerance must equal the planner's elevation tolerance");
namespace {
dg::EntityId Building (const std::string& key)
{
    return { dg::Level::Building, "building:" + key };
}
dg::EntityId Storey (const std::string& building, int n)
{
    return { dg::Level::Storey, "storey:building:" + building + ":" + std::to_string (n) };
}
dg::Entity Make (dg::EntityId id, dg::EntityId parent, dg::Vec3 lo, dg::Vec3 hi, bool planned = true)
{
    dg::Entity e;
    e.id = std::move (id);
    e.parent = std::move (parent);
    e.boundsMin = lo;
    e.boundsMax = hi;
    e.planned = planned;
    return e;
}
// One building at x0, storeys of the given heights stacked from z = 0.
void AddBuilding (dg::Scene& scene, const std::string& key, double x0, const std::vector<double>& heights,
                  double width = 10)
{
    double z = 0;
    for (double h : heights)
        z += h;
    scene.entities.push_back (Make (Building (key), {}, { x0, 0, 0 }, { x0 + width, 10, z }));
    z = 0;
    for (size_t i = 0; i < heights.size (); ++i) {
        scene.entities.push_back (
            Make (Storey (key, int (i)), Building (key), { x0, 0, z }, { x0 + width, 10, z + heights[i] }, i != 2));
        z += heights[i];
    }
}
dg::LayoutRequest Linear ()
{
    dg::LayoutRequest request;
    request.rules = dg::DefaultLayoutRules ();
    request.rules.easing = dg::Easing::Linear;
    return request;
}
// The packing rules the algorithm tests were measured against, independent of the defaults.
dg::LayoutRequest Measured ()
{
    auto request = Linear ();
    request.rules.building.strategy = dg::Strategy::Horizontal;
    request.rules.building.gapFraction = 0.25;
    request.rules.building.minGapMetres = 2.0;
    request.rules.building.maxGapMetres = 20.0;
    request.rules.storey.gapFraction = 0.5;
    request.rules.storey.minGapMetres = 0.6;
    request.rules.storey.maxGapMetres = 6.0;
    return request;
}
const dg::NodePose& PoseOf (const dg::Frame& frame, const dg::EntityId& id)
{
    for (const auto& pose : frame.poses)
        if (pose.id == id)
            return pose;
    throw std::runtime_error ("no pose " + id.key);
}
dg::PickMesh Box (const dg::EntityId& id, dg::Vec3 half)
{
    dg::PickMesh m;
    m.id = id;
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 3; ++k)
            m.positions.push_back ((i >> k & 1) ? half[size_t (k)] : -half[size_t (k)]);
    m.indices = { 0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                  2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5 };
    return m;
}
} // namespace

TEST (DiagramLayout, AssembledPoseReproducesSource)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 2.2, 3.0, 4.5, 3.0 });
    const auto plan = dg::Plan (scene, Linear ());
    const auto frame = dg::Evaluate (plan, {}, 0.0);
    for (const auto& pose : frame.poses)
        for (double v : pose.offset)
            EXPECT_EQ (v, 0.0) << pose.id.key;
    EXPECT_EQ (frame.boundsMin, (dg::Vec3 { 0, 0, 0 }));
    EXPECT_NEAR (frame.boundsMax[2], 12.7, 1e-12);
}

TEST (DiagramLayout, IrregularStoreysSeparateByTheirOwnExtent)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 2.2, 3.0, 4.5, 3.0 });
    const auto plan = dg::Plan (scene, Measured ());
    const auto frame = dg::Evaluate (plan, {}, 1.0);
    // gap = clamp (0.5 * previous height, 0.6, 6.0): 1.1, 1.5, 2.25.
    const double expected[] = { 1.1, 1.5, 2.25 };
    for (int i = 0; i < 3; ++i) {
        const int a = plan.Find (Storey ("A", i)), b = plan.Find (Storey ("A", i + 1));
        const double top = plan.nodes[size_t (a)].boundsMax[2] + frame.poses[size_t (a)].offset[2];
        const double bottom = plan.nodes[size_t (b)].boundsMin[2] + frame.poses[size_t (b)].offset[2];
        EXPECT_NEAR (bottom - top, expected[i], 1e-12) << i;
    }
    for (const auto& pose : frame.poses) {
        EXPECT_EQ (pose.offset[0], 0.0);
        EXPECT_EQ (pose.offset[1], 0.0);
    }
}

TEST (DiagramLayout, DefaultsExplodeUpwardOnly)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3, 3 });
    AddBuilding (scene, "B", 5, { 3, 3 }); // overlaps A in x: still not moved sideways
    const auto plan = dg::Plan (scene, Linear ());
    const auto frame = dg::Evaluate (plan, {}, 1.0);
    for (const auto& pose : frame.poses) {
        EXPECT_EQ (pose.offset[0], 0.0) << pose.id.key;
        EXPECT_EQ (pose.offset[1], 0.0) << pose.id.key;
    }
    // One storey height between plates: 3 m storeys lift by 3 and 6.
    EXPECT_NEAR (PoseOf (frame, Storey ("A", 1)).offset[2], 3.0, 1e-12);
    EXPECT_NEAR (PoseOf (frame, Storey ("A", 2)).offset[2], 6.0, 1e-12);
    EXPECT_NEAR (PoseOf (frame, Storey ("B", 1)).offset[2], 3.0, 1e-12);
}

TEST (DiagramLayout, GapClampHasMinimumFirstAndMaximumLast)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 0.5, 20.0, 3.0 });
    const auto plan = dg::Plan (scene, Measured ());
    // 0.5 * 0.5 = 0.25 -> min 0.6; 0.5 * 20 = 10 -> max 6.
    EXPECT_NEAR (plan.nodes[size_t (plan.Find (Storey ("A", 1)))].target[2], 0.6, 1e-12);
    EXPECT_NEAR (plan.nodes[size_t (plan.Find (Storey ("A", 2)))].target[2], 6.6, 1e-12);
}

TEST (DiagramLayout, ReversingTDoesNotDrift)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3, 3, 3 });
    AddBuilding (scene, "B", 5, { 4, 4 });
    auto request = Linear ();
    request.rules.easing = dg::Easing::InOutCubic;
    request.rules.staggerFraction = 0.3;
    const auto plan = dg::Plan (scene, request);
    const auto reference = dg::Evaluate (plan, {}, 0.6);
    double t = 0;
    for (int i = 0; i < 10000; ++i) {
        t = i % 2 ? 0.0 : 1.0;
        (void) dg::Evaluate (plan, {}, t);
    }
    const auto again = dg::Evaluate (plan, {}, 0.6);
    ASSERT_EQ (again.poses.size (), reference.poses.size ());
    for (size_t i = 0; i < again.poses.size (); ++i)
        EXPECT_EQ (again.poses[i].offset, reference.poses[i].offset);
    // Stagger ends exactly at t = 1 for every sibling.
    const auto end = dg::Evaluate (plan, {}, 1.0);
    for (size_t i = 0; i < end.poses.size (); ++i)
        for (int k = 0; k < 3; ++k)
            EXPECT_DOUBLE_EQ (
                end.poses[i].offset[size_t (k)] -
                    (plan.nodes[i].parent < 0 ? 0.0 : end.poses[size_t (plan.nodes[i].parent)].offset[size_t (k)]),
                plan.nodes[i].target[size_t (k)]);
}

TEST (DiagramLayout, InputOrderDoesNotChangeTheFrame)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 2.2, 3.0, 4.5 });
    AddBuilding (scene, "B", 4, { 3.0, 3.0 });
    dg::Scene reversed = scene;
    std::reverse (reversed.entities.begin (), reversed.entities.end ());
    const auto a = dg::Evaluate (dg::Plan (scene, Linear ()), {}, 0.6);
    const auto b = dg::Evaluate (dg::Plan (reversed, Linear ()), {}, 0.6);
    ASSERT_EQ (a.poses.size (), b.poses.size ());
    for (size_t i = 0; i < a.poses.size (); ++i) {
        EXPECT_EQ (a.poses[i].id, b.poses[i].id);
        EXPECT_EQ (a.poses[i].offset, b.poses[i].offset);
    }
}

TEST (DiagramLayout, BuildingsSpreadAndStoreysComposeOnTop)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3 });
    AddBuilding (scene, "B", 5, { 3, 3 }); // overlaps A in x
    const auto plan = dg::Plan (scene, Measured ());
    const auto frame = dg::Evaluate (plan, {}, 1.0);
    // B moves to A's end (10) plus clamp (0.25 * 10, 2, 20) = 2.5: x0 12.5, offset 7.5.
    EXPECT_NEAR (PoseOf (frame, Building ("B")).offset[0], 7.5, 1e-12);
    const auto& top = PoseOf (frame, Storey ("B", 1));
    EXPECT_NEAR (top.offset[0], 7.5, 1e-12);
    EXPECT_NEAR (top.offset[2], 1.5, 1e-12);
    dg::Vec3 lo, hi;
    dg::FramingBounds (plan, lo, hi);
    EXPECT_LE (lo[0], 0.0);
    EXPECT_GE (hi[0], 22.5);
    EXPECT_GE (hi[2], 7.5);
}

TEST (DiagramLayout, SelectiveScopeLeavesOtherSubtreesAtSource)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3 });
    AddBuilding (scene, "B", 20, { 3, 3 });
    auto request = Linear ();
    request.rules.building.strategy = dg::Strategy::None;
    request.scope.explode = { Building ("A") };
    const auto frame = dg::Evaluate (dg::Plan (scene, request), {}, 1.0);
    EXPECT_GT (PoseOf (frame, Storey ("A", 1)).offset[2], 0.0);
    EXPECT_EQ (PoseOf (frame, Storey ("B", 1)).offset, (dg::Vec3 {}));
}

TEST (DiagramLayout, ExactlyOneOwnerPerRegion)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3, 3 }); // storey 2 is unplanned
    const dg::EntityId flat { dg::Level::Apartment, "apartment:building:A:7" };
    const dg::EntityId shellFlat { dg::Level::Apartment, "apartment:building:A:9" };
    scene.entities.push_back (Make (flat, Storey ("A", 0), { 0, 0, 0 }, { 5, 10, 3 }));
    scene.entities.push_back (Make (shellFlat, Storey ("A", 2), { 0, 0, 6 }, { 5, 10, 9 }));
    auto request = Linear ();
    request.scope.expanded = { Building ("A"), Storey ("A", 0), Storey ("A", 2) };
    const auto plan = dg::Plan (scene, request);
    auto visible = [&] (const dg::EntityId& id) { return plan.nodes[size_t (plan.Find (id))].visible; };
    EXPECT_FALSE (visible (Building ("A")));
    EXPECT_FALSE (visible (Storey ("A", 0)));
    EXPECT_TRUE (visible (flat));
    EXPECT_TRUE (visible (Storey ("A", 1)));
    // An unplanned storey stays a shell; its child is never shown.
    EXPECT_TRUE (visible (Storey ("A", 2)));
    EXPECT_FALSE (visible (shellFlat));
    for (size_t i = 0; i < plan.nodes.size (); ++i)
        for (int p = plan.nodes[i].parent; p >= 0; p = plan.nodes[size_t (p)].parent)
            EXPECT_FALSE (plan.nodes[i].visible && plan.nodes[size_t (p)].visible) << plan.nodes[i].id.key;
    EXPECT_EQ (plan.nodes[size_t (plan.Find (flat))].representation, dg::Representation::CellVolume);
}

TEST (DiagramLayout, OffsetCapIsReportedNotExceeded)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3, 3 });
    auto request = Linear ();
    request.rules.storey.maxOffsetMetres = 2.0;
    const auto plan = dg::Plan (scene, request);
    for (const auto& node : plan.nodes)
        EXPECT_LE (node.target[2], 2.0);
    EXPECT_FALSE (plan.notes.empty ());
}

TEST (DiagramLayout, UnknownParentAndCycleAreReported)
{
    dg::Scene scene;
    scene.entities.push_back (Make (Storey ("X", 0), Building ("missing"), { 0, 0, 0 }, { 1, 1, 1 }));
    scene.entities.push_back (Make (Building ("P"), Building ("Q"), { 0, 0, 0 }, { 1, 1, 1 }));
    scene.entities.push_back (Make (Building ("Q"), Building ("P"), { 0, 0, 0 }, { 1, 1, 1 }));
    const auto plan = dg::Plan (scene, Linear ());
    EXPECT_GE (plan.Find (Storey ("X", 0)), 0);
    EXPECT_LT (plan.Find (Building ("P")), 0);
    EXPECT_EQ (plan.notes.size (), 3u);
}

TEST (DiagramLayout, PickHitsTheDisplacedStoreyNotItsSource)
{
    dg::Scene scene;
    AddBuilding (scene, "A", 0, { 3, 3 });
    const auto plan = dg::Plan (scene, Linear ());
    const auto frame = dg::Evaluate (plan, {}, 1.0); // storey 1 now spans z 4.5..7.5
    std::vector<dg::PickMesh> meshes = { Box (Building ("A"), { 5, 5, 3 }), Box (Storey ("A", 0), { 5, 5, 1.5 }),
                                         Box (Storey ("A", 1), { 5, 5, 1.5 }) };
    auto hit = dg::Pick (frame, plan, meshes, { 5, -50, 6.0 }, { 0, 1, 0 });
    ASSERT_TRUE (hit);
    EXPECT_EQ (hit->id, Storey ("A", 1));
    EXPECT_NEAR (hit->point[1], 0.0, 1e-9);
    // The gap between the displaced storeys hits nothing, the hidden aggregate included.
    EXPECT_FALSE (dg::Pick (frame, plan, meshes, { 5, -50, 3.75 }, { 0, 1, 0 }));
}

TEST (DiagramLayout, PickIsExactForAConcaveCell)
{
    dg::Scene scene;
    scene.entities.push_back (Make (Storey ("L", 0), {}, { 0, 0, 0 }, { 10, 10, 3 }));
    const auto plan = dg::Plan (scene, Linear ());
    const auto frame = dg::Evaluate (plan, {}, 0.0);
    // L footprint: (0,0)-(10,5) plus (0,5)-(5,10); top face only, pivot (5,5,1.5).
    dg::PickMesh l;
    l.id = Storey ("L", 0);
    const double pts[][2] = { { 0, 0 }, { 10, 0 }, { 10, 5 }, { 5, 5 }, { 5, 10 }, { 0, 10 } };
    for (const auto& p : pts)
        l.positions.insert (l.positions.end (), { p[0] - 5, p[1] - 5, 1.5 });
    l.indices = { 0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5 };
    EXPECT_FALSE (dg::Pick (frame, plan, { l }, { 7.5, 7.5, 50 }, { 0, 0, -1 })); // the notch
    auto hit = dg::Pick (frame, plan, { l }, { 2.5, 7.5, 50 }, { 0, 0, -1 });
    ASSERT_TRUE (hit);
    EXPECT_NEAR (hit->point[2], 3.0, 1e-9);
}

TEST (DiagramLayout, DisplayMatrixIsRelativeToTheRenderOrigin)
{
    dg::NodePose pose;
    pose.pivot = { 6500000.25, 2400000.5, 10 };
    pose.offset = { 0, 0, 1.5 };
    const auto m = dg::DisplayMatrix (pose, { 6500000, 2400000, 0 });
    EXPECT_FLOAT_EQ (m[12], 0.25f);
    EXPECT_FLOAT_EQ (m[13], 0.5f);
    EXPECT_FLOAT_EQ (m[14], 11.5f);
    EXPECT_FLOAT_EQ (m[15], 1.0f);
}

namespace {
std::vector<dg::StoreySource> Stack (const std::vector<double>& heights)
{
    std::vector<dg::StoreySource> s;
    double z = 0;
    for (size_t i = 0; i < heights.size (); ++i) {
        s.push_back ({ "", int (i), z, int (i) });
        z += heights[i];
    }
    return s;
}
std::vector<dg::StoreyBinding> Bound (const dg::BindResult& r)
{
    return r.bindings;
}
} // namespace

TEST (DiagramStoreyBinding, StableAcrossIdenticalAndReorderedSources)
{
    uint64_t next = 1;
    const auto first = dg::BindStoreys ("building:A", {}, Stack ({ 3, 3, 3 }), next);
    ASSERT_EQ (first.bindings.size (), 3u);
    EXPECT_EQ (first.bindings[0].id.key, "storey:building:A:1");
    EXPECT_EQ (next, 4u);
    const auto same = dg::BindStoreys ("building:A", Bound (first), Stack ({ 3, 3, 3 }), next);
    auto reversed = Stack ({ 3, 3, 3 });
    std::reverse (reversed.begin (), reversed.end ());
    const auto flipped = dg::BindStoreys ("building:A", Bound (first), reversed, next);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ (same.bindings[i].id, first.bindings[i].id);
        EXPECT_EQ (flipped.bindings[2 - i].id, first.bindings[i].id);
    }
    EXPECT_EQ (next, 4u);
    EXPECT_TRUE (same.dropped.empty ());
}

TEST (DiagramStoreyBinding, HeightChangeKeepsIdentityByStory)
{
    uint64_t next = 1;
    const auto first = dg::BindStoreys ("b", {}, Stack ({ 3, 3, 3 }), next);
    const auto taller = dg::BindStoreys ("b", Bound (first), Stack ({ 4, 4, 4 }), next); // z 0, 4, 8
    for (size_t i = 0; i < 3; ++i)
        EXPECT_EQ (taller.bindings[i].id, first.bindings[i].id);
    EXPECT_DOUBLE_EQ (taller.bindings[2].source.z, 8.0);
}

TEST (DiagramStoreyBinding, InsertAboveAndDeleteTop)
{
    uint64_t next = 1;
    const auto first = dg::BindStoreys ("b", {}, Stack ({ 3, 3 }), next);
    const auto grown = dg::BindStoreys ("b", Bound (first), Stack ({ 3, 3, 3 }), next);
    EXPECT_EQ (grown.bindings[0].id, first.bindings[0].id);
    EXPECT_EQ (grown.bindings[1].id, first.bindings[1].id);
    EXPECT_EQ (grown.bindings[2].serial, 3u);
    const auto shrunk = dg::BindStoreys ("b", Bound (grown), Stack ({ 3 }), next);
    EXPECT_EQ (shrunk.bindings[0].id, first.bindings[0].id);
    ASSERT_EQ (shrunk.dropped.size (), 2u);
    std::vector<std::string> notes;
    EXPECT_TRUE (dg::KeepSelection (grown.bindings[2].id, shrunk, notes).Empty ());
    EXPECT_EQ (notes.size (), 1u);
    EXPECT_EQ (dg::KeepSelection (first.bindings[0].id, shrunk, notes), first.bindings[0].id);
}

TEST (DiagramStoreyBinding, PersistentIdWinsOverValues)
{
    uint64_t next = 1;
    std::vector<dg::StoreySource> a = { { "p-low", 0, 0, 0 }, { "p-high", 1, 3, 1 } };
    const auto first = dg::BindStoreys ("b", {}, a, next);
    // Values swapped, persistent IDs kept: identity follows the ID, not elevation.
    std::vector<dg::StoreySource> b = { { "p-high", 0, 0, 0 }, { "p-low", 1, 3, 1 } };
    const auto second = dg::BindStoreys ("b", Bound (first), b, next);
    EXPECT_EQ (second.bindings[0].id, first.bindings[1].id);
    EXPECT_EQ (second.bindings[1].id, first.bindings[0].id);
}

TEST (DiagramStoreyBinding, AmbiguousMatchRefusesTheRebind)
{
    uint64_t next = 1;
    const auto first = dg::BindStoreys ("b", {}, { { "", 0, 0, 0 } }, next);
    // Two current storeys agree with the single previous one at the same rank.
    const auto split = dg::BindStoreys ("b", Bound (first), { { "", 0, 0, 0 }, { "", 0, 0.01, 1 } }, next);
    EXPECT_NE (split.bindings[0].id, first.bindings[0].id);
    EXPECT_NE (split.bindings[1].id, first.bindings[0].id);
    EXPECT_FALSE (split.notes.empty ());
    ASSERT_EQ (split.dropped.size (), 1u);
    std::vector<std::string> notes;
    EXPECT_TRUE (dg::KeepSelection (first.bindings[0].id, split, notes).Empty ());
}

namespace {
// Archicad stories with floorIds, stacked from z = 0 at 3 m.
std::vector<dg::StoreySource> Floors (const std::vector<int>& floorIds)
{
    std::vector<dg::StoreySource> s;
    for (size_t i = 0; i < floorIds.size (); ++i)
        s.push_back ({ dg::StoreyPersistentId (floorIds[i]), int (i), 3.0 * double (i), int (i) });
    return s;
}
} // namespace

TEST (DiagramStoreyBinding, InsertMiddleKeepsMovedIdentity)
{
    uint64_t next = 1;
    const auto before = dg::BindStoreys ("b", {}, Floors ({ 10, 11, 12 }), next);
    // A storey (floorId 13) inserted at z = 3: the old 11 and 12 move up one index and 3 m.
    const auto after = dg::BindStoreys ("b", Bound (before), Floors ({ 10, 13, 11, 12 }), next);
    EXPECT_EQ (after.bindings[0].id, before.bindings[0].id);
    EXPECT_EQ (after.bindings[2].id, before.bindings[1].id);
    EXPECT_EQ (after.bindings[3].id, before.bindings[2].id);
    EXPECT_EQ (after.bindings[1].serial, 4u);
    EXPECT_TRUE (after.dropped.empty ());
    std::vector<std::string> notes;
    EXPECT_EQ (dg::KeepSelection (before.bindings[1].id, after, notes), before.bindings[1].id);
    EXPECT_TRUE (notes.empty ());
}

TEST (DiagramStoreyBinding, DeleteMiddleKeepsMovedIdentity)
{
    uint64_t next = 1;
    const auto before = dg::BindStoreys ("b", {}, Floors ({ 10, 11, 12 }), next);
    const auto after = dg::BindStoreys ("b", Bound (before), Floors ({ 10, 12 }), next);
    EXPECT_EQ (after.bindings[0].id, before.bindings[0].id);
    EXPECT_EQ (after.bindings[1].id, before.bindings[2].id); // took 11's index and elevation
    ASSERT_EQ (after.dropped.size (), 1u);
    EXPECT_EQ (after.dropped[0], before.bindings[1].id);
    std::vector<std::string> notes;
    EXPECT_TRUE (dg::KeepSelection (before.bindings[1].id, after, notes).Empty ());
}

TEST (DiagramStoreyBinding, DifferentPersistentIdsNeverMatchByPosition)
{
    uint64_t next = 1;
    const auto before = dg::BindStoreys ("b", {}, Floors ({ 10 }), next);
    // Same index and elevation, a different story: replaced, not renamed.
    const auto after = dg::BindStoreys ("b", Bound (before), Floors ({ 20 }), next);
    EXPECT_NE (after.bindings[0].id, before.bindings[0].id);
    EXPECT_EQ (after.dropped.size (), 1u);
    // A missing ID still falls back to position (older snapshots, fixtures).
    auto anonymous = Floors ({ 20 });
    anonymous[0].persistentId.clear ();
    const auto fallback = dg::BindStoreys ("b", Bound (after), anonymous, next);
    EXPECT_EQ (fallback.bindings[0].id, after.bindings[0].id);
}
