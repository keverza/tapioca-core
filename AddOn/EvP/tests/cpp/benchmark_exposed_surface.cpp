// Opt-in S0 exposed-surface measurement. This is not a correctness substitute
// for the analytic tests and is never part of the default test executable.
#include "Geometry/ExposedSurface.hpp"
#include "Geometry/SurfaceCoverage.hpp"
#include "MeshFixtures.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
namespace ex = geomsrv::exposed;
using geomsrv::Mesh;

constexpr size_t kDefaultIterations = 25;
constexpr size_t kMaximumIterations = 1000;
constexpr size_t kMultiFaceCount = 32;
constexpr double kAreaToleranceM2 = 1e-6;

struct Fixture {
    std::string name;
    geomsrv::Snapshot snapshot;
    ex::Request request;
    double expectedGrossM2 = 0;
    double expectedExposedM2 = 0;
    double actualGrossM2 = 0;
    double actualExposedM2 = 0;
    ex::Statistics statistics;
};

Mesh Box (const std::string& guid, double x, double y, double z, double sx, double sy, double sz, int32_t material = 10)
{
    Mesh mesh = evptest::MakeBox (guid, x, y, z, sx, sy, sz);
    mesh.triMaterial.assign (mesh.TriangleCount (), material);
    mesh.triSourceBody.assign (mesh.TriangleCount (), 1);
    for (int32_t polygon = 1; polygon <= 6; ++polygon)
        mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { polygon, polygon });
    return mesh;
}

void Quad (Mesh& mesh, const geomsrv::engine::Vector3& a, const geomsrv::engine::Vector3& b,
           const geomsrv::engine::Vector3& c, const geomsrv::engine::Vector3& d, int32_t polygon, int32_t material)
{
    const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());
    for (const auto& point : { a, b, c, d })
        mesh.vertices.insert (mesh.vertices.end (), { point.x, point.y, point.z });
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    mesh.triSourceBody.insert (mesh.triSourceBody.end (), { 1, 1 });
    mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { polygon, polygon });
    mesh.triMaterial.insert (mesh.triMaterial.end (), { material, material });
}

Mesh Opening ()
{
    using V = geomsrv::engine::Vector3;
    Mesh mesh;
    mesh.guid = "opening-reveals";
    const auto front = [&] (double x0, double x1, double z0, double z1) {
        Quad (mesh, { x0, 0, z0 }, { x1, 0, z0 }, { x1, 0, z1 }, { x0, 0, z1 }, 1, 10);
    };
    front (0, 5, 0, .8);
    front (0, 5, 2.3, 3);
    front (0, 1, .8, 2.3);
    front (2.2, 5, .8, 2.3);
    Quad (mesh, V { 1, 0, .8 }, V { 1, .2, .8 }, V { 1, .2, 2.3 }, V { 1, 0, 2.3 }, 2, 20);
    Quad (mesh, V { 2.2, .2, .8 }, V { 2.2, 0, .8 }, V { 2.2, 0, 2.3 }, V { 2.2, .2, 2.3 }, 3, 20);
    Quad (mesh, V { 1, 0, 2.3 }, V { 1, .2, 2.3 }, V { 2.2, .2, 2.3 }, V { 2.2, 0, 2.3 }, 4, 20);
    Quad (mesh, V { 1, .2, .8 }, V { 1, 0, .8 }, V { 2.2, 0, .8 }, V { 2.2, .2, .8 }, 5, 20);
    evptest::RecomputeBounds (mesh);
    return mesh;
}

geomsrv::Snapshot Capture (std::vector<Mesh> meshes)
{
    geomsrv::Snapshot snapshot;
    snapshot.id = 19;
    snapshot.captureStamp = 7;
    snapshot.scope = "exposed-surface-benchmark";
    snapshot.meshes = std::move (meshes);
    return snapshot;
}

ex::Request Request (std::vector<ex::FaceKey> faces, std::vector<ex::BodyKey> covers = {})
{
    ex::Request request;
    request.snapshotId = 19;
    request.captureStamp = 7;
    request.coverScopeId = "benchmark-explicit-covers";
    request.faces = std::move (faces);
    request.covers = std::move (covers);
    return request;
}

