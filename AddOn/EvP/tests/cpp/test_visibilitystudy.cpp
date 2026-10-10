#include "MeshFixtures.hpp"
#include "ArchViz/VisibilityStudyDisplay.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "SunStudy/VisibilityStudy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

using namespace evp::sunstudy;

namespace {

std::shared_ptr<const geomsrv::Snapshot> Scene (bool blocked)
{
    std::vector<geomsrv::Mesh> meshes;
    meshes.push_back (evptest::MakeBox ("from", 0.0, 0.0, 0.0, 1.0, 2.0, 2.0));
    if (blocked)
        meshes.push_back (evptest::MakeBox ("blocker", 2.0, -1.0, -1.0, 0.5, 4.0, 2.0));
    meshes.push_back (evptest::MakeBox ("to", 4.0, 0.0, 0.0, 1.0, 2.0, 2.0));
    return std::make_shared<geomsrv::Snapshot> (evptest::MakeSnapshot (std::move (meshes), blocked ? 2 : 1));
}

VisibilityStudyOptions Options ()
{
    VisibilityStudyOptions options;
    options.domain = SamplingDomain::TriangleLegacy;
    options.spacing = 0.75;
    options.normalOffset = 0.02;
    options.maxAimPoints = 24;
    options.maxParallel = 1;
    return options;
}

class VisibilityStudyControllerTests : public testing::Test {
  protected:
    void SetUp () override
    {
        geomsrv::archviz::visibilitystudy::Shutdown ();
        auto& queue = geomsrv::archviz::SceneCmdQueue::Get ();
        queue.Clear ();
        queue.SetConsumer (true);
    }

    void TearDown () override
    {
        geomsrv::archviz::visibilitystudy::Shutdown ();
        geomsrv::archviz::SceneCmdQueue::Get ().SetConsumer (false);
        geomsrv::MeshStore::Get ().Release ();
    }

    geomsrv::archviz::visibilitystudy::Request Request ()
    {
        auto snapshot = std::make_shared<geomsrv::Snapshot> (*Scene (false));
        snapshot->id = geomsrv::MeshStore::Get ().NextId ();
        snapshot->captureStamp = geomsrv::MeshStore::Get ().CaptureStamp ();
        snapshot->completeModel = true;
        geomsrv::MeshStore::Get ().PublishShared (snapshot);
        geomsrv::archviz::visibilitystudy::Request request;
        request.snapshot = snapshot;
        request.fromElements = { "from" };
        request.toElements = { "to" };
        request.options = Options ();
        return request;
    }

    bool WaitUntilIdle ()
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (geomsrv::archviz::visibilitystudy::GetStatus ().running) {
            if (std::chrono::steady_clock::now () > deadline)
                return false;
            std::this_thread::yield ();
        }
        return true;
    }
};

} // namespace

TEST (VisibilityStudy, SurfaceModeUsesFromCellsAndFractionalTargetVisibility)
{
    const auto clear = RunVisibilityStudy (Scene (false), { "FROM" }, { "TO" }, Options ());
    ASSERT_TRUE (clear.valid) << clear.error;
    EXPECT_EQ (clear.origin, VisibilityOrigin::Surfaces);
    EXPECT_EQ (clear.displayElements, std::vector<std::string> ({ "FROM" }));
    ASSERT_FALSE (clear.values.empty ());
    EXPECT_GT (clear.rayCount, 0u);
    EXPECT_GT (clear.aimPointCount, 1u);
    EXPECT_GT (clear.meanVisibility, 0.0);
    EXPECT_LT (clear.meanVisibility, 1.0);
    const auto blocked = RunVisibilityStudy (Scene (true), { "from" }, { "to" }, Options ());
    ASSERT_TRUE (blocked.valid) << blocked.error;
    EXPECT_LT (blocked.meanVisibility, clear.meanVisibility);
    EXPECT_TRUE (std::any_of (blocked.values.begin (), blocked.values.end (),
                              [] (double value) { return value > 0.0 && value < 1.0; }));
}

