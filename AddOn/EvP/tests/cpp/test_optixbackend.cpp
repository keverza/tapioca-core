// The OptiX backend against its oracle (HANDOFF-SunStudyRust.md §7.1 G3/G5):
// the same fixtures as test_dxrbackend.cpp, the same bar -- zero differing
// final step bits against MakeCpuReferenceBackend.
//
// ⚠️ A STRICT GPU TEST. It SKIPS, loudly, without an RT-capable NVIDIA device
// and a driver new enough for the OptiX SDK it was built with; it never
// passes there. Built only when the CUDA toolkit and OptiX SDK were found.

#include "OptixWorker/OptixBackend.hpp"
#include "RayWorker/RayWorkerService.hpp"
#include "rayworker_scenes.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>

namespace {

using namespace evp::rayworker;
using namespace rayworkertest;

class OptixBackendTest : public ::testing::Test {
  protected:
    void SetUp () override
    {
        optix_ = evp::optixworker::MakeOptixBackend ();
        const protocol::HelloPayload hello = optix_->Describe ();
        if (hello.reason != GpuReason::None)
            GTEST_SKIP () << "no OptiX device (" << GpuReasonCode (hello.reason) << ": " << hello.deviceName
                          << ") -- the strict OptiX differential did NOT run";
        std::printf ("[ OPTIX    ] %s, compute %u, driver CUDA %u, OptiX %u\n", hello.deviceName.c_str (),
                     hello.computeCapability, hello.driverVersion, hello.runtimeVersion);
    }

    std::unique_ptr<RayBackend> optix_;
};

TEST_F (OptixBackendTest, StreetSceneMatchesTheCpuReferenceBitForBit)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    const Comparison c = CompareWithCpuReference (*optix_, scene, request);
    EXPECT_GT (c.litBits, 0u);
    EXPECT_EQ (c.differingBits, 0u) << "of " << c.litBits << " lit bits";
}

TEST_F (OptixBackendTest, SurveyOffsetSceneMatchesThroughTheSharedOrigin)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    Translate (scene, request, 512345.0, 5123456.0, 312.0);
    const Comparison c = CompareWithCpuReference (*optix_, scene, request);
    EXPECT_EQ (c.differingBits, 0u) << "of " << c.litBits << " lit bits";
}

TEST_F (OptixBackendTest, BoundedRangeAndUnitlessNormalsMatch)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    request.normals.clear ();
    request.tmax = 7.5;
    const Comparison c = CompareWithCpuReference (*optix_, scene, request);
    EXPECT_EQ (c.differingBits, 0u) << "of " << c.litBits << " lit bits";
}

TEST_F (OptixBackendTest, EmptySceneAndEmptyIntervalAreClear)
{
    packet::Scene empty;
    packet::Request request;
    packet::Scene street;
    StreetScene (street, request);
    EXPECT_EQ (CompareWithCpuReference (*optix_, empty, request).differingBits, 0u);
    request.tmax = request.tmin * 0.5;
    EXPECT_EQ (CompareWithCpuReference (*optix_, street, request).differingBits, 0u);
}

TEST_F (OptixBackendTest, ManyLaunchesGiveTheSameBitsAsOne)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    evp::optixworker::OptixBackendOptions options;
    options.raysPerLaunch = 1000;
    optix_ = evp::optixworker::MakeOptixBackend (options);
    EXPECT_EQ (CompareWithCpuReference (*optix_, scene, request).differingBits, 0u);
}

TEST_F (OptixBackendTest, CancellationReportsFailureAndNoBits)
{
    packet::Scene scene;
    packet::Request request;
    StreetScene (scene, request);
    protocol::SceneReadyPayload ready;
    std::string error;
    ASSERT_EQ (optix_->BuildScene (scene, ready, error), GpuReason::None) << error;
    packet::Result result;
    protocol::RunTimings timings;
    EXPECT_NE (optix_->Trace (request, result, timings, [] { return true; }, error), GpuReason::None);
}

} // namespace

TEST_F (OptixBackendTest, ABlockCityMatchesTheCpuReferenceAtEverySize)
{
    // From a few dozen boxes to a few thousand: a hierarchy deep enough to
    // matter, not only the street scene's five meshes.
    for (const int side : { 4, 20, 22, 40, 70 }) {
        packet::Scene scene;
        packet::Request request;
        BlockCity (scene, request, side);
        const Comparison c = CompareWithCpuReference (*optix_, scene, request);
        EXPECT_GT (c.litBits, 0u) << "side " << side;
        EXPECT_EQ (c.differingBits, 0u) << "side " << side << " (" << scene.triangleMesh.size () << " triangles), of "
                                        << c.litBits << " lit bits";
    }
}
