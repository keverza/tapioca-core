// L2 offline tests for SunStudy/CpuTraversal — the baseline occlusion backend.
//
// The properties that matter are not "does it trace a ray" (QueryEngine's own
// tests cover that) but the ones the sharding could break: that a threaded run
// and an inline run agree exactly, that a shard boundary never drops or
// duplicates a query, and that a degenerate caller comes back CLEAR rather than
// shadowed.

#include "SunStudy/CpuTraversal.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace evp::sunstudy;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

// A row of 1 x 1 blocks at z in [5, 6], one every 2 m along x from 0 to 2*(n-1).
// A sample under an even x is shadowed from straight above; an odd x is clear.
// That makes the expected answer a known alternating pattern of any length,
// which is what a shard-boundary bug shows up in.
std::shared_ptr<const Snapshot> MakeCombSnapshot (int blocks)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 7;

    Mesh mesh;
    mesh.guid = "comb";
    mesh.elemType = 0;
    for (int i = 0; i < blocks; ++i) {
        const double x = static_cast<double> (i) * 2.0;
        const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
        // One quad at z = 5 spanning [x - 0.4, x + 0.4] in both x and y.
        mesh.vertices.insert (mesh.vertices.end (), {
                                                        x - 0.4,
                                                        -0.4,
                                                        5.0,
                                                        x + 0.4,
                                                        -0.4,
                                                        5.0,
                                                        x + 0.4,
                                                        0.4,
                                                        5.0,
                                                        x - 0.4,
                                                        0.4,
                                                        5.0,
                                                    });
        mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    snap->meshes.push_back (std::move (mesh));
    return snap;
}

// Origins on the ground under each block position and each gap between them.
std::vector<double> CombOrigins (int positions)
{
    std::vector<double> origins;
    origins.reserve (static_cast<size_t> (positions) * 3);
    for (int i = 0; i < positions; ++i) {
        origins.push_back (static_cast<double> (i)); // x: even = under a block
        origins.push_back (0.0);
        origins.push_back (0.0);
    }
    return origins;
}

const double kUp[3] = { 0.0, 0.0, 1.0 };

} // namespace

TEST (CpuTraversal, DirectionalOcclusionMatchesTheKnownPattern)
{
    const int positions = 21;
    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (11)));
    const std::vector<double> origins = CombOrigins (positions);

    std::vector<uint8_t> out (positions, 0xFF);
    traversal.OccludeDirectional (origins.data (), positions, kUp, 0.001, 0.0, out.data (), 1);

    for (int i = 0; i < positions; ++i) {
        const bool underABlock = (i % 2) == 0;
        EXPECT_EQ (out[i], underABlock ? 1 : 0) << "at x = " << i;
    }
}

// ⚠️ THE INVARIANT THE SHARDING COULD BREAK. A threaded run and an inline run
// must be byte-identical: a study whose answer depends on the machine's core
// count is not an analysis.
TEST (CpuTraversal, ThreadedAndInlineRunsAgreeExactly)
{
    // Above the inline threshold, so the threaded arm really does spawn.
    const int positions = 20001;
    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (11)));
    const std::vector<double> origins = CombOrigins (positions);

    std::vector<uint8_t> inlineOut (positions, 0xFF);
    std::vector<uint8_t> threadedOut (positions, 0xFE);
    traversal.OccludeDirectional (origins.data (), positions, kUp, 0.001, 0.0, inlineOut.data (), 1);
    traversal.OccludeDirectional (origins.data (), positions, kUp, 0.001, 0.0, threadedOut.data (), 8);

    EXPECT_EQ (inlineOut, threadedOut);

    // And every slot was written -- neither 0xFF nor 0xFE survives anywhere.
    for (int i = 0; i < positions; ++i) {
        EXPECT_LE (inlineOut[i], 1) << "inline slot " << i << " never written";
        EXPECT_LE (threadedOut[i], 1) << "threaded slot " << i << " never written";
    }
}

