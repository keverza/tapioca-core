// Planting receptor layers (#278 Stage 1A): the C++ wrapper over the Rust
// `receptor` kernel and the record's overhead test.

#include "SunStudy/SunStudyReceptor.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyLiftedPoint.hpp"
#include "SunStudy/SunStudyCrownClearance.hpp"

#include "MeshFixtures.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <memory>

using namespace evp::sunstudy;

namespace {

constexpr double kOffset = 0.01;

// A 10 x 10 m bed at z 0, gridded at 1 m: 100 receptors of 1 m2 plan each.
SampleGrid Bed ()
{
    const std::vector<double> vertices { 0, 0, 0, 10, 0, 0, 10, 10, 0, 0, 10, 0 };
    const std::vector<uint32_t> triangles { 0, 1, 2, 0, 2, 3 };
    SamplerOptions options;
    options.spacing = 1.0;
    options.normalOffset = kOffset;
    options.wantLayouts = true;
    return kernels::BuildSampleGrid (vertices.data (), 4, triangles.data (), 2, nullptr, options);
}

// The bed as a slab, and a balcony 4 m deep across its west edge whose
// underside is at z 2.5.
void BalconyScene (std::shared_ptr<geomsrv::Snapshot>& snapshot, StudyRecord& record)
{
    snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 278;
    snapshot->meshes = { evptest::MakeBox ("bed", 0, 0, -0.3, 10, 10, 0.3),
                         evptest::MakeBox ("balcony", 0, 0, 2.5, 4, 10, 0.2) };
    record.snapshot = snapshot;
    record.snapshotId = snapshot->id;
    record.occluders = BuildSunStudyOccluders (*snapshot, kernels::ResolveElementRoles (*snapshot, {}, {}, {}));
}

} // namespace

TEST (SunStudyReceptor, LayerLiftsVerticallyKeepsOrderAndWeighsPlanArea)
{
    const SampleGrid grid = Bed ();
    ASSERT_TRUE (grid.valid);
    ReceptorOptions options;
    options.height = 1.5;
    ReceptorLayer layer;
    std::string error;
    ASSERT_TRUE (BuildReceptorLayer (grid, kOffset, options, nullptr, layer, error)) << error;
    ASSERT_EQ (layer.Count (), grid.Count ());
    for (size_t i = 0; i < layer.Count (); ++i) {
        EXPECT_DOUBLE_EQ (layer.positions[i * 3 + 0], grid.positions[i * 3 + 0]);
        EXPECT_DOUBLE_EQ (layer.positions[i * 3 + 1], grid.positions[i * 3 + 1]);
        EXPECT_NEAR (layer.positions[i * 3 + 2], 1.5, 1.0e-12);
        EXPECT_NEAR (layer.hosts[i * 3 + 2], 0.0, 1.0e-12);
        EXPECT_EQ (layer.normals[i * 3 + 2], 1.0);
    }
    EXPECT_EQ (layer.available, layer.Count ());
    EXPECT_NEAR (layer.availableArea, 100.0, 1.0e-9);
    EXPECT_EQ (layer.unavailableArea, 0.0);
}

