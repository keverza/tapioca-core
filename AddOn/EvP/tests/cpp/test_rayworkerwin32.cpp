// The GPU worker's platform half (G1, #43) over REAL Windows IPC: a named
// pipe, file-mapping segments and, where it was built, the spawned
// TapiocaDxrWorker.exe. test_rayworker.cpp covers the same conversation over
// an in-memory loopback; this file proves the bytes survive the transport and
// that a dead, missing or refusing worker becomes a CPU fallback with a reason.

#include "RayWorker/RayWorkerClient.hpp"
#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerService.hpp"
#include "RayWorker/RayWorkerTraversal.hpp"
#include "RayWorker/RayWorkerWin32.hpp"
#include "Geometry/QueryEngine.hpp"
#include "MeshFixtures.hpp"
#include "SunStudy/CpuTraversal.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace evp::rayworker;
namespace win32 = evp::rayworker::win32;

std::shared_ptr<packet::Scene> SceneWithBox ()
{
    auto scene = std::make_shared<packet::Scene> ();
    scene->vertices = { -20, -20, 0, 20, -20, 0, 20, 20, 0, -20, 20, 0 };
    scene->triangles = { 0, 1, 2, 0, 2, 3 };
    scene->triangleMesh = { 0, 0 };
    const double x0 = -2, y0 = -2, x1 = 2, y1 = 2, h = 6;
    for (int i = 0; i < 8; ++i)
        scene->vertices.insert (scene->vertices.end (), { (i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? h : 0.0 });
    const uint32_t faces[12][3] = { { 0, 1, 3 }, { 0, 3, 2 }, { 4, 6, 7 }, { 4, 7, 5 }, { 0, 4, 5 }, { 0, 5, 1 },
                                    { 2, 3, 7 }, { 2, 7, 6 }, { 0, 2, 6 }, { 0, 6, 4 }, { 1, 5, 7 }, { 1, 7, 3 } };
    for (const auto& f : faces) {
        for (const uint32_t corner : f)
            scene->triangles.push_back (4 + corner);
        scene->triangleMesh.push_back (1);
    }
    scene->meshCount = 2;
    return scene;
}

packet::Request GroundRequest ()
{
    packet::Request request;
    for (int iy = 0; iy < 24; ++iy)
        for (int ix = 0; ix < 24; ++ix) {
            request.positions.insert (request.positions.end (), { -11.9 + 1.013 * ix, -11.7 + 1.017 * iy, 0.0 });
            request.normals.insert (request.normals.end (), { 0.0, 0.0, 1.0 });
        }
    request.totalSteps = 70; // two result words per sample
    for (uint32_t k = 0; k < 70; k += 3) {
        const double azimuth = 0.09 * k, elevation = 0.2 + 0.01 * k;
        request.steps.push_back (k);
        request.directions.insert (request.directions.end (),
                                   { std::cos (elevation) * std::cos (azimuth),
                                     std::cos (elevation) * std::sin (azimuth), std::sin (elevation) });
    }
    return request;
}

packet::Result Expected (const packet::Scene& scene, const packet::Request& request)
{
    auto cpu = MakeCpuReferenceBackend ();
    protocol::SceneReadyPayload ready;
    protocol::RunTimings timings;
    std::string error;
    packet::Result result;
    EXPECT_EQ (cpu->BuildScene (scene, ready, error), GpuReason::None);
    EXPECT_EQ (cpu->Trace (request, result, timings, {}, error), GpuReason::None);
    return result;
}

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

// Drive the client from the host's events until an outcome arrives.
bool Pump (win32::WorkerProcess& host, RayWorkerClient& client, std::vector<RayWorkerOutcome>& outcomes,
           int timeoutMs = 20000)
{
    const int64_t deadline = NowMs () + timeoutMs;
    while (NowMs () < deadline) {
        auto events = host.Take ();
        for (const auto& frame : events.frames)
            client.OnFrame (frame, NowMs ());
        if (events.exited)
            client.OnProcessExited (NowMs ());
        client.Tick (NowMs ());
        auto taken = client.TakeOutcomes ();
        if (!taken.empty ()) {
            outcomes = std::move (taken);
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return false;
}

std::wstring WorkerBesideTests ()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW (nullptr, path, MAX_PATH);
    return (std::filesystem::path (path).parent_path () / L"TapiocaDxrWorker.exe").wstring ();
}

TEST (RayWorkerWin32, MappedSegmentsRoundTripAndRefuseWhatIsNotThere)
{
    win32::MappedSegmentStore writer (win32::NewSegmentPrefix ());
    std::vector<uint8_t> bytes (100000);
    for (size_t i = 0; i < bytes.size (); ++i)
        bytes[i] = static_cast<uint8_t> (i * 7);
    ASSERT_TRUE (writer.Put ("a", bytes));
    std::vector<uint8_t> read;
    ASSERT_TRUE (writer.Get ("a", read));
    EXPECT_EQ (read, bytes);
    EXPECT_FALSE (writer.Get ("missing", read));
    // A re-Put of the same name (a scene re-uploaded after a restart) replaces it.
    bytes.resize (300000, 9);
    ASSERT_TRUE (writer.Put ("a", bytes));
    ASSERT_TRUE (writer.Get ("a", read));
    EXPECT_EQ (read, bytes);
    ASSERT_TRUE (writer.Put ("empty", {}));
    ASSERT_TRUE (writer.Get ("empty", read));
    EXPECT_TRUE (read.empty ());
}

TEST (RayWorkerWin32, AStudyCrossesTheRealPipeAndSegmentsBitForBit)
{
    win32::WorkerProcess host ({}, [] (const std::wstring& pipe, const std::string& segments) {
        win32::RunWorker (pipe, segments, MakeCpuReferenceBackend (), 50);
    });
    RayWorkerClient client (host.Segments (), host.Hooks ());
    const auto scene = SceneWithBox ();
    const packet::Request request = GroundRequest ();
    const packet::Result expected = Expected (*scene, request);

    for (uint64_t generation = 1; generation <= 2; ++generation) {
        client.Submit (generation, 7, scene, request, NowMs ());
        std::vector<RayWorkerOutcome> outcomes;
        ASSERT_TRUE (Pump (host, client, outcomes)) << "no outcome within the deadline";
        ASSERT_EQ (outcomes.size (), 1u);
        EXPECT_TRUE (outcomes[0].published) << GpuReasonCode (outcomes[0].reason) << ": " << outcomes[0].message;
        EXPECT_EQ (outcomes[0].generation, generation);
        EXPECT_EQ (outcomes[0].result.bits, expected.bits);
        EXPECT_EQ (outcomes[0].result.resolved, expected.resolved);
    }
    EXPECT_EQ (client.Supervisor ().Launches (), 1u) << "the second run reuses the worker and its scene";
    EXPECT_EQ (client.Supervisor ().ResidentScene (), 7u);
    client.Shutdown ();
}

TEST (RayWorkerWin32, AMissingExecutableFallsBackWithWorkerMissing)
{
    win32::WorkerProcess host (L"C:\\does-not-exist\\TapiocaDxrWorker.exe");
    RayWorkerClient client (host.Segments (), host.Hooks ());
    client.Submit (1, 1, SceneWithBox (), GroundRequest (), NowMs ());
    const auto outcomes = client.TakeOutcomes ();
    ASSERT_EQ (outcomes.size (), 1u);
    EXPECT_FALSE (outcomes[0].published);
    EXPECT_EQ (outcomes[0].reason, GpuReason::WorkerMissing);
}

TEST (RayWorkerWin32, AWorkerThatDiesBeforeConnectingIsACrashNotAHang)
{
    // Any executable that ignores our arguments and exits stands in for a
    // worker that crashed during startup.
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW (system, MAX_PATH);
    win32::WorkerProcess host (std::wstring (system) + L"\\whoami.exe");
    RayWorkerClient client (host.Segments (), host.Hooks ());
    client.Submit (1, 1, SceneWithBox (), GroundRequest (), NowMs ());
    std::vector<RayWorkerOutcome> outcomes;
    ASSERT_TRUE (Pump (host, client, outcomes, 10000));
    ASSERT_EQ (outcomes.size (), 1u);
    EXPECT_FALSE (outcomes[0].published);
    EXPECT_EQ (outcomes[0].reason, GpuReason::WorkerCrashed);
}

TEST (RayWorkerWin32, TheDxrWorkerExecutableServesOrRefusesWithAReason)
{
    const std::wstring exe = WorkerBesideTests ();
    if (GetFileAttributesW (exe.c_str ()) == INVALID_FILE_ATTRIBUTES)
        GTEST_SKIP () << "TapiocaDxrWorker.exe was not built beside the tests";
    win32::WorkerProcess host (exe);
    RayWorkerClient client (host.Segments (), host.Hooks ());
    const auto scene = SceneWithBox ();
    const packet::Request request = GroundRequest ();
    client.Submit (1, 3, scene, request, NowMs ());
    std::vector<RayWorkerOutcome> outcomes;
    ASSERT_TRUE (Pump (host, client, outcomes)) << "no outcome within the deadline";
    ASSERT_EQ (outcomes.size (), 1u);
    if (!outcomes[0].published) {
        // A machine without DXR: the worker connected and said why.
        EXPECT_EQ (outcomes[0].reason, GpuReason::DxrUnsupported) << outcomes[0].message;
        GTEST_SKIP () << "no DXR adapter: " << outcomes[0].message;
    }
    const packet::Result expected = Expected (*scene, request);
    EXPECT_EQ (outcomes[0].result.bits, expected.bits);
    EXPECT_EQ (outcomes[0].result.resolved, expected.resolved);
    client.Shutdown ();
}

// ---- the study's traversal over the worker -----------------------------------

struct TraversalFixture {
    std::shared_ptr<const geomsrv::QueryEngine> engine;
    std::shared_ptr<const evp::sunstudy::CpuTraversal> cpu;
    std::vector<double> origins;

    TraversalFixture ()
    {
        std::vector<geomsrv::Mesh> meshes;
        meshes.push_back (evptest::MakeBox ("ground", -20.0, -20.0, -1.0, 40.0, 40.0, 1.0));
        meshes.push_back (evptest::MakeBox ("tower", -2.0, -2.0, 0.0, 4.0, 4.0, 6.0));
        engine = std::make_shared<geomsrv::QueryEngine> (
            std::make_shared<geomsrv::Snapshot> (evptest::MakeSnapshot (std::move (meshes))));
        cpu = std::make_shared<evp::sunstudy::CpuTraversal> (engine);
        for (int iy = 0; iy < 30; ++iy)
            for (int ix = 0; ix < 30; ++ix)
                origins.insert (origins.end (), { -14.9 + 1.013 * ix, -14.7 + 1.017 * iy, 0.0 });
    }

    void ExpectSameAsCpu (const evp::sunstudy::ITraversal& traversal)
    {
        const size_t count = origins.size () / 3;
        for (int k = 0; k < 6; ++k) {
            const double dir[3] = { std::cos (0.9 * k), std::sin (0.9 * k), 0.4 + 0.1 * k };
            std::vector<uint8_t> expected (count), actual (count, 7);
            cpu->OccludeDirectional (origins.data (), count, dir, 0.001, 0.0, expected.data ());
            ASSERT_TRUE (traversal.OccludeDirectionalCancellable (origins.data (), count, dir, 0.001, 0.0,
                                                                  actual.data (), 0, {}));
            EXPECT_EQ (actual, expected) << "direction " << k;
        }
    }
};

TEST (RayWorkerTraversal, AnInProcessWorkerAnswersWhatTheCpuTraversalAnswers)
{
    TraversalFixture f;
    auto channel = std::make_shared<RayWorkerChannel> (
        std::wstring (), [] (const std::wstring& pipe, const std::string& segments) {
            win32::RunWorker (pipe, segments, MakeCpuReferenceBackend (), 50);
        });
    RayWorkerTraversal traversal (f.cpu, channel, SceneFromIndices (f.engine.get (), nullptr), "dxr");
    f.ExpectSameAsCpu (traversal);
    EXPECT_EQ (traversal.ActualBackend (), "dxr");
    EXPECT_EQ (traversal.GpuCalls (), 6u);
    EXPECT_EQ (traversal.SceneVersion (), f.cpu->SceneVersion ());
    channel->Shutdown ();
}

TEST (RayWorkerTraversal, AWorkerThatDiesFallsBackToTheCpuForGoodWithAReason)
{
    TraversalFixture f;
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW (system, MAX_PATH);
    auto channel = std::make_shared<RayWorkerChannel> (std::wstring (system) + L"\\whoami.exe");
    RayWorkerTraversal traversal (f.cpu, channel, SceneFromIndices (f.engine.get (), nullptr), "dxr");
    f.ExpectSameAsCpu (traversal);
    EXPECT_EQ (traversal.ActualBackend (), "cpu");
    EXPECT_EQ (traversal.FallbackReason (), GpuReason::WorkerCrashed);
    EXPECT_EQ (traversal.GpuCalls (), 0u);
}

TEST (RayWorkerTraversal, TheDxrWorkerExecutableAnswersWhatTheCpuTraversalAnswers)
{
    const std::wstring exe = WorkerBesideTests ();
    if (GetFileAttributesW (exe.c_str ()) == INVALID_FILE_ATTRIBUTES)
        GTEST_SKIP () << "TapiocaDxrWorker.exe was not built beside the tests";
    TraversalFixture f;
    auto channel = std::make_shared<RayWorkerChannel> (exe);
    RayWorkerTraversal traversal (f.cpu, channel, SceneFromIndices (f.engine.get (), nullptr), "dxr");
    f.ExpectSameAsCpu (traversal);
    if (traversal.ActualBackend () != "dxr")
        GTEST_SKIP () << "no DXR here: " << GpuReasonCode (traversal.FallbackReason ());
    EXPECT_EQ (traversal.GpuCalls (), 6u);
    channel->Shutdown ();
}

} // namespace
