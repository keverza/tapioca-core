// A build without the Rust archive (TAPIOCA_LINK_RUST_GEOMETRY=OFF) has no
// sun-study core (P7a) and no sun-study kernels (P7b). It still compiles; a
// study is refused with the message StartSunStudy reports, every store reader
// refuses a record, the follower policy the driver holds never adopts
// anything, every kernel refuses with the same reason, and so do the viewpoint
// and visibility studies. With the archive this file has no tests: the store,
// follower, kernel and view tests run the real core.

#include "SunStudy/SunStudyFollowerPolicy.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/ViewpointStudy.hpp"
#include "SunStudy/VisibilityStudy.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#if !defined(TAPIOCA_GEOMETRY_RUST)

using namespace evp::sunstudy;

TEST (SunStudyNoCore, AStudyIsRefusedWithTheBuildsReason)
{
    StudyRecord record;
    record.positions = { 0.0, 0.0, 0.0 };
    record.normals = { 0.0, 0.0, 1.0 };
    const geomsrv::Snapshot snapshot;
    std::string error;
    EXPECT_FALSE (AttachRustStudyCore (record, snapshot, nullptr, 4096, error));
    EXPECT_EQ (error, "this build has no sun-study core");
    EXPECT_EQ (record.rustCore, nullptr);
    EXPECT_TRUE (record.CurrentProgress ().empty);

    auto& store = SunStudyStore::Get ();
    store.Clear ();
    const std::string id = store.Insert (std::make_unique<StudyRecord> ());
    ASSERT_FALSE (id.empty ());
    std::vector<double> hours, positions, normals;
    EXPECT_FALSE (store.Results (id, hours, positions, normals, nullptr, error));
    EXPECT_EQ (error, "this build has no sun-study core");
    store.Clear ();
}

TEST (SunStudyNoCore, TheFollowerPolicyNeverAdopts)
{
    SunStudyFollowerPolicy follower;
    follower.Adopt ("study", { 1, 2, 3 }, 1000);
    EXPECT_EQ (follower.State (), SunStudyFollowState::NoStudy);
    EXPECT_FALSE (follower.HasRenderableCache ());
    EXPECT_FALSE (follower.Observe ({ 2, 2, 3 }, 1100));
    EXPECT_FALSE (follower.ShouldStart (5000));
    EXPECT_EQ (follower.Describe (), "this build has no sun-study core");
}

TEST (SunStudyNoCore, EveryKernelAndBothViewsRefuseWithTheBuildsReason)
{
    const std::string reason = "this build has no sun-study core";
    std::string error;
    EXPECT_EQ (kernels::SeriesFromSteps ({ SunStep () }, 60, 0.0, &error).StepCount (), 0u);
    EXPECT_EQ (error, reason);
    error.clear ();
    EXPECT_TRUE (kernels::EnumerateTimesteps (60, 0, 24, &error).empty ());
    EXPECT_EQ (error, reason);
    error.clear ();
    EXPECT_EQ (kernels::ComputeAnalysisLimits (MachineResources (), 49, kPatchAtlasOccupancy, &error).maxSamples, 0u);
    EXPECT_EQ (error, reason);
    const geomsrv::Snapshot snapshot;
    SurfaceSamplingOptions options;
    error.clear ();
    EXPECT_FALSE (kernels::BuildSurfaceSampling (snapshot, {}, options, nullptr, {}, {}, &error).valid);
    EXPECT_EQ (error, reason);
    SunStudyPatchAtlas atlas;
    error.clear ();
    kernels::FitPatchAtlas (atlas, PatchSampleGrid (), 1, 8192, &error);
    EXPECT_EQ (error, reason);
    EXPECT_EQ (atlas.Width (), 0u);

    const auto viewpoint = RunSelectedViewpointStudy (std::make_shared<geomsrv::Snapshot> (), {}, ViewpointOptions ());
    EXPECT_FALSE (viewpoint.valid);
    EXPECT_EQ (viewpoint.error, reason);
    const auto visibility = RunSelectedVisibilityStudy (std::make_shared<geomsrv::Snapshot> (), {}, {});
    EXPECT_FALSE (visibility.valid);
    EXPECT_EQ (visibility.error, reason);
}

#endif
