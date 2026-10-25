// The DXR backend against its oracle (HANDOFF-SunStudyRust.md §7.1 G2/G5):
// the same scene and request through MakeCpuReferenceBackend and
// MakeDxrBackend must give the same lit bitset, bit for bit.
//
// ⚠️ A STRICT GPU TEST. It SKIPS, loudly, on a machine without a DXR Tier 1.0
// adapter; it never passes there. A skip is not evidence the backend works.
//
// The scenes are well conditioned on purpose (axis-aligned boxes, samples on a
// lattice that no shadow edge passes through exactly): any difference here is
// a defect in the backend, not f32 traversal at a grazing edge. Grazing and
// survey-offset scenes belong to G5's precision record, which reports counts.

#include "DxrWorker/DxrBackend.hpp"
#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerService.hpp"
#include "rayworker_scenes.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace evp::rayworker;

using namespace rayworkertest;

class DxrBackendTest : public ::testing::Test {
  protected:
    void SetUp () override
    {
        dxr_ = evp::dxrworker::MakeDxrBackend ();
        const protocol::HelloPayload hello = dxr_->Describe ();
        if (hello.reason != GpuReason::None)
            GTEST_SKIP () << "no DXR Tier 1.0 adapter (" << GpuReasonCode (hello.reason) << ": " << hello.deviceName
                          << ") -- the strict DXR differential did NOT run";
        std::printf ("[ DXR      ] %s, tier %u, driver %u\n", hello.deviceName.c_str (), hello.runtimeVersion,
                     hello.driverVersion);
    }

    Comparison Compare (const packet::Scene& scene, const packet::Request& request)
    {
        return CompareWithCpuReference (*dxr_, scene, request);
    }

    std::unique_ptr<RayBackend> dxr_;
};

TEST_F (DxrBackendTest, StreetSceneMatchesTheCpuReferenceBitForBit)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    const Comparison c = Compare (scene, request);
    EXPECT_GT (c.litBits, 0u);
    EXPECT_EQ (c.differingBits, 0u) << "of " << c.litBits << " lit bits";
}

TEST_F (DxrBackendTest, SurveyOffsetSceneMatchesThroughTheSharedOrigin)
{
    // Half a million metres east and five million north, as a national grid
    // puts a site: without the shared origin f32 would keep ~0.5 m here.
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    Translate (scene, request, 512345.0, 5123456.0, 312.0);
    const Comparison c = Compare (scene, request);
    EXPECT_EQ (c.differingBits, 0u) << "of " << c.litBits << " lit bits";
}

TEST_F (DxrBackendTest, BoundedRangeAndUnitlessNormalsMatch)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    request.normals.clear (); // always facing: every sample traced
    request.tmax = 7.5;       // blockers further than 7.5 m are ignored
    const Comparison c = Compare (scene, request);
    EXPECT_EQ (c.differingBits, 0u) << "of " << c.litBits << " lit bits";
}

TEST_F (DxrBackendTest, EmptySceneAndEmptyIntervalAreClear)
{
    packet::Scene empty;
    packet::Request request;
    packet::Scene street;
    StreetScene (street, request);
    EXPECT_EQ (Compare (empty, request).differingBits, 0u);
    request.tmax = request.tmin * 0.5; // inverted interval: nothing in it
    EXPECT_EQ (Compare (street, request).differingBits, 0u);
}

TEST_F (DxrBackendTest, ManyLaunchesGiveTheSameBitsAsOne)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    evp::dxrworker::DxrBackendOptions options;
    options.raysPerLaunch = 1000; // an odd size, so launches split steps
    dxr_ = evp::dxrworker::MakeDxrBackend (options);
    EXPECT_EQ (Compare (scene, request).differingBits, 0u);
}

TEST_F (DxrBackendTest, CancellationReportsFailureAndNoBits)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    protocol::SceneReadyPayload ready;
    std::string error;
    ASSERT_EQ (dxr_->BuildScene (scene, ready, error), GpuReason::None) << error;
    packet::Result result;
    protocol::RunTimings timings;
    EXPECT_NE (dxr_->Trace (request, result, timings, [] { return true; }, error), GpuReason::None);
}

} // namespace