// A count that divides badly across workers is where an off-by-one in the shard
// arithmetic lives: the last shard is short, or one query is done twice.
TEST (CpuTraversal, AwkwardCountsAreFullyAndSinglyCovered)
{
    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (11)));

    for (const size_t count : { size_t { 1 }, size_t { 2 }, size_t { 3 }, size_t { 7 }, size_t { 4097 } }) {
        const std::vector<double> origins = CombOrigins (static_cast<int> (count));
        std::vector<uint8_t> reference (count, 0xFF);
        traversal.OccludeDirectional (origins.data (), count, kUp, 0.001, 0.0, reference.data (), 1);

        for (const size_t threads : { size_t { 2 }, size_t { 3 }, size_t { 8 }, size_t { 64 } }) {
            std::vector<uint8_t> sharded (count, 0xFF);
            traversal.OccludeDirectional (origins.data (), count, kUp, 0.001, 0.0, sharded.data (), threads);
            EXPECT_EQ (reference, sharded) << "count " << count << " threads " << threads;
        }
    }
}

TEST (CpuTraversal, RayBatchAgreesWithTheDirectionalForm)
{
    const int positions = 21;
    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (11)));
    const std::vector<double> origins = CombOrigins (positions);

    std::vector<uint8_t> directional (positions, 0xFF);
    traversal.OccludeDirectional (origins.data (), positions, kUp, 0.001, 0.0, directional.data (), 1);

    std::vector<OcclusionRay> rays (positions);
    for (int i = 0; i < positions; ++i) {
        rays[i].origin[0] = origins[static_cast<size_t> (i) * 3 + 0];
        rays[i].origin[1] = origins[static_cast<size_t> (i) * 3 + 1];
        rays[i].origin[2] = origins[static_cast<size_t> (i) * 3 + 2];
        rays[i].dir[0] = kUp[0];
        rays[i].dir[1] = kUp[1];
        rays[i].dir[2] = kUp[2];
        rays[i].tmin = 0.001;
        rays[i].tmax = 0.0;
    }
    std::vector<uint8_t> batched (positions, 0xFF);
    traversal.OccludeRays (rays.data (), positions, batched.data (), 1);

    EXPECT_EQ (directional, batched);
}

// ⚠️ A DEGENERATE CALLER MUST COME BACK CLEAR, NOT SHADOWED, and the buffer must
// still be fully written -- an untouched output buffer carries whatever the
// caller last put in it, which is the worst of the three outcomes.
TEST (CpuTraversal, DegenerateInputWritesClearRatherThanLeavingTheBuffer)
{
    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (4)));
    std::vector<uint8_t> out (5, 0xFF);

    traversal.OccludeDirectional (nullptr, out.size (), kUp, 0.001, 0.0, out.data (), 1);
    for (const uint8_t value : out)
        EXPECT_EQ (value, 0) << "a null origin array must read as clear, not as stale memory";

    std::fill (out.begin (), out.end (), uint8_t { 0xFF });
    traversal.OccludeRays (nullptr, out.size (), out.data (), 1);
    for (const uint8_t value : out)
        EXPECT_EQ (value, 0);
}

TEST (CpuTraversal, NullEngineIsClearAndVersionZero)
{
    CpuTraversal traversal (nullptr);
    EXPECT_EQ (traversal.SceneVersion (), 0u);

    std::vector<uint8_t> out (3, 0xFF);
    const std::vector<double> origins = CombOrigins (3);
    traversal.OccludeDirectional (origins.data (), 3, kUp, 0.001, 0.0, out.data (), 1);
    for (const uint8_t value : out)
        EXPECT_EQ (value, 0);
}

TEST (CpuTraversal, ZeroCountTouchesNothing)
{
    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (4)));
    uint8_t sentinel = 0xAB;
    traversal.OccludeDirectional (nullptr, 0, kUp, 0.001, 0.0, &sentinel, 1);
    EXPECT_EQ (sentinel, 0xAB);
}