TEST (VisibilityStudy, PointModeColoursTargetCellsInsideTheViewCone)
{
    auto options = Options ();
    options.origin = VisibilityOrigin::Point;
    options.point[0] = 3.0;
    options.point[1] = 1.0;
    options.point[2] = 1.0;
    options.direction[0] = 1.0;
    options.direction[1] = 0.0;
    options.direction[2] = 0.0;
    options.coneDegrees = 60.0;
    const auto facing = RunVisibilityStudy (Scene (false), {}, { "to" }, options);
    ASSERT_TRUE (facing.valid) << facing.error;
    EXPECT_EQ (facing.displayElements, std::vector<std::string> ({ "to" }));
    EXPECT_GT (facing.visibleSamples, 0u);
    EXPECT_GT (facing.meanVisibility, 0.0);

    options.direction[0] = -1.0;
    const auto away = RunVisibilityStudy (Scene (false), {}, { "to" }, options);
    ASSERT_TRUE (away.valid) << away.error;
    EXPECT_EQ (away.visibleSamples, 0u);
    EXPECT_DOUBLE_EQ (away.meanVisibility, 0.0);
}

TEST (VisibilityStudy, RejectsOverlappingRolesAndHonoursCancellation)
{
    const auto overlap = RunVisibilityStudy (Scene (false), { "from", "to" }, { "to" }, Options ());
    EXPECT_FALSE (overlap.valid);
    EXPECT_NE (overlap.error.find ("must not overlap"), std::string::npos);

    std::atomic<bool> cancelled { true };
    const auto stopped = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, Options (),
                                             [&cancelled] { return cancelled.load (); });
    EXPECT_FALSE (stopped.valid);
    EXPECT_EQ (stopped.error, "visibility study cancelled");
}

TEST (VisibilityStudy, RefusesMissingAndStaleRoleMembers)
{
    const auto noTarget = RunVisibilityStudy (Scene (false), { "from" }, {}, Options ());
    EXPECT_FALSE (noTarget.valid);
    EXPECT_NE (noTarget.error.find ("TO element"), std::string::npos);

    const auto absent = RunVisibilityStudy (Scene (false), { "from" }, { "deleted-guid" }, Options ());
    EXPECT_FALSE (absent.valid);
    EXPECT_NE (absent.error.find ("absent from the live snapshot"), std::string::npos);
    const auto partiallyAbsent = RunVisibilityStudy (Scene (false), { "from" }, { "to", "deleted-guid" }, Options ());
    EXPECT_FALSE (partiallyAbsent.valid);
    EXPECT_NE (partiallyAbsent.error.find ("absent from the live snapshot"), std::string::npos);
}

TEST (VisibilityStudy, FullyBlockingGeometryProducesZeroVisibility)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    snapshot->id = 31;
    snapshot->meshes.push_back (evptest::MakeBox ("full-blocker", 2.0, -10.0, -10.0, 0.5, 22.0, 22.0));
    const auto result = RunVisibilityStudy (snapshot, { "from" }, { "to" }, Options ());
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_EQ (result.visibleSamples, 0u);
    EXPECT_DOUBLE_EQ (result.meanVisibility, 0.0);
}

TEST (VisibilityStudy, SerialAndParallelResultsAreDeterministic)
{
    auto options = Options ();
    options.spacing = 0.1;
    const auto serial = RunVisibilityStudy (Scene (true), { "from" }, { "to" }, options);
    ASSERT_TRUE (serial.valid) << serial.error;
    options.maxParallel = 4;
    const auto parallel = RunVisibilityStudy (Scene (true), { "from" }, { "to" }, options);
    ASSERT_TRUE (parallel.valid) << parallel.error;
    EXPECT_EQ (serial.values, parallel.values);
    EXPECT_EQ (serial.rayCount, parallel.rayCount);
}

