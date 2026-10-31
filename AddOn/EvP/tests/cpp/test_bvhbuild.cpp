// L2 offline tests for Geometry/BvhBuild: the binned-SAH tree QueryEngine
// installs in place of nanort's own Build (#97).
//
// Two kinds of check. STRUCTURE: every triangle is in exactly one leaf, boxes
// contain what they hold, the pre-order layout nanort's traversals assume, and
// the depth cap the GPU stack relies on. PARITY: on a scene with known-bad
// cases (a ground slab spanning everything, slivers, coincident duplicates),
// the new tree answers every query as nanort's tree does, because the
// traversal arithmetic is nanort's on both; only a tie between two triangles
// at one distance may be broken the other way.
#include "Geometry/BvhBuild.hpp"
#include "Geometry/QueryEngine.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using geomsrv::BuildBinnedSahBvh;
using geomsrv::BvhBuilder;
using geomsrv::BvhBuildResult;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

struct Lcg {
    uint64_t state = 12345;
    double Next ()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return double (state >> 11) * (1.0 / 9007199254740992.0);
    }
};

// A ground slab under a block city, plus long slivers across it and a stack of
// identical triangles: the inputs that make a binned build degenerate.
std::shared_ptr<Snapshot> AwkwardScene (size_t blocks)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 7;
    snap->meshes.push_back (evptest::MakeBox ("ground", -2.0, -2.0, -0.2, 44.0, 44.0, 0.2));
    Lcg random;
    for (size_t b = 0; b < blocks; ++b)
        snap->meshes.push_back (evptest::MakeBox ("block-" + std::to_string (b), 40.0 * random.Next (),
                                                  40.0 * random.Next (), 0.0, 0.5 + random.Next (),
                                                  0.5 + random.Next (), 1.0 + 9.0 * random.Next ()));
    Mesh slivers;
    slivers.guid = "slivers";
    for (int s = 0; s < 20; ++s) {
        const double y = 2.0 * s, z = 0.5 + 0.4 * s;
        const uint32_t base = static_cast<uint32_t> (slivers.vertices.size () / 3);
        slivers.vertices.insert (slivers.vertices.end (), { -1.0, y, z, 41.0, y + 0.001, z, 41.0, y, z + 0.002 });
        slivers.triangles.insert (slivers.triangles.end (), { base, base + 1, base + 2 });
    }
    snap->meshes.push_back (slivers);
    Mesh stack;
    stack.guid = "stack";
    for (int s = 0; s < 40; ++s) {
        const uint32_t base = static_cast<uint32_t> (stack.vertices.size () / 3);
        stack.vertices.insert (stack.vertices.end (), { 20.0, 20.0, 12.0, 21.0, 20.0, 12.0, 20.0, 21.0, 12.0 });
        stack.triangles.insert (stack.triangles.end (), { base, base + 1, base + 2 });
    }
    snap->meshes.push_back (stack);
    for (auto& mesh : snap->meshes)
        evptest::RecomputeBounds (mesh);
    return snap;
}

struct Soup {
    std::vector<double> vertices;
    std::vector<uint32_t> faces;
};

Soup Flatten (const Snapshot& snap)
{
    Soup soup;
    for (const auto& mesh : snap.meshes) {
        const uint32_t base = static_cast<uint32_t> (soup.vertices.size () / 3);
        soup.vertices.insert (soup.vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
        for (const uint32_t v : mesh.triangles)
            soup.faces.push_back (base + v);
    }
    return soup;
}

bool Contains (const geomsrv::BvhBuildNode& outer, const double lo[3], const double hi[3])
{
    for (int k = 0; k < 3; ++k)
        if (lo[k] < outer.bmin[k] || hi[k] > outer.bmax[k])
            return false;
    return true;
}

} // namespace

