// ArchViz/MeshGroups — per-triangle materials -> contiguous draw ranges.
//
// ⚠️ WHY THIS TEST EXISTS AT ALL: getting the grouping wrong does not fail, it
// renders every surface of an element in the FIRST material's colour, which
// looks like a styling choice rather than a bug (plan §6.3). It has already been
// got wrong once on the three.js side. There is no way to see it from Archicad
// without knowing what the model is supposed to look like, so it is checked
// here, offline, where a wrong permutation is an assertion and not an opinion.
//
// The first three cases MIRROR Commands/ModelViewer/test_scenegen.py so the two
// renderers cannot drift apart about what a group is; the rest are C++-specific
// (the truncated-array contract, and the final-run bug that drops the last
// material).

#include "ArchViz/MeshGroups.hpp"

#include <gtest/gtest.h>

#include <set>

using geomsrv::archviz::BuildMaterialGroups;
using geomsrv::archviz::MaterialRange;

namespace {

// 3 sequential indices per triangle, so a triangle's identity is readable from
// its first index: triangle i is {3i, 3i+1, 3i+2}.
std::vector<uint32_t> Triangles (size_t count)
{
    std::vector<uint32_t> t (count * 3);
    for (size_t i = 0; i < count * 3; ++i)
        t[i] = uint32_t (i);
    return t;
}

} // namespace

// Mirrors test_material_groups_are_contiguous_and_cover_everything.
TEST (MeshGroups, ContiguousAndCoversEverything)
{
    const std::vector<int32_t> mats = { 3, 1, 3, 2, 1, 1 };
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (mats.size ()), mats, idx, ranges);

    ASSERT_EQ (ranges.size (), 3u);
    EXPECT_EQ (ranges[0].material, 1);
    EXPECT_EQ (ranges[1].material, 2);
    EXPECT_EQ (ranges[2].material, 3);

    // Index-buffer units, 3 per triangle — material 1 has three triangles.
    EXPECT_EQ (ranges[0].indexCount, 9u);
    EXPECT_EQ (ranges[1].indexCount, 3u);
    EXPECT_EQ (ranges[2].indexCount, 6u);
    EXPECT_EQ (ranges[0].firstIndex, 0u);
    EXPECT_EQ (ranges[1].firstIndex, 9u);
    EXPECT_EQ (ranges[2].firstIndex, 12u);

    uint32_t total = 0;
    for (const MaterialRange& r : ranges)
        total += r.indexCount;
    EXPECT_EQ (total, uint32_t (mats.size () * 3));
    EXPECT_EQ (idx.size (), mats.size () * 3);

    // Every triangle appears exactly once — a permutation, not a filter.
    std::set<uint32_t> firsts;
    for (size_t i = 0; i < idx.size (); i += 3)
        firsts.insert (idx[i] / 3);
    EXPECT_EQ (firsts.size (), mats.size ());
}

// Mirrors test_material_groups_sorted_order_matches_group_materials: every
// triangle inside a range really does carry that range's material.
TEST (MeshGroups, EveryTriangleInARangeHasThatMaterial)
{
    const std::vector<int32_t> mats = { 5, 0, 5, 0, 7 };
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (mats.size ()), mats, idx, ranges);

    for (const MaterialRange& r : ranges) {
        for (uint32_t i = r.firstIndex; i < r.firstIndex + r.indexCount; i += 3) {
            const uint32_t sourceTri = idx[i] / 3;
            EXPECT_EQ (mats[sourceTri], r.material)
                << "triangle " << sourceTri << " landed in the range for material " << r.material;
        }
    }
}

// Mirrors test_material_groups_empty.
TEST (MeshGroups, Empty)
{
    std::vector<uint32_t> idx = { 1, 2, 3 }; // pre-dirtied, must be cleared
    std::vector<MaterialRange> ranges = { MaterialRange { 9, 9, 9 } };
    BuildMaterialGroups ({}, {}, idx, ranges);
    EXPECT_TRUE (idx.empty ());
    EXPECT_TRUE (ranges.empty ());
}

