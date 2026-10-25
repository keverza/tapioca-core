// The GPU ray worker's GPU-free half (raycast plan E7): the control wire, the
// shared-memory packets, the supervisor's lifecycle, and the whole
// conversation end to end over an in-memory loopback with the CPU reference
// backend, whose results must equal the sun study's own accumulator bit for
// bit. The hardware backend and the platform transport plug into the seams
// tested here.

#include "Geometry/QueryEngine.hpp"
#include "RayWorker/RayWorkerClient.hpp"
#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerProtocol.hpp"
#include "RayWorker/RayWorkerService.hpp"
#include "RayWorker/RayWorkerSupervisor.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace evp::rayworker;
namespace proto = evp::rayworker::protocol;

// ---- fixtures ---------------------------------------------------------------

void AppendQuad (geomsrv::Mesh& mesh, const double o[3], const double u[3], const double v[3])
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int corner = 0; corner < 4; ++corner) {
        const double a = corner == 1 || corner == 2 ? 1.0 : 0.0, b = corner >= 2 ? 1.0 : 0.0;
        for (int axis = 0; axis < 3; ++axis)
            mesh.vertices.push_back (o[axis] + a * u[axis] + b * v[axis]);
    }
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
}

geomsrv::Mesh Box (const std::string& guid, double x, double y, double w, double d, double h)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double top[3] = { x, y, h }, ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h };
    const double south[3] = { x, y, 0 }, north[3] = { x + w, y + d, 0 }, wx[3] = { -w, 0, 0 }, dy[3] = { 0, -d, 0 };
    AppendQuad (mesh, top, ex, ey);
    AppendQuad (mesh, south, ex, ez);
    AppendQuad (mesh, north, wx, ez);
    AppendQuad (mesh, south, ez, ey);
    AppendQuad (mesh, north, ez, dy);
    return mesh;
}

std::shared_ptr<geomsrv::Snapshot> City (uint64_t id, double shift)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = id;
    snapshot->meshes = { Box ("a", 0 + shift, 0, 4, 4, 9), Box ("b", 8, 3, 3, 5, 4), Box ("c", -6, 6, 2, 2, 12) };
    return snapshot;
}

// A ground grid and a day, as the sun study samples them.
packet::Request Day (size_t steps)
{
    packet::Request request;
    for (double y = -10; y <= 14; y += 0.75)
        for (double x = -12; x <= 16; x += 0.75) {
            request.positions.insert (request.positions.end (), { x, y, 0.1 });
            request.normals.insert (request.normals.end (), { 0, 0, 1 });
        }
    request.totalSteps = steps;
    for (uint32_t s = 0; s < steps; ++s) {
        const double azimuth = 0.4 + 0.25 * s, altitude = 0.2 + 0.06 * (s % 7);
        request.steps.push_back (s);
        request.directions.insert (request.directions.end (),
                                   { std::cos (altitude) * std::cos (azimuth), std::cos (altitude) * std::sin (azimuth),
                                     std::sin (altitude) });
    }
    return request;
}

// The oracle: the sun study's accumulator over the original snapshot.
std::vector<uint64_t> Oracle (const std::shared_ptr<geomsrv::Snapshot>& snapshot, const packet::Request& request)
{
    const evp::sunstudy::CpuTraversal traversal (std::make_shared<geomsrv::QueryEngine> (snapshot));
    evp::sunstudy::OcclusionAccumulator accumulator (request.positions.size () / 3, request.totalSteps);
    evp::sunstudy::SampleSet set;
    set.positions = request.positions.data ();
    set.normals = request.normals.data ();
    set.count = request.positions.size () / 3;
    for (size_t i = 0; i < request.steps.size (); ++i)
        accumulator.AccumulateStep (traversal, set, request.steps[i], &request.directions[i * 3], request.tmin,
                                    request.tmax);
    return accumulator.Bits ();
}

// ---- the loopback -------------------------------------------------------------

struct Loopback {
    MemorySegmentStore segments;
    std::unique_ptr<RayWorkerService> worker;
    std::deque<std::vector<uint8_t>> toHost;
    std::unique_ptr<RayWorkerClient> client;
    bool startable = true;
    bool deliver = true; // false: the worker hangs and answers nothing
    int launches = 0, kills = 0, uploads = 0;
    std::function<std::unique_ptr<RayBackend> ()> backend = [] { return MakeCpuReferenceBackend (); };
    std::function<void (MemorySegmentStore&)> tamper; // after a worker reply, before the host reads