TEST (VisibilityStudy, RejectsInvalidOptionsAndAnInsufficientRayBudget)
{
    auto options = Options ();
    options.maxRays = 1;
    const auto capped = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, options);
    EXPECT_FALSE (capped.valid);
    EXPECT_NE (capped.error.find ("ray budget"), std::string::npos);
    options = Options ();
    options.tmin = std::numeric_limits<double>::quiet_NaN ();
    EXPECT_FALSE (RunVisibilityStudy (Scene (false), { "from" }, { "to" }, options).valid);
    options = Options ();
    options.origin = VisibilityOrigin::Point;
    options.point[0] = std::numeric_limits<double>::infinity ();
    EXPECT_FALSE (RunVisibilityStudy (Scene (false), {}, { "to" }, options).valid);
}

TEST (VisibilityStudy, CancellationDuringCalculationNeverPublishesACompleteResult)
{
    size_t checks = 0;
    const auto result =
        RunVisibilityStudy (Scene (false), { "from" }, { "to" }, Options (), [&checks] { return ++checks >= 12; });
    EXPECT_FALSE (result.valid);
    EXPECT_EQ (result.error, "visibility study cancelled");
}

TEST (VisibilityStudy, FirstHitClassificationHonoursTheRayLowerBound)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    snapshot->id = 32;
    snapshot->meshes.push_back (evptest::MakeBox ("near-sheet", 1.025, -10.0, -10.0, 0.005, 22.0, 22.0));
    auto options = Options ();
    const auto blocked = RunVisibilityStudy (snapshot, { "from" }, { "to" }, options);
    ASSERT_TRUE (blocked.valid) << blocked.error;
    options.tmin = 0.1;
    const auto beyondLowerBound = RunVisibilityStudy (snapshot, { "from" }, { "to" }, options);
    ASSERT_TRUE (beyondLowerBound.valid) << beyondLowerBound.error;
    EXPECT_GT (beyondLowerBound.meanVisibility, blocked.meanVisibility);
}

TEST (VisibilityStudy, SmallAimCapNeverDropsAnEntireTarget)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    snapshot->id = 33;
    snapshot->meshes.push_back (evptest::MakeBox ("second-to", 4.0, 4.0, 0.0));
    auto options = Options ();
    options.maxAimPoints = 1;
    const auto result = RunVisibilityStudy (snapshot, { "from" }, { "to", "second-to" }, options);
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_EQ (result.aimPointCount, 2u);
}

TEST (VisibilityStudy, RefusesAnEmptyTargetAmongOtherwiseValidTargets)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    snapshot->id = 34;
    geomsrv::Mesh empty;
    empty.guid = "empty-to";
    snapshot->meshes.push_back (std::move (empty));
    const auto result = RunVisibilityStudy (snapshot, { "from" }, { "to", "empty-to" }, Options ());
    EXPECT_FALSE (result.valid);
    EXPECT_NE (result.error.find ("no valid target triangles"), std::string::npos);
}

TEST (VisibilityStudy, ZeroRayLowerBoundStillIncludesTheTargetEndpoint)
{
    auto options = Options ();
    options.tmin = 0.0;
    const auto result = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, options);
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_GT (result.visibleSamples, 0u);
}

TEST (VisibilityStudy, ReusesTheSunStudyAtlasAndFaceMapDisplayChannel)
{
    auto result = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, Options ());
    ASSERT_TRUE (result.valid) << result.error;
    result.id = "visibility-test";
    std::string error;
    const auto upload = geomsrv::archviz::BuildVisibilityStudyUpload (result, error);
    ASSERT_NE (upload, nullptr) << error;
    EXPECT_EQ (upload->analysisKind, 1u);
    EXPECT_EQ (upload->debugMode, static_cast<uint32_t> (geomsrv::archviz::SunStudyDebugMode::Visibility));
    ASSERT_EQ (upload->elements.size (), 1u);
    EXPECT_EQ (upload->elements.front ().guid, "from");
    ASSERT_NE (upload->texels, nullptr);
    EXPECT_TRUE (std::any_of (upload->texels->begin (), upload->texels->end (),
                              [] (float value) { return value >= 0.0f && value <= 1.0f; }));
}