// The version is the snapshot's identity, so a new snapshot is a new version and
// the same snapshot is the same version. A separate counter could drift from the
// geometry; this cannot.
TEST (CpuTraversal, SceneVersionTracksTheSnapshot)
{
    auto first = MakeCombSnapshot (4);
    CpuTraversal a (std::make_shared<QueryEngine> (first));
    EXPECT_EQ (a.SceneVersion (), 7u);

    auto second = std::make_shared<Snapshot> (*first);
    second->id = 9;
    CpuTraversal b (std::make_shared<QueryEngine> (second));
    EXPECT_EQ (b.SceneVersion (), 9u);
    EXPECT_NE (a.SceneVersion (), b.SceneVersion ());
}

TEST (CpuTraversal, ThreadCountNeverExceedsTheWorkAndIsNeverZero)
{
    EXPECT_EQ (ChooseThreadCount (0, 0), 1u);
    EXPECT_EQ (ChooseThreadCount (100, 1), 1u) << "maxParallel 1 must run inline";
    EXPECT_EQ (ChooseThreadCount (100, 0), 1u) << "a small batch must not spawn";
    EXPECT_EQ (ChooseThreadCount (1000000, 4), 4u);
    EXPECT_GE (ChooseThreadCount (1000000, 0), 1u);
}

// ---------------------------------------------------------------------------
// throughput
// ---------------------------------------------------------------------------
//
// DISABLED because it is a MEASUREMENT, not an assertion: the number depends on
// the machine, and a threshold that passes on a workstation fails in CI for
// reasons that have nothing to do with the code. Run it deliberately:
//
//   EvPGeomTests.exe --gtest_also_run_disabled_tests \
//                    --gtest_filter=CpuTraversal.DISABLED_Throughput
//
// It exists because the decision to build a GPU backend is supposed to be made
// by a measured run rather than an assumption, and this is that run.
//
// ⚠️ THE OCCLUDER FIELD HERE IS SYNTHETIC AND FAR SIMPLER THAN A REAL MODEL --
// a few hundred triangles against a real project's hundreds of thousands. BVH
// traversal grows with the log of the triangle count, so this number is an UPPER
// BOUND on throughput and must not be quoted as the study's speed. What it
// legitimately settles is the order of magnitude and the parallel speedup; the
// real figure comes from a live run against a real project.
TEST (CpuTraversal, DISABLED_Throughput)
{
    // A city-block-ish occluder field and a day of sun directions.
    constexpr int kBlocks = 400;    // 400 quads -> 800 triangles of occluder
    constexpr int kSamples = 84052; // the sample count the previous study reported
    constexpr int kTimesteps = 46;  // a 46-step equinox day

    CpuTraversal traversal (std::make_shared<QueryEngine> (MakeCombSnapshot (kBlocks)));

    std::vector<double> origins (static_cast<size_t> (kSamples) * 3);
    for (int i = 0; i < kSamples; ++i) {
        origins[static_cast<size_t> (i) * 3 + 0] = static_cast<double> (i % 800) * 1.0;
        origins[static_cast<size_t> (i) * 3 + 1] = static_cast<double> (i / 800) * 1.0;
        origins[static_cast<size_t> (i) * 3 + 2] = 0.0;
    }

    std::vector<uint8_t> out (kSamples, 0);

    const auto run = [&] (size_t threads) {
        const auto start = std::chrono::steady_clock::now ();
        for (int step = 0; step < kTimesteps; ++step) {
            const double angle = 3.14159265358979 * (0.1 + 0.8 * step / double (kTimesteps));
            const double dir[3] = { std::cos (angle), 0.2, std::sin (angle) };
            traversal.OccludeDirectional (origins.data (), kSamples, dir, 0.001, 0.0, out.data (), threads);
        }
        return std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
    };

    const double serial = run (1);
    const double parallel = run (0);
    const double rays = double (kSamples) * kTimesteps;

    std::printf ("\n  rays          %.0f (%d samples x %d timesteps)\n", rays, kSamples, kTimesteps);
    std::printf ("  serial        %.3f s  (%.2f M rays/s)\n", serial, rays / serial / 1e6);
    std::printf ("  parallel      %.3f s  (%.2f M rays/s)\n", parallel, rays / parallel / 1e6);
    std::printf ("  speedup       %.2fx\n", serial / parallel);
    std::printf ("  prior baseline 33 s -> %.1fx faster\n\n", 33.0 / parallel);

    SUCCEED ();
}