    Loopback ()
    {
        RayWorkerHooks hooks;
        hooks.launch = [this] {
            if (!startable)
                return false;
            ++launches;
            worker = std::make_unique<RayWorkerService> (backend (), segments);
            toHost.push_back (worker->Hello ());
            return true;
        };
        hooks.kill = [this] {
            ++kills;
            worker.reset ();
        };
        hooks.send = [this] (std::vector<uint8_t> frame) {
            proto::Header header;
            std::string error;
            if (proto::DecodeHeader (frame.data (), frame.size (), header, error) &&
                header.messageType == proto::MessageType::UploadScene)
                ++uploads;
            if (worker == nullptr || !deliver)
                return;
            for (auto& reply : worker->OnFrame (frame))
                toHost.push_back (std::move (reply));
            if (tamper)
                tamper (segments);
        };
        client = std::make_unique<RayWorkerClient> (segments, std::move (hooks));
    }

    void Pump (int64_t now = 0)
    {
        while (!toHost.empty ()) {
            auto frame = std::move (toHost.front ());
            toHost.pop_front ();
            client->OnFrame (frame, now);
        }
    }

    std::vector<RayWorkerOutcome> Run (uint64_t generation, uint64_t epoch,
                                       const std::shared_ptr<const packet::Scene>& scene,
                                       const packet::Request& request, int64_t now = 0)
    {
        client->Submit (generation, epoch, scene, request, now);
        Pump (now);
        return client->TakeOutcomes ();
    }
};

} // namespace

// ---- protocol -------------------------------------------------------------------

TEST (RayWorkerProtocol, EveryPayloadRoundTrips)
{
    proto::HelloPayload hello;
    hello.processId = 42;
    hello.capabilities = proto::CapabilityHardwareRayTracing;
    hello.reason = GpuReason::DriverIncompatible;
    hello.driverVersion = 55299;
    hello.runtimeVersion = 80000;
    hello.computeCapability = 89;
    hello.deviceMemoryBytes = 12ull << 30;
    hello.deviceName = "device";
    proto::HelloPayload back;
    std::string error;
    const auto bytes = proto::Encode (hello);
    ASSERT_TRUE (proto::Decode (bytes.data (), bytes.size (), back, error)) << error;
    EXPECT_EQ (back.deviceMemoryBytes, hello.deviceMemoryBytes);
    EXPECT_EQ (back.reason, GpuReason::DriverIncompatible);
    EXPECT_EQ (back.deviceName, "device");

    proto::RunStudyPayload run { 7, 3, { "req", 100, 5 }, { "res", 200, 0 } };
    proto::RunStudyPayload runBack;
    const auto runBytes = proto::Encode (run);
    ASSERT_TRUE (proto::Decode (runBytes.data (), runBytes.size (), runBack, error)) << error;
    EXPECT_EQ (runBack.request.name, "req");
    EXPECT_EQ (runBack.result.bytes, 200u);

    proto::RunResultPayload done { 7, 3, 999, 0xabcdef, { 1, 2, 3 } };
    proto::RunResultPayload doneBack;
    const auto doneBytes = proto::Encode (done);
    ASSERT_TRUE (proto::Decode (doneBytes.data (), doneBytes.size (), doneBack, error)) << error;
    EXPECT_EQ (doneBack.checksum, 0xabcdefu);
    EXPECT_EQ (doneBack.timings.readback, 3u);
}