TEST (VisibilityStudy, ExplicitContextAloneOccludesAndUnassignedObjectsAreIgnored)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    snapshot->id = 101;
    snapshot->meshes.push_back (evptest::MakeBox ("blocker", 2.0, -10.0, -10.0, 0.5, 22.0, 22.0));
    auto options = Options ();
    options.explicitContext = true;
    const auto clear = RunVisibilityStudy (snapshot, { "from" }, { "to" }, options);
    ASSERT_TRUE (clear.valid) << clear.error;
    EXPECT_GT (clear.visibleSamples, 0u);
    options.contextElements = { "BLOCKER" };
    const auto blocked = RunVisibilityStudy (snapshot, { "from" }, { "to" }, options);
    ASSERT_TRUE (blocked.valid) << blocked.error;
    EXPECT_EQ (blocked.visibleSamples, 0u);
    options.contextElements.clear ();
    const auto cleared = RunVisibilityStudy (snapshot, { "from" }, { "to" }, options);
    ASSERT_TRUE (cleared.valid) << cleared.error;
    EXPECT_EQ (clear.values, cleared.values);
}

TEST (VisibilityStudy, ExplicitContextRejectsOverlapsAndMissingMembers)
{
    auto options = Options ();
    options.explicitContext = true;
    for (const auto& context : std::vector<std::string> { "FROM", "TO" }) {
        options.contextElements = { context };
        const auto overlap = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, options);
        EXPECT_FALSE (overlap.valid);
        EXPECT_NE (overlap.error.find ("must not overlap"), std::string::npos);
    }
    options.contextElements = { "deleted" };
    const auto missing = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, options);
    EXPECT_FALSE (missing.valid);
    EXPECT_NE (missing.error.find ("absent"), std::string::npos);
}

TEST (VisibilityStudy, PatchDomainSharesLatticeAndAtlasAcrossCoplanarTriangleSeams)
{
    auto options = Options ();
    options.domain = SamplingDomain::SurfacePatch;
    options.explicitContext = true;
    const auto result = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, options);
    ASSERT_TRUE (result.valid) << result.error;
    EXPECT_TRUE (result.IsPatchDomain ());
    EXPECT_TRUE (result.grid.areas.empty ());
    EXPECT_EQ (result.patchGrid.spans.size (), 6u) << "one lattice per box face, not twelve triangles";
    EXPECT_EQ (result.values.size (), result.patchGrid.Count ());
    std::string error;
    const auto upload = geomsrv::archviz::BuildVisibilityStudyUpload (result, error);
    ASSERT_NE (upload, nullptr) << error;
    EXPECT_TRUE (upload->patchDomain);
    ASSERT_EQ (upload->elements.size (), 1u);
    const auto& faces = upload->elements.front ().faces;
    ASSERT_EQ (faces.size (), 12u);
    for (size_t face = 0; face < faces.size (); face += 2) {
        for (size_t component = 0; component < 4; ++component) {
            EXPECT_EQ (faces[face].tile[component], faces[face + 1].tile[component]);
            EXPECT_EQ (faces[face].originAndInvSpacing[component], faces[face + 1].originAndInvSpacing[component]);
            EXPECT_EQ (faces[face].uAxisAndStart[component], faces[face + 1].uAxisAndStart[component]);
            EXPECT_EQ (faces[face].vAxisAndStart[component], faces[face + 1].vAxisAndStart[component]);
        }
    }
    for (size_t sample = 0; sample < result.Count (); ++sample) {
        const auto texel = result.patchAtlas.TexelOf (result.patchGrid, sample);
        ASSERT_GE (texel, 0);
        EXPECT_FLOAT_EQ ((*upload->texels)[static_cast<size_t> (texel)], static_cast<float> (result.values[sample]));
    }
}