// ⚠️ THE LAST RUN IS NEVER CLOSED BY THE LOOP — it has no successor to differ
// from — so forgetting to emit it after the loop silently drops the final
// material. One surface goes missing, which reads as bad geometry.
TEST (MeshGroups, LastRunIsEmitted)
{
    const std::vector<int32_t> mats = { 0, 0, 1 };
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (mats.size ()), mats, idx, ranges);

    ASSERT_EQ (ranges.size (), 2u);
    EXPECT_EQ (ranges[1].material, 1);
    EXPECT_EQ (ranges[1].firstIndex, 6u);
    EXPECT_EQ (ranges[1].indexCount, 3u);
}

// A single material must still produce ONE range covering everything — the
// common case for a simple element, and the one where an off-by-one in the run
// bookkeeping would be invisible in the multi-material tests above.
TEST (MeshGroups, SingleMaterialIsOneRange)
{
    const std::vector<int32_t> mats = { 4, 4, 4, 4 };
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (mats.size ()), mats, idx, ranges);

    ASSERT_EQ (ranges.size (), 1u);
    EXPECT_EQ (ranges[0].material, 4);
    EXPECT_EQ (ranges[0].firstIndex, 0u);
    EXPECT_EQ (ranges[0].indexCount, 12u);
    // Stable: with one material nothing may be reordered at all.
    for (uint32_t i = 0; i < idx.size (); ++i)
        EXPECT_EQ (idx[i], i);
}

// Equal materials keep their input order. This is what makes the output
// diffable between runs and keeps a checked-in fixture valid.
TEST (MeshGroups, StableWithinAGroup)
{
    const std::vector<int32_t> mats = { 1, 0, 1, 0, 1 };
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (mats.size ()), mats, idx, ranges);

    ASSERT_EQ (ranges.size (), 2u);
    // material 0: source triangles 1 then 3, in that order.
    EXPECT_EQ (idx[0] / 3, 1u);
    EXPECT_EQ (idx[3] / 3, 3u);
    // material 1: source triangles 0, 2, 4.
    EXPECT_EQ (idx[6] / 3, 0u);
    EXPECT_EQ (idx[9] / 3, 2u);
    EXPECT_EQ (idx[12] / 3, 4u);
}

// The documented contract for a truncated material array: read as material 0,
// never out of bounds. A bad extraction should draw in one colour, not crash —
// and under ASan an over-read here would be a hard failure, which is the point.
TEST (MeshGroups, TruncatedMaterialArrayIsTreatedAsZero)
{
    const std::vector<int32_t> mats = { 2 }; // 1 entry for 3 triangles
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (3), mats, idx, ranges);

    ASSERT_EQ (ranges.size (), 2u);
    EXPECT_EQ (ranges[0].material, 0);
    EXPECT_EQ (ranges[0].indexCount, 6u); // triangles 1 and 2
    EXPECT_EQ (ranges[1].material, 2);
    EXPECT_EQ (ranges[1].indexCount, 3u); // triangle 0
    EXPECT_EQ (idx.size (), 9u);
}

// An empty material array at all: everything is material 0, one range, no
// reordering. The degenerate input a snapshot of an untextured element gives.
TEST (MeshGroups, NoMaterialsAtAll)
{
    std::vector<uint32_t> idx;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (2), {}, idx, ranges);

    ASSERT_EQ (ranges.size (), 1u);
    EXPECT_EQ (ranges[0].material, 0);
    EXPECT_EQ (ranges[0].firstIndex, 0u);
    EXPECT_EQ (ranges[0].indexCount, 6u);
}

TEST (MeshGroups, WireEdgesFollowMaterialPermutation)
{
    const std::vector<int32_t> mats = { 2, 1, 2 };
    const std::vector<uint8_t> wire = { 1, 2, 4 };
    std::vector<uint32_t> idx;
    std::vector<uint32_t> reorderedWire;
    std::vector<MaterialRange> ranges;
    BuildMaterialGroups (Triangles (3), mats, idx, ranges, &wire, &reorderedWire);

    EXPECT_EQ (reorderedWire, (std::vector<uint32_t> { 2, 1, 4 }));
    EXPECT_EQ (idx[0] / 3, 1u);
    EXPECT_EQ (idx[3] / 3, 0u);
    EXPECT_EQ (idx[6] / 3, 2u);
}