TEST (BvhBuild, EveryTriangleIsInExactlyOneLeafAndEveryBoxHoldsItsContents)
{
    const auto snap = AwkwardScene (800);
    const Soup soup = Flatten (*snap);
    const size_t triangles = soup.faces.size () / 3;
    const BvhBuildResult tree = BuildBinnedSahBvh (soup.vertices.data (), soup.faces.data (), triangles);
    ASSERT_FALSE (tree.nodes.empty ());
    ASSERT_EQ (tree.indices.size (), triangles);
    std::vector<int> seen (triangles, 0);
    std::vector<size_t> depth (tree.nodes.size (), 0);
    depth[0] = 1;
    size_t deepest = 0;
    for (size_t n = 0; n < tree.nodes.size (); ++n) {
        const auto& node = tree.nodes[n];
        deepest = (std::max) (deepest, depth[n]);
        if (node.flag == 0) {
            // Pre-order: the left child follows its parent; the right one after it.
            EXPECT_EQ (node.data[0], n + 1);
            EXPECT_GT (node.data[1], node.data[0]);
            ASSERT_LT (node.data[1], tree.nodes.size ());
            for (const uint32_t child : node.data) {
                EXPECT_TRUE (Contains (node, tree.nodes[child].bmin, tree.nodes[child].bmax));
                depth[child] = depth[n] + 1;
            }
            continue;
        }
        EXPECT_EQ (node.flag, 1);
        ASSERT_LE (size_t (node.data[1]) + node.data[0], tree.indices.size ());
        for (uint32_t i = 0; i < node.data[0]; ++i) {
            const uint32_t t = tree.indices[node.data[1] + i];
            ++seen[t];
            for (int c = 0; c < 3; ++c) {
                const double* v = &soup.vertices[3 * size_t (soup.faces[3 * t + c])];
                EXPECT_TRUE (Contains (node, v, v)) << "triangle " << t << " outside its leaf";
            }
        }
    }
    for (size_t t = 0; t < triangles; ++t)
        ASSERT_EQ (seen[t], 1) << "triangle " << t;
    EXPECT_EQ (tree.depth, deepest);
    EXPECT_LE (tree.depth, geomsrv::kBvhMaxDepth);
}

// nanort's Build evaluates SAH on X only and, where X cannot separate, peels
// 1/64 of the Y or Z extent; on a regular grid that is a tree many times
// deeper than log2(n). The new tree stays near log2(n) on both layouts.
TEST (BvhBuild, TheTreeStaysShallowOnARegularGrid)
{
    auto grid = std::make_shared<Snapshot> ();
    grid->id = 9;
    for (int b = 0; b < 4096; ++b)
        grid->meshes.push_back (evptest::MakeBox ("g" + std::to_string (b), b % 64, b / 64, 0.0, 0.6, 0.6, 3.0));
    const QueryEngine legacy (grid, BvhBuilder::NanortLegacy);
    const QueryEngine sah (grid, BvhBuilder::BinnedSah);
    EXPECT_EQ (sah.TriangleCount (), legacy.TriangleCount ());
    EXPECT_LT (sah.BvhDepth (), 28u) << "49k triangles";
    EXPECT_GT (legacy.BvhDepth (), 2 * sah.BvhDepth ()) << "the legacy defect on a grid";
    const QueryEngine random (AwkwardScene (4000));
    EXPECT_LT (random.BvhDepth (), 28u);
}

TEST (BvhBuild, CoincidentTrianglesStillSplitIntoBoundedLeaves)
{
    std::vector<double> vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    std::vector<uint32_t> faces;
    for (int t = 0; t < 1000; ++t)
        faces.insert (faces.end (), { 0, 1, 2 });
    const BvhBuildResult tree = BuildBinnedSahBvh (vertices.data (), faces.data (), 1000);
    size_t total = 0;
    for (const auto& node : tree.nodes)
        if (node.flag == 1) {
            EXPECT_LE (node.data[0], 16u);
            total += node.data[0];
        }
    EXPECT_EQ (total, 1000u);
}

TEST (BvhBuild, EmptyInputBuildsNothing)
{
    const BvhBuildResult tree = BuildBinnedSahBvh (nullptr, nullptr, 0);
    EXPECT_TRUE (tree.nodes.empty ());
    EXPECT_TRUE (tree.indices.empty ());
    EXPECT_EQ (tree.depth, 0u);
}