TEST (VisibilityStudy, PointPatchModeUsesExplicitContextAndFocusFirstHits)
{
    auto options = Options ();
    options.domain = SamplingDomain::SurfacePatch;
    options.explicitContext = true;
    options.origin = VisibilityOrigin::Point;
    options.point[0] = 1.5;
    options.point[1] = options.point[2] = 1.0;
    const auto clear = RunVisibilityStudy (Scene (true), {}, { "to" }, options);
    ASSERT_TRUE (clear.valid) << clear.error;
    EXPECT_GT (clear.visibleSamples, 0u);
    options.contextElements = { "blocker" };
    const auto blocked = RunVisibilityStudy (Scene (true), {}, { "to" }, options);
    ASSERT_TRUE (blocked.valid) << blocked.error;
    EXPECT_LT (blocked.meanVisibility, clear.meanVisibility);
}

TEST (VisibilityStudy, ViewerUsesSharedCaptureAndModelEditsInvalidateItsStamp)
{
    auto& store = geomsrv::MeshStore::Get ();
    auto filtered = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    filtered->id = 40;
    filtered->completeModel = false;
    store.Publish (filtered);
    auto shared = std::make_shared<geomsrv::Snapshot> (*Scene (false));
    shared->id = 41;
    shared->completeModel = true;
    shared->captureStamp = store.CaptureStamp ();
    store.PublishShared (shared);
    EXPECT_EQ (geomsrv::archviz::visibilitystudy::ViewerCapture (), shared);
    EXPECT_EQ (store.Current (), filtered);
    const auto admitted = store.CaptureStamp ();
    store.BumpCaptureStamp ();
    EXPECT_FALSE (geomsrv::archviz::visibilitystudy::CaptureIsFresh (shared, admitted));
    EXPECT_EQ (geomsrv::archviz::visibilitystudy::ViewerCapture (), nullptr);
    store.Release ();
}

TEST (VisibilityStudy, DisplayOwnershipRejectsSupersededProducersAndSurvivesAQueueRace)
{
    using namespace geomsrv::archviz;
    auto& queue = SceneCmdQueue::Get ();
    queue.Clear ();
    queue.SetConsumer (true);
    const auto sun = queue.ClaimAnalysisDisplay (0);
    const auto visibility = queue.ClaimAnalysisDisplay (1);
    EXPECT_FALSE (queue.PushAnalysisAtlas (sun, std::make_unique<SunStudyAtlasUpload> ()));
    queue.PushSunStudyAtlas (std::make_unique<SunStudyAtlasUpload> ());
    queue.PushClearSunStudy ();
    EXPECT_EQ (queue.PendingCount (), 0u);
    auto upload = std::make_unique<SunStudyAtlasUpload> ();
    upload->analysisKind = 1;
    EXPECT_TRUE (queue.PushAnalysisAtlas (visibility, std::move (upload)));
    const auto replacementSun = queue.ClaimAnalysisDisplay (0);
    EXPECT_EQ (queue.PendingCount (), 0u) << "superseded queued analysis removed before consumption";
    EXPECT_EQ (queue.PendingBytes (), 0u);
    EXPECT_FALSE (queue.PushClearAnalysis (visibility));
    EXPECT_TRUE (queue.PushAnalysisAtlas (replacementSun, std::make_unique<SunStudyAtlasUpload> ()));
    queue.SetConsumer (false);
    queue.SetConsumer (true);
    EXPECT_FALSE (queue.OwnsAnalysisDisplay (replacementSun));
    queue.SetConsumer (false);
}

TEST (VisibilityStudy, CancellationAfterAnalysisPreventsAtlasPublication)
{
    const auto result = RunVisibilityStudy (Scene (false), { "from" }, { "to" }, Options ());
    ASSERT_TRUE (result.valid);
    std::string error;
    EXPECT_EQ (geomsrv::archviz::BuildVisibilityStudyUpload (result, error, [] { return true; }), nullptr);
    EXPECT_EQ (error, "visibility display cancelled");
}