TEST (RayWorkerProtocol, MalformedFramesAreRefusedBeforeAllocation)
{
    std::string error;
    proto::Header header;
    auto frame = proto::EncodeHeader (proto::MessageType::Hello, 1, 0, 0);
    frame[0] = 9; // another version
    EXPECT_FALSE (proto::DecodeHeader (frame.data (), frame.size (), header, error));
    EXPECT_NE (error.find ("protocol version 9"), std::string::npos) << error;

    auto big = proto::EncodeHeader (proto::MessageType::Log, 1, 0, proto::MaxPayloadBytes + 1);
    EXPECT_FALSE (proto::DecodeHeader (big.data (), big.size (), header, error));

    auto unknown = proto::EncodeHeader (proto::MessageType::Log, 1, 0, 0);
    unknown[4] = 99;
    EXPECT_FALSE (proto::DecodeHeader (unknown.data (), unknown.size (), header, error));

    // A text length over the limit is refused, not allocated.
    std::vector<uint8_t> text = { 0xff, 0xff, 0xff, 0x7f };
    std::string utf8;
    EXPECT_FALSE (proto::DecodeText (text.data (), text.size (), utf8, error));

    // Trailing bytes and truncation.
    auto cancel = proto::Encode (proto::CancelPayload { 5 });
    proto::CancelPayload c;
    cancel.push_back (0);
    EXPECT_FALSE (proto::Decode (cancel.data (), cancel.size (), c, error));
    cancel.resize (4);
    EXPECT_FALSE (proto::Decode (cancel.data (), cancel.size (), c, error));

    // An unknown reason code is refused.
    auto failed = proto::Encode (proto::FailedPayload { GpuReason::OutOfMemory, "oom" });
    failed[0] = 200;
    proto::FailedPayload f;
    EXPECT_FALSE (proto::Decode (failed.data (), failed.size (), f, error));
}

TEST (RayWorkerProtocol, ReasonCodesAreTheContractStrings)
{
    EXPECT_STREQ (GpuReasonCode (GpuReason::WorkerCrashed), "worker_crashed");
    EXPECT_STREQ (GpuReasonCode (GpuReason::DriverIncompatible), "driver_incompatible");
    EXPECT_STREQ (GpuReasonCode (GpuReason::GpuUnsupported), "gpu_unsupported");
    EXPECT_STREQ (GpuReasonCode (GpuReason::OutOfMemory), "out_of_memory");
    EXPECT_STREQ (GpuReasonCode (GpuReason::WorkerMissing), "worker_missing");
    EXPECT_STREQ (GpuReasonCode (GpuReason::DisabledForSession), "gpu_disabled_for_session");
    EXPECT_STREQ (GpuReasonCode (GpuReason::DxrUnsupported), "dxr_unsupported");
    EXPECT_STREQ (GpuReasonCode (GpuReason::WorkerTimeout), "worker_timeout");
    EXPECT_STREQ (GpuReasonCode (GpuReason::DeviceRemoved), "device_removed");
    EXPECT_STREQ (GpuReasonCode (GpuReason::PrecisionUnsupported), "precision_unsupported");
    EXPECT_STREQ (GpuReasonCode (GpuReason::ValidationFailed), "validation_failed");
    // The wire value is contract too: a newer worker's reason must be known here.
    EXPECT_EQ (static_cast<uint32_t> (GpuReason::ValidationFailed), 13u);
    EXPECT_TRUE (IsKnownGpuReason (13));
    EXPECT_FALSE (IsKnownGpuReason (14));
    EXPECT_FALSE (IsKnownGpuReason (99));
}

// ---- packets ----------------------------------------------------------------------