TEST (SunStudyReceptor, BalconyBlocksTheTreeLayerButNotTheShrubLayer)
{
    std::shared_ptr<geomsrv::Snapshot> snapshot;
    StudyRecord record;
    BalconyScene (snapshot, record);
    const OverheadDistance overhead = [&record] (const double host[3]) {
        return SunStudyOverheadDistance (record, host);
    };
    const SampleGrid grid = Bed ();
    std::string error;

    ReceptorOptions shrub;
    shrub.height = 1.5;
    ReceptorLayer low;
    ASSERT_TRUE (BuildReceptorLayer (grid, kOffset, shrub, &overhead, low, error)) << error;
    EXPECT_EQ (low.blocked, 0u);
    EXPECT_NEAR (low.availableArea, 100.0, 1.0e-9);

    ReceptorOptions tree;
    tree.height = 3.0;
    ReceptorLayer high;
    ASSERT_TRUE (BuildReceptorLayer (grid, kOffset, tree, &overhead, high, error)) << error;
    // The 4 m strip under the balcony (x < 4) is unavailable, not dark. The
    // triangle lattice is per triangle, so the expected totals come from the
    // hosts, not from a 1 m square grid.
    size_t under = 0;
    double underArea = 0.0;
    for (size_t i = 0; i < high.Count (); ++i)
        if (high.hosts[i * 3] < 4.0) {
            ++under;
            underArea += high.areas[i];
        }
    EXPECT_GT (under, 0u);
    EXPECT_LT (under, high.Count ());
    EXPECT_EQ (high.blocked, under);
    EXPECT_NEAR (high.unavailableArea, underArea, 1.0e-9);
    EXPECT_NEAR (high.availableArea + high.unavailableArea, 100.0, 1.0e-9);
    for (size_t i = 0; i < high.Count (); ++i) {
        const bool isUnder = high.hosts[i * 3] < 4.0;
        EXPECT_EQ (high.status[i], isUnder ? ReceptorStatus::Blocked : ReceptorStatus::Available) << i;
        // Fixed layer: never clamped below the requested height.
        EXPECT_NEAR (high.positions[i * 3 + 2], 3.0, 1.0e-12);
    }
    // Same host order across heights: cell identity joins the two layers.
    EXPECT_EQ (low.hosts, high.hosts);
}

TEST (SunStudyReceptor, OverheadDistanceIsTheLiftedPointTest)
{
    std::shared_ptr<geomsrv::Snapshot> snapshot;
    StudyRecord record;
    BalconyScene (snapshot, record);
    const double under[3] = { 1.0, 5.0, 0.0 }, open[3] = { 8.0, 5.0, 0.0 };
    EXPECT_NEAR (SunStudyOverheadDistance (record, under), 2.5 - 0.001, 1.0e-9);
    EXPECT_LT (SunStudyOverheadDistance (record, open), 0.0);
    LiftedPoint lifted;
    ASSERT_TRUE (LiftSunStudyPoint (record, under, 3.0, lifted));
    EXPECT_TRUE (lifted.clamped); // the placed point clamps where the layer blocks
    EXPECT_NEAR (lifted.height, 2.5 - kLiftedPointClearance, 1.0e-6);
}

TEST (SunStudyReceptor, RefusedRequestLeavesNoLayer)
{
    const SampleGrid grid = Bed ();
    ReceptorOptions options;
    options.height = -1.0;
    ReceptorLayer layer;
    layer.available = 7;
    std::string error;
    EXPECT_FALSE (BuildReceptorLayer (grid, kOffset, options, nullptr, layer, error));
    EXPECT_NE (error.find ("ReceptorLayer"), std::string::npos) << error;
    EXPECT_EQ (layer.Count (), 0u);
    EXPECT_EQ (layer.available, 0u);
}

TEST (SunStudyReceptor, KeepAvailableReceptorsFiltersIdentityInStep)
{
    std::shared_ptr<geomsrv::Snapshot> snapshot;
    StudyRecord record;
    BalconyScene (snapshot, record);
    const OverheadDistance overhead = [&record] (const double host[3]) {
        return SunStudyOverheadDistance (*record.occluders, host);
    };
    SampleGrid grid = Bed ();
    const SampleGrid before = grid;
    ReceptorOptions tree;
    tree.height = 3.0;
    ReceptorLayer layer;
    std::string error;
    ASSERT_TRUE (BuildReceptorLayer (grid, kOffset, tree, &overhead, layer, error)) << error;
    ASSERT_TRUE (KeepAvailableReceptors (grid, layer));
    ASSERT_EQ (grid.Count (), layer.available);
    ASSERT_EQ (grid.positions.size (), grid.Count () * 3);
    ASSERT_EQ (grid.cellColumns.size (), grid.Count ());
    EXPECT_EQ (grid.layouts.size (), before.layouts.size ()) << "layouts still map the host";
    double area = 0.0;
    size_t kept = 0;
    for (size_t i = 0; i < before.Count (); ++i) {
        if (layer.status[i] != ReceptorStatus::Available)
            continue;
        EXPECT_EQ (grid.faces[kept], before.faces[i]);
        EXPECT_EQ (grid.cellColumns[kept], before.cellColumns[i]);
        EXPECT_EQ (grid.cellRows[kept], before.cellRows[i]);
        EXPECT_NEAR (grid.positions[kept * 3 + 2], 3.0, 1.0e-12);
        EXPECT_GE (grid.positions[kept * 3], 4.0) << "nothing under the balcony is traced";
        area += grid.areas[kept];
        ++kept;
    }
    EXPECT_EQ (kept, grid.Count ());
    EXPECT_NEAR (area, layer.availableArea, 1.0e-9);

    ReceptorLayer foreign;
    foreign.status.resize (1);
    SampleGrid untouched = Bed ();
    EXPECT_FALSE (KeepAvailableReceptors (untouched, foreign));
    EXPECT_EQ (untouched.Count (), before.Count ());
}