// ⚠️ THE INVARIANT THAT MATTERS: the index answers exactly as before. Same
// triangles, same nanort arithmetic, different tree. Occluded, closest-hit
// distance and every pierce must agree ray for ray, including grazing rays
// along the ground and rays through the duplicate stack.
TEST (BvhBuild, QueriesAgreeWithNanortsTreeRayForRay)
{
    const auto snap = AwkwardScene (600);
    const QueryEngine legacy (snap, BvhBuilder::NanortLegacy);
    const QueryEngine sah (snap, BvhBuilder::BinnedSah);
    Lcg random;
    size_t occluded = 0;
    for (int r = 0; r < 4000; ++r) {
        const double origin[3] = { -5.0 + 50.0 * random.Next (), -5.0 + 50.0 * random.Next (),
                                   r % 4 == 0 ? 0.0005 : 15.0 * random.Next () };
        const double elevation = r % 3 == 0 ? 0.02 * random.Next () : 1.5 * random.Next ();
        const double azimuth = 6.283185307179586 * random.Next ();
        double dir[3] = { std::cos (azimuth) * std::cos (elevation), std::sin (azimuth) * std::cos (elevation),
                          std::sin (elevation) };
        if (r % 7 == 0)
            dir[2] = -dir[2];
        const bool a = legacy.Occluded (origin, dir, 0.001, 0.0);
        ASSERT_EQ (sah.Occluded (origin, dir, 0.001, 0.0), a) << "ray " << r;
        occluded += a ? 1 : 0;
        const auto hitA = legacy.Raycast (origin, dir, 0.0);
        const auto hitB = sah.Raycast (origin, dir, 0.0);
        ASSERT_EQ (hitB.hit, hitA.hit) << "ray " << r;
        // Two triangles can meet a ray at one distance to the last bit or
        // two (a shared edge, the duplicate stack); either is the answer.
        if (hitA.hit)
            ASSERT_NEAR (hitB.t, hitA.t, 1e-9) << "ray " << r;
        const auto allA = legacy.RaycastAll (origin, dir, 0.0, 0);
        const auto allB = sah.RaycastAll (origin, dir, 0.0, 0);
        ASSERT_EQ (allB.hits.size (), allA.hits.size ()) << "ray " << r;
        for (size_t h = 0; h < allA.hits.size (); ++h)
            ASSERT_NEAR (allB.hits[h].t, allA.hits[h].t, 1e-9) << "ray " << r << " hit " << h;
    }
    // Both outcomes are exercised, not one trivially.
    EXPECT_GT (occluded, 400u);
    EXPECT_LT (occluded, 3600u);
}

// ⚠️ THE PARALLEL BUILD IS AN OPTIMISATION, NOT A DIFFERENT TREE. Built on one
// thread and on eight, every node, link, box and leaf range is the same, so
// what the parity test above proves for one holds for the other (#97).
TEST (BvhBuild, ParallelAndSequentialTreesAreIdentical)
{
    const auto snap = AwkwardScene (6000);
    const Soup soup = Flatten (*snap);
    const size_t triangles = soup.faces.size () / 3;
    ASSERT_GT (triangles, 4u * 16384u) << "large enough to fan out";
    const BvhBuildResult one = BuildBinnedSahBvh (soup.vertices.data (), soup.faces.data (), triangles, 1);
    for (const size_t threads : { size_t (2), size_t (3), size_t (8) }) {
        const BvhBuildResult many = BuildBinnedSahBvh (soup.vertices.data (), soup.faces.data (), triangles, threads);
        ASSERT_EQ (many.nodes.size (), one.nodes.size ()) << threads << " threads";
        EXPECT_EQ (many.indices, one.indices) << threads << " threads";
        EXPECT_EQ (many.depth, one.depth) << threads << " threads";
        for (size_t n = 0; n < one.nodes.size (); ++n) {
            const auto& a = one.nodes[n];
            const auto& b = many.nodes[n];
            ASSERT_TRUE (a.flag == b.flag && a.axis == b.axis && a.data[0] == b.data[0] && a.data[1] == b.data[1])
                << "node " << n << ", " << threads << " threads";
            for (int k = 0; k < 3; ++k)
                ASSERT_TRUE (a.bmin[k] == b.bmin[k] && a.bmax[k] == b.bmax[k]) << "node " << n;
        }
    }
}