Fixture SingleFaceFixture (std::string name, std::vector<Mesh> meshes, std::vector<ex::BodyKey> covers,
                           double expectedGrossM2, double expectedExposedM2)
{
    std::vector<ex::FaceKey> faces { { { 0, 1 }, 3 } };
    Fixture fixture;
    fixture.name = std::move (name);
    fixture.snapshot = Capture (std::move (meshes));
    fixture.request = Request (std::move (faces), std::move (covers));
    fixture.expectedGrossM2 = expectedGrossM2;
    fixture.expectedExposedM2 = expectedExposedM2;
    return fixture;
}

std::vector<Fixture> MakeFixtures ()
{
    std::vector<Fixture> fixtures;
    fixtures.push_back (SingleFaceFixture ("uncovered", { Box ("uncovered-wall", 0, 0, 0, 5, .2, 3) }, {}, 15, 15));
    fixtures.push_back (SingleFaceFixture (
        "partial-contact", { Box ("partial-wall", 0, 0, 0, 5, .2, 3), Box ("partial-cover", 0, -.3, 0, 2, .3, 3) },
        { { 1, 1 } }, 15, 9));
    fixtures.push_back (
        SingleFaceFixture ("overlapping-covers",
                           { Box ("overlap-wall", 0, 0, 0, 5, .2, 3), Box ("overlap-cover-a", 0, -.3, 0, 2, .5, 3),
                             Box ("overlap-cover-b", 1, -.3, 0, 2, .5, 3) },
                           { { 1, 1 }, { 2, 1 } }, 15, 6));

    Fixture opening;
    opening.name = "opening-reveals";
    opening.snapshot = Capture ({ Opening () });
    std::vector<ex::FaceKey> openingFaces;
    for (int32_t polygon = 1; polygon <= 5; ++polygon)
        openingFaces.push_back ({ { 0, 1 }, polygon });
    opening.request = Request (std::move (openingFaces));
    opening.expectedGrossM2 = 14.28; // 5*3 - 1.2*1.5 + 2*(0.2*1.5) + 2*(1.2*0.2)
    opening.expectedExposedM2 = 14.28;
    fixtures.push_back (std::move (opening));

    Fixture workload;
    workload.name = "multi-face-32";
    std::vector<Mesh> meshes;
    std::vector<ex::FaceKey> faces;
    std::vector<ex::BodyKey> covers;
    meshes.reserve (2 * kMultiFaceCount);
    faces.reserve (kMultiFaceCount);
    covers.reserve (kMultiFaceCount);
    double expectedExposed = 0;
    for (size_t index = 0; index < kMultiFaceCount; ++index) {
        const double x = double (index) * 8.0;
        meshes.push_back (Box ("workload-wall-" + std::to_string (index), x, 0, 0, 5, .2, 3));
        faces.push_back ({ { index, 1 }, 3 });
        const double coveredWidth = .5 + double (index % 5) * .75;
        expectedExposed += 15.0 - coveredWidth * 3.0;
    }
    for (size_t index = 0; index < kMultiFaceCount; ++index) {
        const double x = double (index) * 8.0;
        const double coveredWidth = .5 + double (index % 5) * .75;
        meshes.push_back (Box ("workload-cover-" + std::to_string (index), x, -.3, 0, coveredWidth, .3, 3));
        covers.push_back ({ kMultiFaceCount + index, 1 });
    }
    workload.snapshot = Capture (std::move (meshes));
    workload.request = Request (std::move (faces), std::move (covers));
    workload.expectedGrossM2 = 15.0 * double (kMultiFaceCount);
    workload.expectedExposedM2 = expectedExposed;
    fixtures.push_back (std::move (workload));
    return fixtures;
}

void CheckArea (const Fixture& fixture, const ex::Result& result)
{
    const auto check = [&] (const char* measure, double expected, double actual) {
        if (!std::isfinite (actual) || std::abs (expected - actual) > kAreaToleranceM2) {
            throw std::runtime_error (fixture.name + ": " + measure + " area mismatch (expected " +
                                      std::to_string (expected) + ", actual " + std::to_string (actual) + ")");
        }
    };
    check ("gross", fixture.expectedGrossM2, result.areas.grossGeometricArea);
    check ("exposed", fixture.expectedExposedM2, result.areas.exposedArea);
}

void ValidateFixtures (std::vector<Fixture>& fixtures)
{
    // Run every independent analytic oracle before starting any timed sample.
    for (auto& fixture : fixtures) {
        ex::Result result;
        std::string error;
        if (!ex::Calculate (fixture.snapshot, fixture.request, result, error))
            throw std::runtime_error (fixture.name + ": preflight failed: " + error);
        CheckArea (fixture, result);
        fixture.actualGrossM2 = result.areas.grossGeometricArea;
        fixture.actualExposedM2 = result.areas.exposedArea;
        fixture.statistics = result.statistics;
    }
}

