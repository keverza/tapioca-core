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

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace evp::rayworker;

void AddBox (packet::Scene& scene, double x0, double y0, double z0, double x1, double y1, double z1)
{
    const uint32_t base = static_cast<uint32_t> (scene.vertices.size () / 3);
    for (int i = 0; i < 8; ++i) {
        scene.vertices.push_back ((i & 1) ? x1 : x0);
        scene.vertices.push_back ((i & 2) ? y1 : y0);
        scene.vertices.push_back ((i & 4) ? z1 : z0);
    }
    const uint32_t faces[12][3] = { { 0, 1, 3 }, { 0, 3, 2 }, { 4, 6, 7 }, { 4, 7, 5 }, { 0, 4, 5 }, { 0, 5, 1 },
                                    { 2, 3, 7 }, { 2, 7, 6 }, { 0, 2, 6 }, { 0, 6, 4 }, { 1, 5, 7 }, { 1, 7, 3 } };
    for (const auto& f : faces) {
        for (const uint32_t corner : f)
            scene.triangles.push_back (base + corner);
        scene.triangleMesh.push_back (scene.meshCount);
    }
    ++scene.meshCount;
}

void AddGround (packet::Scene& scene, double half, double z)
{
    const uint32_t base = static_cast<uint32_t> (scene.vertices.size () / 3);
    const double corners[4][2] = { { -half, -half }, { half, -half }, { half, half }, { -half, half } };
    for (const auto& c : corners)
        scene.vertices.insert (scene.vertices.end (), { c[0], c[1], z });
    scene.triangles.insert (scene.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    scene.triangleMesh.insert (scene.triangleMesh.end (), { scene.meshCount, scene.meshCount });
    ++scene.meshCount;
}

void Translate (packet::Scene& scene, packet::Request& request, double dx, double dy, double dz)
{
    for (size_t i = 0; i < scene.vertices.size (); i += 3) {
        scene.vertices[i] += dx;
        scene.vertices[i + 1] += dy;
        scene.vertices[i + 2] += dz;
    }
    for (size_t i = 0; i < request.positions.size (); i += 3) {
        request.positions[i] += dx;
        request.positions[i + 1] += dy;
        request.positions[i + 2] += dz;
    }
}

// A ground plane with a few boxes; samples on the ground (up-facing) and on a
// box wall (east-facing), a day of sun directions over the sky.
void StreetScene (packet::Scene& scene, packet::Request& request)
{
    AddGround (scene, 60.0, 0.0);
    AddBox (scene, -10.0, -4.0, 0.0, -2.0, 4.0, 12.0);
    AddBox (scene, 3.0, -8.0, 0.0, 9.0, -2.0, 20.0);
    AddBox (scene, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0); // floating: light passes under it
    for (int iy = 0; iy < 64; ++iy)
        for (int ix = 0; ix < 64; ++ix) {
            request.positions.insert (request.positions.end (),
                                      { -30.0 + 0.9371 * ix + 0.1234, -30.0 + 0.9377 * iy + 0.0567, 0.0 });
            request.normals.insert (request.normals.end (), { 0.0, 0.0, 1.0 });
        }
    for (int iz = 0; iz < 12; ++iz)
        for (int iy = 0; iy < 8; ++iy) {
            request.positions.insert (request.positions.end (), { 9.0, -7.7 + 0.71 * iy, 0.613 + 1.57 * iz });
            request.normals.insert (request.normals.end (), { 1.0, 0.0, 0.0 });
        }
    const int steps = 40;
    request.totalSteps = static_cast<uint64_t> (steps + 7); // some steps untraced
    for (int k = 0; k < steps; ++k) {
        const double azimuth = 0.1 + 6.0 * k / steps; // radians
        const double elevation = 0.05 + 1.3 * std::fabs (std::sin (0.37 * k + 0.2));
        request.steps.push_back (static_cast<uint32_t> (k + (k > 20 ? 5 : 0)));
        // Non-unit on purpose: tmin/tmax stay metres, the backend normalises.
        const double scale = 1.0 + 0.5 * (k % 3);
        request.directions.insert (request.directions.end (),
                                   { scale * std::cos (elevation) * std::cos (azimuth),
                                     scale * std::cos (elevation) * std::sin (azimuth), scale * std::sin (elevation) });
    }
}

struct Comparison {
    size_t differingBits = 0;
    size_t litBits = 0;
};

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
        auto cpu = MakeCpuReferenceBackend ();
        protocol::SceneReadyPayload ready;
        ready.sceneEpoch = 1;
        std::string error;
        EXPECT_EQ (cpu->BuildScene (scene, ready, error), GpuReason::None) << error;
        EXPECT_EQ (dxr_->BuildScene (scene, ready, error), GpuReason::None) << error;
        packet::Result expected, actual;
        protocol::RunTimings timings;
        EXPECT_EQ (cpu->Trace (request, expected, timings, {}, error), GpuReason::None) << error;
        EXPECT_EQ (dxr_->Trace (request, actual, timings, {}, error), GpuReason::None) << error;
        EXPECT_EQ (actual.sampleCount, expected.sampleCount);
        EXPECT_EQ (actual.stepCount, expected.stepCount);
        EXPECT_EQ (actual.resolved, expected.resolved);
        Comparison out;
        if (actual.bits.size () != expected.bits.size ()) {
            ADD_FAILURE () << "bitset sizes differ";
            return out;
        }
        for (size_t w = 0; w < expected.bits.size (); ++w) {
            uint64_t diff = actual.bits[w] ^ expected.bits[w];
            for (; diff != 0; diff &= diff - 1)
                ++out.differingBits;
            for (uint64_t lit = expected.bits[w]; lit != 0; lit &= lit - 1)
                ++out.litBits;
        }
        return out;
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
