#ifndef EVP_TESTS_RAYWORKER_SCENES_HPP
#define EVP_TESTS_RAYWORKER_SCENES_HPP

// Scenes and the CPU-reference comparison shared by the hardware backend
// differentials (test_dxrbackend.cpp, test_optixbackend.cpp): every backend
// is held to the same fixtures and the same zero-differing-bits bar.

#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerService.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace rayworkertest {

using namespace evp::rayworker;

inline void AddBox (packet::Scene& scene, double x0, double y0, double z0, double x1, double y1, double z1)
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

inline void AddGround (packet::Scene& scene, double half, double z)
{
    const uint32_t base = static_cast<uint32_t> (scene.vertices.size () / 3);
    const double corners[4][2] = { { -half, -half }, { half, -half }, { half, half }, { -half, half } };
    for (const auto& c : corners)
        scene.vertices.insert (scene.vertices.end (), { c[0], c[1], z });
    scene.triangles.insert (scene.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    scene.triangleMesh.insert (scene.triangleMesh.end (), { scene.meshCount, scene.meshCount });
    ++scene.meshCount;
}

inline void Translate (packet::Scene& scene, packet::Request& request, double dx, double dy, double dz)
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
inline void StreetScene (packet::Scene& scene, packet::Request& request)
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

// A dense grid of `side` x `side` boxes, 0.6 m on 1 m centres, 1-10 m tall,
// on a ground plane; up-facing samples in the 0.4 m streets and one high sun.
// The street scene's handful of boxes never exercised a deep hierarchy: this
// does (12 triangles a box, thousands of boxes).
inline void BlockCity (packet::Scene& scene, packet::Request& request, int side)
{
    AddGround (scene, double (side) + 4.0, 0.0);
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (int b = 0; b < side * side; ++b) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const double h = 1.0 + 9.0 * double (state >> 11) * (1.0 / 9007199254740992.0);
        const double x0 = double (b % side) + 0.2, y0 = double (b / side) + 0.2;
        AddBox (scene, x0, y0, 0.0, x0 + 0.6, y0 + 0.6, h);
    }
    for (int iy = 0; iy < 2 * side; ++iy)
        for (int ix = 0; ix < 2 * side; ++ix) {
            request.positions.insert (request.positions.end (), { 0.1 + 0.5 * ix, 0.05 + 0.5 * iy, 0.05 });
            request.normals.insert (request.normals.end (), { 0.0, 0.0, 1.0 });
        }
    const int steps = 6;
    request.totalSteps = steps;
    for (int k = 0; k < steps; ++k) {
        const double azimuth = 0.3 + 1.0 * k, elevation = 0.5 + 0.15 * k;
        request.steps.push_back (static_cast<uint32_t> (k));
        request.directions.insert (request.directions.end (),
                                   { std::cos (elevation) * std::cos (azimuth),
                                     std::cos (elevation) * std::sin (azimuth), std::sin (elevation) });
    }
}

struct Comparison {
    size_t differingBits = 0;
    size_t litBits = 0;
};

// Run `scene`/`request` through the CPU reference and `gpu`; count the final
// step bits that differ.
inline Comparison CompareWithCpuReference (RayBackend& gpu, const packet::Scene& scene, const packet::Request& request)
{
    auto cpu = MakeCpuReferenceBackend ();
    protocol::SceneReadyPayload ready;
    ready.sceneEpoch = 1;
    std::string error;
    EXPECT_EQ (cpu->BuildScene (scene, ready, error), GpuReason::None) << error;
    EXPECT_EQ (gpu.BuildScene (scene, ready, error), GpuReason::None) << error;
    packet::Result expected, actual;
    protocol::RunTimings timings;
    EXPECT_EQ (cpu->Trace (request, expected, timings, {}, error), GpuReason::None) << error;
    EXPECT_EQ (gpu.Trace (request, actual, timings, {}, error), GpuReason::None) << error;
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

} // namespace rayworkertest

#endif
