// Planting receptor layers (#278 Stage 1A): the C++ wrapper over the Rust
// `receptor` kernel and the record's overhead test.

#include "SunStudy/SunStudyReceptor.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyLiftedPoint.hpp"

#include "MeshFixtures.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

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