TEST (RayWorkerPacket, ScenesRequestsAndResultsRoundTripAndAreValidated)
{
    const auto scene = packet::SceneFromSnapshot (*City (1, 0));
    const auto bytes = packet::Write (11, scene);
    const uint64_t checksum = packet::StoredChecksum (bytes);
    packet::Scene back;
    std::string error;
    ASSERT_TRUE (packet::Read (bytes.data (), bytes.size (), 11, checksum, back, error)) << error;
    EXPECT_EQ (back.vertices, scene.vertices);
    EXPECT_EQ (back.triangleMesh, scene.triangleMesh);
    EXPECT_FALSE (packet::Read (bytes.data (), bytes.size (), 12, checksum, back, error)) << "another epoch";
    EXPECT_FALSE (packet::Read (bytes.data (), bytes.size () - 1, 11, checksum, back, error)) << "truncated";
    auto flipped = bytes;
    flipped[packet::HeaderBytes + 40] ^= 1;
    EXPECT_FALSE (packet::Read (flipped.data (), flipped.size (), 11, checksum, back, error));
    EXPECT_NE (error.find ("checksum"), std::string::npos) << error;

    // Internally inconsistent scenes are refused even with a valid checksum.
    packet::Scene bad = scene;
    bad.triangles[0] = 100000;
    const auto badBytes = packet::Write (11, bad);
    EXPECT_FALSE (
        packet::Read (badBytes.data (), badBytes.size (), 11, packet::StoredChecksum (badBytes), back, error));
    bad = scene;
    bad.vertices[0] = std::numeric_limits<double>::quiet_NaN ();
    const auto nanBytes = packet::Write (11, bad);
    EXPECT_FALSE (
        packet::Read (nanBytes.data (), nanBytes.size (), 11, packet::StoredChecksum (nanBytes), back, error));

    const auto request = Day (5);
    const auto requestBytes = packet::Write (11, 4, request);
    packet::Request requestBack;
    ASSERT_TRUE (packet::Read (requestBytes.data (), requestBytes.size (), 11, 4, packet::StoredChecksum (requestBytes),
                               requestBack, error))
        << error;
    EXPECT_EQ (requestBack.positions, request.positions);
    EXPECT_FALSE (packet::Read (requestBytes.data (), requestBytes.size (), 11, 5,
                                packet::StoredChecksum (requestBytes), requestBack, error))
        << "another run";
    packet::Scene wrongKind;
    EXPECT_FALSE (packet::Read (requestBytes.data (), requestBytes.size (), 11, packet::StoredChecksum (requestBytes),
                                wrongKind, error));
    auto unordered = request;
    std::swap (unordered.steps[0], unordered.steps[1]);
    const auto unorderedBytes = packet::Write (11, 4, unordered);
    EXPECT_FALSE (packet::Read (unorderedBytes.data (), unorderedBytes.size (), 11, 4,
                                packet::StoredChecksum (unorderedBytes), requestBack, error));
}

// ---- supervisor --------------------------------------------------------------------

TEST (RayWorkerSupervisor, LaunchesLazilyAndRefusesForTheSessionWhatCannotChange)
{
    RayWorkerSupervisor s;
    EXPECT_EQ (s.State (), WorkerState::Stopped);
    auto a = s.RequestRun (1, 1, 0);
    EXPECT_TRUE (a.launch);
    proto::HelloPayload hello;
    hello.reason = GpuReason::DriverIncompatible;
    a = s.HelloReceived (hello, 10);
    EXPECT_TRUE (a.sendHelloAck && !a.ack.refusal.empty () && a.kill && a.fallback);
    EXPECT_EQ (a.failedRun, 1u);
    EXPECT_EQ (s.State (), WorkerState::Disabled);
    a = s.RequestRun (2, 1, 20);
    EXPECT_TRUE (a.fallback && !a.launch);
    EXPECT_EQ (a.reason, GpuReason::DriverIncompatible);
    EXPECT_NE (s.Describe ().find ("driver_incompatible"), std::string::npos);

    RayWorkerSupervisor missing;
    missing.RequestRun (1, 1, 0);
    a = missing.LaunchFailed (GpuReason::None, 0);
    EXPECT_EQ (a.reason, GpuReason::WorkerMissing);
    EXPECT_EQ (missing.State (), WorkerState::Disabled);

    RayWorkerSupervisor noRt;
    noRt.RequestRun (1, 1, 0);
    a = noRt.HelloReceived (proto::HelloPayload (), 0);
    EXPECT_EQ (a.reason, GpuReason::GpuUnsupported);
}

TEST (RayWorkerSupervisor, ACrashFailsTheRunRestartsLaterAndASecondCrashDisables)
{
    RayWorkerSupervisor s;
    proto::HelloPayload hello;
    hello.capabilities = proto::CapabilityHardwareRayTracing;
    s.RequestRun (1, 7, 0);
    auto a = s.HelloReceived (hello, 0);
    EXPECT_TRUE (a.uploadScene && !a.sendRun);
    a = s.SceneReady (7, 1);
    EXPECT_TRUE (a.sendRun);
    EXPECT_EQ (s.State (), WorkerState::Busy);

    a = s.ProcessExited (2);
    EXPECT_TRUE (a.fallback && a.kill);
    EXPECT_EQ (a.failedRun, 1u);
    EXPECT_EQ (a.reason, GpuReason::WorkerCrashed);
    EXPECT_EQ (s.State (), WorkerState::Stopped);
    EXPECT_EQ (s.ResidentScene (), 0u) << "a new worker holds no scene";

    a = s.RequestRun (2, 7, 3);
    EXPECT_TRUE (a.launch) << "the worker restarts on the next request";
    a = s.HelloReceived (hello, 4);
    EXPECT_TRUE (a.uploadScene) << "and rebuilds its scene";
    s.SceneReady (7, 5);
    // Silence counts as a crash.
    a = s.Tick (5 + kHeartbeatTimeoutMs + 1);
    EXPECT_EQ (a.reason, GpuReason::DisabledForSession);
    EXPECT_EQ (a.failedRun, 2u);
    EXPECT_EQ (s.State (), WorkerState::Disabled);
    EXPECT_EQ (s.Crashes (), kCrashesBeforeDisable);
}