TEST (SunStudyReceptor, UnavailableTexelsAreTheirOwnStateAndNeverOverwriteAMeasurement)
{
    std::shared_ptr<geomsrv::Snapshot> snapshot;
    StudyRecord record;
    BalconyScene (snapshot, record);
    const OverheadDistance overhead = [&record] (const double host[3]) {
        return SunStudyOverheadDistance (*record.occluders, host);
    };
    SampleGrid grid = Bed ();
    ReceptorOptions tree;
    tree.height = 3.0;
    ReceptorLayer layer;
    std::string error;
    ASSERT_TRUE (BuildReceptorLayer (grid, kOffset, tree, &overhead, layer, error)) << error;
    const std::vector<ReceptorCell> cells = UnavailableReceptorCells (grid, layer);
    ASSERT_EQ (cells.size (), layer.blocked);
    ASSERT_TRUE (KeepAvailableReceptors (grid, layer));

    // The atlas is built from the kept samples, exactly as StartSunStudy does.
    const SunStudyAtlas atlas = kernels::BuildSunStudyAtlas (grid);
    ASSERT_TRUE (atlas.valid);
    const std::vector<uint32_t> texels = ReceptorCellTexels (atlas, cells);
    ASSERT_EQ (texels.size (), cells.size ()) << "every host face keeps its tile";
    std::vector<double> hours (grid.Count (), 5.0);
    std::vector<float> image = kernels::ScatterToAtlas (atlas, hours, -1.0f);
    ASSERT_FALSE (image.empty ());
    for (const uint32_t texel : texels)
        EXPECT_LT (image[texel], 0.0f) << "an unavailable cell holds no measurement";

    EXPECT_EQ (StampUnavailableTexels (image, texels), texels.size ());
    size_t unavailable = 0, measured = 0;
    for (const float value : image) {
        unavailable += value == kUnavailableTexel;
        measured += value == 5.0f;
    }
    EXPECT_EQ (unavailable, texels.size ());
    EXPECT_EQ (measured, grid.Count ()) << "no measured texel was stamped";

    // A texel that holds a value is left alone, and out-of-range indices are ignored.
    std::vector<float> small { 2.0f, -1.0f };
    EXPECT_EQ (StampUnavailableTexels (small, { 0u, 1u, 7u }), 1u);
    EXPECT_EQ (small[0], 2.0f);
    EXPECT_EQ (small[1], kUnavailableTexel);
}

TEST (SunStudyReceptor, UnavailableAreaIsSplitByHostAndWhy)
{
    // #278 area table: the balcony's blocked strip, per host and by reason.
    std::shared_ptr<geomsrv::Snapshot> snapshot;
    StudyRecord record;
    BalconyScene (snapshot, record);
    const OverheadDistance overhead = [&record] (const double host[3]) {
        return SunStudyOverheadDistance (record, host);
    };
    SampleGrid grid = Bed ();
    for (size_t i = 0; i < grid.groups.size (); ++i)
        grid.groups[i] = grid.positions[i * 3] < 5.0 ? 3u : 7u; // two hosts, split at x 5
    ReceptorOptions tree;
    tree.height = 3.0;
    ReceptorLayer layer;
    std::string error;
    ASSERT_TRUE (BuildReceptorLayer (grid, kOffset, tree, &overhead, layer, error)) << error;
    const auto losses = UnavailableByGroup (grid, layer);
    ASSERT_EQ (losses.size (), 1u) << "only the west host is under the balcony";
    EXPECT_EQ (losses[0].group, 3u);
    EXPECT_NEAR (losses[0].blockedArea, layer.unavailableArea, 1.0e-9);
    EXPECT_EQ (losses[0].steepArea, 0.0);
    // After compaction the layer no longer matches: nothing, never a guess.
    SampleGrid kept = grid;
    ASSERT_TRUE (KeepAvailableReceptors (kept, layer));
    EXPECT_TRUE (UnavailableByGroup (kept, layer).empty ());
}