uint64_t PeakRssBytes ()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory {};
    if (!GetProcessMemoryInfo (GetCurrentProcess (), &memory, sizeof (memory)))
        throw std::runtime_error ("GetProcessMemoryInfo failed");
    return static_cast<uint64_t> (memory.PeakWorkingSetSize);
#else
    rusage usage {};
    if (getrusage (RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error ("getrusage failed");
#ifdef __APPLE__
    return static_cast<uint64_t> (usage.ru_maxrss);
#else
    return static_cast<uint64_t> (usage.ru_maxrss) * 1024;
#endif
#endif
}

size_t ParseIterations (std::string_view text)
{
    size_t iterations = 0;
    const auto parsed = std::from_chars (text.data (), text.data () + text.size (), iterations);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data () + text.size () || iterations == 0 ||
        iterations > kMaximumIterations)
        throw std::runtime_error ("iterations must be an integer from 1 to " + std::to_string (kMaximumIterations));
    return iterations;
}

double NearestRank (const std::vector<double>& sortedSamples, double percentile)
{
    const size_t index = static_cast<size_t> (std::ceil (percentile * double (sortedSamples.size ()))) - 1;
    return sortedSamples[index];
}

void Measure (Fixture& fixture, size_t iterations)
{
    std::vector<double> samples;
    samples.reserve (iterations);
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        ex::Result result;
        std::string error;
        const auto started = std::chrono::steady_clock::now ();
        const bool succeeded = ex::Calculate (fixture.snapshot, fixture.request, result, error);
        const auto finished = std::chrono::steady_clock::now ();
        if (!succeeded)
            throw std::runtime_error (fixture.name + ": timed calculation failed: " + error);
        const double elapsedMs = std::chrono::duration<double, std::milli> (finished - started).count ();
        if (!std::isfinite (elapsedMs) || elapsedMs < 0)
            throw std::runtime_error (fixture.name + ": invalid elapsed time");
        CheckArea (fixture, result);
        samples.push_back (elapsedMs);
        fixture.actualGrossM2 = result.areas.grossGeometricArea;
        fixture.actualExposedM2 = result.areas.exposedArea;
        fixture.statistics = result.statistics;
    }
    std::sort (samples.begin (), samples.end ());

    std::cout << fixture.name << ',' << fixture.request.faces.size () << ',' << fixture.request.covers.size () << ','
              << fixture.snapshot.meshes.size () << ',' << fixture.snapshot.TotalTriangles () << ','
              << fixture.expectedGrossM2 << ',' << fixture.actualGrossM2 << ',' << fixture.expectedExposedM2 << ','
              << fixture.actualExposedM2 << ',' << fixture.statistics.candidateCoverPairs << ','
              << fixture.statistics.sectionCount << ',' << fixture.statistics.work << ','
              << fixture.statistics.outputTriangles << ',' << iterations << ',' << NearestRank (samples, .50) << ','
              << NearestRank (samples, .95) << ',' << samples.back () << ',' << PeakRssBytes () << '\n';
}
} // namespace

int main (int argc, char** argv)
{
    try {
        size_t iterations = kDefaultIterations;
        if (argc == 3 && std::string_view (argv[1]) == "--iterations")
            iterations = ParseIterations (argv[2]);
        else if (argc != 1) {
            std::cerr << "usage: TapiocaExposedSurfaceBench [--iterations 1.." << kMaximumIterations << "]\n";
            return 2;
        }

        auto fixtures = MakeFixtures ();
        ValidateFixtures (fixtures);

        std::cout << "configuration,fixture,target_faces,cover_bodies,mesh_count,input_triangles,expected_gross_m2,"
                     "actual_gross_m2,expected_exposed_m2,actual_exposed_m2,candidate_cover_pairs,sections,work,"
                     "output_triangles,iterations,p50_ms,p95_ms,max_ms,peak_rss_bytes\n";
        std::cout << std::setprecision (12);
        for (auto& fixture : fixtures) {
            std::cout << TAPIOCA_BENCH_CONFIG << ',';
            Measure (fixture, iterations);
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "TapiocaExposedSurfaceBench: " << error.what () << '\n';
        return 1;
    }
}