TEST (VisibilityStudy, VisibilityClearWorksForNativeResultsButCannotClearSun)
{
    using namespace geomsrv::archviz;
    auto& queue = SceneCmdQueue::Get ();
    queue.Clear ();
    queue.SetConsumer (true);
    const auto nativeVisibility = queue.ClaimAnalysisDisplay (1);
    EXPECT_TRUE (queue.PushClearAnalysisKind (1));
    EXPECT_FALSE (queue.OwnsAnalysisDisplay (nativeVisibility));
    auto packets = queue.Take (10);
    ASSERT_EQ (packets.size (), 1u);
    EXPECT_EQ (packets.front ().type, SceneCmdType::ClearSunStudy);
    const auto sun = queue.ClaimAnalysisDisplay (0);
    EXPECT_FALSE (queue.PushClearAnalysisKind (1));
    EXPECT_TRUE (queue.OwnsAnalysisDisplay (sun));
    EXPECT_EQ (queue.PendingCount (), 0u);
    queue.SetConsumer (false);
}

TEST_F (VisibilityStudyControllerTests, RejectsAnAlreadyStaleCaptureBeforeClaimingTheDisplay)
{
    auto request = Request ();
    geomsrv::MeshStore::Get ().BumpCaptureStamp ();
    auto& queue = geomsrv::archviz::SceneCmdQueue::Get ();
    const auto sun = queue.ClaimAnalysisDisplay (0);
    std::string error;
    EXPECT_FALSE (geomsrv::archviz::visibilitystudy::Submit (std::move (request), error));
    EXPECT_NE (error.find ("stale"), std::string::npos);
    EXPECT_TRUE (queue.OwnsAnalysisDisplay (sun));
    EXPECT_FALSE (geomsrv::archviz::visibilitystudy::GetStatus ().running);
}

TEST_F (VisibilityStudyControllerTests, PublishesCompletedViewerWorkAndAllowsARerun)
{
    namespace visibility = geomsrv::archviz::visibilitystudy;
    auto& queue = geomsrv::archviz::SceneCmdQueue::Get ();
    for (int run = 0; run < 2; ++run) {
        auto request = Request ();
        const auto snapshotId = request.snapshot->id;
        std::string error;
        ASSERT_TRUE (visibility::Submit (std::move (request), error)) << error;
        ASSERT_TRUE (WaitUntilIdle ());
        const auto status = visibility::GetStatus ();
        ASSERT_TRUE (status.complete) << status.error;
        EXPECT_EQ (status.snapshotId, snapshotId);
        auto packets = queue.Take (10);
        ASSERT_EQ (packets.size (), 1u);
        ASSERT_NE (packets.front ().sunStudy, nullptr);
        EXPECT_EQ (packets.front ().sunStudy->studyId, status.studyId);
        EXPECT_EQ (packets.front ().sunStudy->captureStamp, geomsrv::MeshStore::Get ().CaptureStamp ());
    }
    visibility::Shutdown ();
    EXPECT_FALSE (visibility::GetStatus ().complete);
}

TEST_F (VisibilityStudyControllerTests, ANewSunRequestCannotBeOverwrittenByTheVisibilityWorker)
{
    namespace visibility = geomsrv::archviz::visibilitystudy;
    std::string error;
    ASSERT_TRUE (visibility::Submit (Request (), error)) << error;
    auto& queue = geomsrv::archviz::SceneCmdQueue::Get ();
    const auto sun = queue.ClaimAnalysisDisplay (0);
    ASSERT_TRUE (WaitUntilIdle ());
    EXPECT_TRUE (queue.OwnsAnalysisDisplay (sun));
    EXPECT_EQ (queue.PendingCount (), 0u);
}