TEST (RayWorkerSupervisor, StaleAndSupersededResultsAreNeverPublished)
{
    RayWorkerSupervisor s;
    proto::HelloPayload hello;
    hello.capabilities = proto::CapabilityHardwareRayTracing;
    s.RequestRun (1, 7, 0);
    s.HelloReceived (hello, 0);
    s.SceneReady (7, 0);
    // A newer run supersedes the one in flight.
    auto a = s.RequestRun (2, 7, 1);
    EXPECT_EQ (a.failedRun, 1u);
    EXPECT_FALSE (a.fallback) << "superseded, not failed over to the CPU";
    a = s.RunCompleted (1, 7, 2);
    EXPECT_FALSE (a.publish);
    EXPECT_TRUE (a.sendRun) << "the newer run goes as soon as the worker is free";
    a = s.RunCompleted (2, 8, 3);
    EXPECT_FALSE (a.publish) << "a result for another scene";
    s.RequestRun (3, 7, 4);
    a = s.RunCompleted (3, 7, 5);
    EXPECT_TRUE (a.publish);
    // Out of memory keeps the worker but drops its scene.
    s.RequestRun (4, 7, 6);
    a = s.RunFailed (4, GpuReason::OutOfMemory, 7);
    EXPECT_TRUE (a.fallback && !a.kill);
    EXPECT_EQ (s.State (), WorkerState::Ready);
    EXPECT_EQ (s.ResidentScene (), 0u);
    a = s.ProtocolRefused (8);
    EXPECT_EQ (a.reason, GpuReason::ProtocolMismatch);
}

// ---- end to end -------------------------------------------------------------------

TEST (RayWorkerLoopback, TheWorkerAnswersBitForBitWhatTheSunStudyComputes)
{
    Loopback loop;
    const auto snapshot = City (1, 0);
    const auto scene = std::make_shared<const packet::Scene> (packet::SceneFromSnapshot (*snapshot));
    const auto request = Day (12);
    auto outcomes = loop.Run (1, 1, scene, request);
    ASSERT_EQ (outcomes.size (), 1u);
    ASSERT_TRUE (outcomes[0].published) << outcomes[0].message;
    EXPECT_EQ (outcomes[0].result.bits, Oracle (snapshot, request));
    const auto shaded = std::count_if (outcomes[0].result.bits.begin (), outcomes[0].result.bits.end (),
                                       [] (uint64_t word) { return word != 0; });
    EXPECT_GT (shaded, 0) << "the city must shade some samples or the comparison is vacuous";
    EXPECT_LT (static_cast<size_t> (shaded), outcomes[0].result.bits.size ());
    EXPECT_EQ (std::count (outcomes[0].result.resolved.begin (), outcomes[0].result.resolved.end (), 1), 12);
    EXPECT_EQ (loop.uploads, 1);

    // The same scene is not uploaded again; a subset of steps traces only those.
    auto subset = request;
    subset.steps = { 2, 5, 9 };
    subset.directions = { request.directions.begin () + 6, request.directions.begin () + 9 };
    subset.directions.insert (subset.directions.end (), request.directions.begin () + 15,
                              request.directions.begin () + 18);
    subset.directions.insert (subset.directions.end (), request.directions.begin () + 27,
                              request.directions.begin () + 30);
    outcomes = loop.Run (2, 1, scene, subset);
    ASSERT_TRUE (outcomes.size () == 1 && outcomes[0].published);
    EXPECT_EQ (outcomes[0].result.bits, Oracle (snapshot, subset));
    EXPECT_EQ (loop.uploads, 1);

    // A new snapshot is a new epoch and a new upload.
    const auto moved = City (2, 1.5);
    outcomes = loop.Run (3, 2, std::make_shared<const packet::Scene> (packet::SceneFromSnapshot (*moved)), request);
    ASSERT_TRUE (outcomes.size () == 1 && outcomes[0].published);
    EXPECT_EQ (outcomes[0].result.bits, Oracle (moved, request));
    EXPECT_EQ (loop.uploads, 2);
    EXPECT_EQ (loop.launches, 1);
}