// #278 Stage 2: the room a mature crown has. The balcony scene, plus a wall
// along the bed's east edge, 6 m tall.
TEST (SunStudyCrownClearance, OverheadAndFreeRadiusPerLevel)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 2782;
    snapshot->meshes = { evptest::MakeBox ("bed", 0, 0, -0.3, 10, 10, 0.3),
                         evptest::MakeBox ("balcony", 0, 0, 2.5, 4, 10, 0.2),
                         evptest::MakeBox ("wall", 10, 0, 0, 0.3, 10, 6) };
    const auto occluders = BuildSunStudyOccluders (*snapshot, kernels::ResolveElementRoles (*snapshot, {}, {}, {}));
    ASSERT_NE (occluders, nullptr);

    // Open ground at (6, 5); under the balcony at (2, 5); and (6, 5) sunk 0.2 m
    // into its own host, whose sides must not read as walls.
    const std::vector<double> x { 6, 2, 6 }, y { 5, 5, 5 }, z { 0, 0, -0.2 };
    const std::vector<uint32_t> host { 0, 0, 0 };
    CrownClearanceOptions options;
    options.levels = { 0.15, 1.0, 2.6, 7.0 };
    options.reach = 12.0;
    options.maxParallel = 1;
    CrownClearance room;
    std::string error;
    std::atomic<size_t> done { 0 };
    ASSERT_TRUE (MeasureCrownClearance (*occluders, x, y, z, host, { "bed" }, options, room, error, {}, &done))
        << error;
    ASSERT_EQ (room.Count (), 3u);
    EXPECT_EQ (done.load (), 3u);

    EXPECT_LT (room.overhead[0], 0.0f) << "open sky";
    EXPECT_NEAR (room.FreeAt (0, 1), 4.0, 1e-4) << "the wall is 4 m east";
    EXPECT_NEAR (room.FreeAt (0, 2), 2.0, 1e-4)
        << "at 2.6 m, through the balcony's thickness, its edge (x = 4) is nearer";
    EXPECT_NEAR (room.FreeAt (0, 3), 12.0, 1e-4) << "above the 6 m wall nothing is in reach";

    EXPECT_NEAR (room.overhead[1], 2.5 - 0.001, 1e-4) << "the balcony's underside";
    EXPECT_NEAR (room.FreeAt (1, 1), 8.0, 1e-4);
    EXPECT_EQ (room.FreeAt (1, 2), 0.0f) << "2.6 m is above the balcony's underside: no crown there";
    EXPECT_EQ (room.FreeAt (1, 3), 0.0f);

    EXPECT_NEAR (room.FreeAt (2, 0), 12.0, 1e-4) << "the sample's own host is not an obstacle";
}

TEST (SunStudyCrownClearance, MismatchedInputAndCancellationAreRefused)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 2783;
    snapshot->meshes = { evptest::MakeBox ("bed", 0, 0, -0.3, 10, 10, 0.3) };
    const auto occluders = BuildSunStudyOccluders (*snapshot, kernels::ResolveElementRoles (*snapshot, {}, {}, {}));
    CrownClearance room;
    std::string error;
    EXPECT_FALSE (MeasureCrownClearance (*occluders, { 1, 2 }, { 1 }, { 0, 0 }, { 0, 0 }, { "bed" }, {}, room, error));
    EXPECT_FALSE (error.empty ());
    std::vector<double> many (500, 5.0), ground (500, 0.0);
    const std::vector<uint32_t> host (500, 0);
    EXPECT_FALSE (
        MeasureCrownClearance (*occluders, many, many, ground, host, { "bed" }, {}, room, error, [] { return true; }));
    EXPECT_EQ (error, "cancelled");
    EXPECT_TRUE (room.Empty ());
}