TEST (RayWorkerLoopback, ACrashedWorkerFailsItsRunAndTheNextRunRestartsIt)
{
    Loopback loop;
    const auto snapshot = City (1, 0);
    const auto scene = std::make_shared<const packet::Scene> (packet::SceneFromSnapshot (*snapshot));
    const auto request = Day (6);
    ASSERT_TRUE (loop.Run (1, 1, scene, request)[0].published);

    loop.deliver = false; // the worker hangs on the next run
    auto outcomes = loop.Run (2, 1, scene, request);
    EXPECT_TRUE (outcomes.empty ()) << "nothing is published while the run is in flight";
    loop.client->OnProcessExited (10);
    outcomes = loop.client->TakeOutcomes ();
    ASSERT_EQ (outcomes.size (), 1u);
    EXPECT_FALSE (outcomes[0].published);
    EXPECT_EQ (outcomes[0].reason, GpuReason::WorkerCrashed);

    loop.deliver = true;
    outcomes = loop.Run (3, 1, scene, request, 20);
    ASSERT_TRUE (outcomes.size () == 1 && outcomes[0].published) << outcomes[0].message;
    EXPECT_EQ (outcomes[0].result.bits, Oracle (snapshot, request));
    EXPECT_EQ (loop.launches, 2);
    EXPECT_EQ (loop.uploads, 2) << "the restarted worker rebuilt its scene";
}

TEST (RayWorkerLoopback, ACorruptOrStaleResultSegmentIsNeverPublished)
{
    Loopback loop;
    const auto scene = std::make_shared<const packet::Scene> (packet::SceneFromSnapshot (*City (1, 0)));
    const auto request = Day (4);
    loop.tamper = [] (MemorySegmentStore& store) {
        auto found = store.Segments ().find (ResultSegmentName (1));
        if (found != store.Segments ().end () && found->second.size () > packet::HeaderBytes + 20)
            found->second[packet::HeaderBytes + 20] ^= 0x40;
    };
    auto outcomes = loop.Run (1, 1, scene, request);
    ASSERT_EQ (outcomes.size (), 1u);
    EXPECT_FALSE (outcomes[0].published);
    EXPECT_NE (outcomes[0].message.find ("checksum"), std::string::npos) << outcomes[0].message;
    loop.tamper = nullptr;
    EXPECT_TRUE (loop.Run (2, 1, scene, request)[0].published) << "the worker stays usable";
}

TEST (RayWorkerLoopback, RefusalsReachTheCallerAsReasonCodes)
{
    {
        Loopback loop;
        loop.startable = false;
        const auto outcomes = loop.Run (1, 1, nullptr, Day (2));
        ASSERT_EQ (outcomes.size (), 1u);
        EXPECT_EQ (outcomes[0].reason, GpuReason::WorkerMissing);
    }
    {
        // A worker that reports an old driver is told to exit; the run falls back.
        struct OldDriver final : RayBackend {
            proto::HelloPayload Describe () const override
            {
                proto::HelloPayload hello;
                hello.reason = GpuReason::DriverIncompatible;
                return hello;
            }
            GpuReason BuildScene (const packet::Scene&, proto::SceneReadyPayload&, std::string&) override
            {
                return GpuReason::WorkerFailed;
            }
            GpuReason Trace (const packet::Request&, packet::Result&, proto::RunTimings&, const std::function<bool ()>&,
                             std::string&) override
            {
                return GpuReason::WorkerFailed;
            }
        };
        Loopback loop;
        loop.backend = [] { return std::make_unique<OldDriver> (); };
        const auto outcomes =
            loop.Run (1, 1, std::make_shared<const packet::Scene> (packet::SceneFromSnapshot (*City (1, 0))), Day (2));
        ASSERT_EQ (outcomes.size (), 1u);
        EXPECT_EQ (outcomes[0].reason, GpuReason::DriverIncompatible);
        EXPECT_EQ (loop.kills, 1);
        EXPECT_EQ (loop.client->Supervisor ().State (), WorkerState::Disabled);
    }
}
