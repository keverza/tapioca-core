// Opt-in GPU ray worker measurement (raycast E5 / G5, #43). No production
// caller and no timing assertions.
//
// Each worker executable built beside this one (TapiocaDxrWorker.exe,
// TapiocaOptixWorker.exe) is driven exactly as the add-on drives it: the real
// pipe, the real file mappings, RayWorkerClient and its supervisor. For every
// scene size a fresh worker measures
//
//   cold     Submit -> first published outcome: launch, handshake, scene
//            write + upload, build, first run;
//   upload   the host writing the scene packet into its segment;
//   build    the worker's own build time (SceneReady);
//   warm     wall time per run on the resident scene, p50/p95, split into the
//            host writing the request, the worker's transfer, trace and
//            readback, and the host reading and checking the result;
//
// and the CPU reference backend (the sun study's accumulator over the C++
// index, all threads) gives the baseline build and trace on the same request.
// Every published result is compared with the CPU's bits; a mismatch is
// printed, never averaged away.
//
// ⚠️ SYNTHETIC GEOMETRY MEASURES, IT DOES NOT VALIDATE. A block city is not a
// recorded model; the E5 gate is the complete sun study on the recorded
// workloads. This isolates the worker's share of it.
//
//   TapiocaRayWorkerBench [--sizes 10000,200000,2000000] [--samples 40000]
//                         [--steps 1,24] [--runs 15] [--workers dxr,optix]

#include "RayWorker/RayWorkerClient.hpp"
#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerService.hpp"
#include "RayWorker/RayWorkerWin32.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace evp::rayworker;
namespace win32 = evp::rayworker::win32;

using Clock = std::chrono::steady_clock;

double MsSince (Clock::time_point since)
{
    return std::chrono::duration<double, std::milli> (Clock::now () - since).count ();
}

int64_t NowMs ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (Clock::now ().time_since_epoch ()).count ();
}

std::vector<size_t> ParseList (const char* text)
{
    std::vector<size_t> values;
    std::stringstream stream (text);
    std::string item;
    while (std::getline (stream, item, ','))
        values.push_back (size_t (std::strtoull (item.c_str (), nullptr, 10)));
    return values;
}

std::vector<std::string> ParseNames (const char* text)
{
    std::vector<std::string> values;
    std::stringstream stream (text);
    std::string item;
    while (std::getline (stream, item, ','))
        values.push_back (item);
    return values;
}

// Deterministic, so two runs build the same city.
struct Lcg {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    double Next ()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return double (state >> 11) * (1.0 / 9007199254740992.0);
    }
};

// A square grid of closed boxes, 12 triangles each, on a two-triangle ground:
// 1 m cells, 0.6 m footprints, 1-10 m heights. One mesh per box, as the
// snapshot's elements would be.
struct City {
    std::shared_ptr<packet::Scene> scene;
    double extent = 0;
};

City MakeCity (size_t triangles)
{
    City city;
    city.scene = std::make_shared<packet::Scene> ();
    packet::Scene& s = *city.scene;
    const size_t boxes = std::max<size_t> (1, (triangles - std::min<size_t> (triangles, 2)) / 12);
    const size_t side = size_t (std::ceil (std::sqrt (double (boxes))));
    city.extent = double (side);
    const double pad = 2.0;
    s.vertices = {
        -pad, -pad, 0, city.extent + pad, -pad, 0, city.extent + pad, city.extent + pad, 0, -pad, city.extent + pad, 0
    };
    s.triangles = { 0, 1, 2, 0, 2, 3 };
    s.triangleMesh = { 0, 0 };
    static const uint32_t faces[12][3] = {
        { 0, 2, 3 }, { 0, 3, 1 }, { 4, 5, 7 }, { 4, 7, 6 }, { 0, 1, 5 }, { 0, 5, 4 },
        { 2, 6, 7 }, { 2, 7, 3 }, { 0, 4, 6 }, { 0, 6, 2 }, { 1, 3, 7 }, { 1, 7, 5 }
    };
    Lcg random;
    uint32_t mesh = 1;
    for (size_t b = 0; b < boxes; ++b, ++mesh) {
        const double x0 = double (b % side) + 0.2, y0 = double (b / side) + 0.2;
        const double h = 1.0 + 9.0 * random.Next ();
        const uint32_t base = uint32_t (s.vertices.size () / 3);
        for (int i = 0; i < 8; ++i)
            s.vertices.insert (s.vertices.end (),
                               { (i & 1) ? x0 + 0.6 : x0, (i & 2) ? y0 + 0.6 : y0, (i & 4) ? h : 0.0 });
        for (const auto& f : faces) {
            for (const uint32_t corner : f)
                s.triangles.push_back (base + corner);
            s.triangleMesh.push_back (mesh);
        }
    }
    s.meshCount = mesh;
    return city;
}

// Ground samples in the streets between the blocks, facing up, and a day of
// sun directions; the step count is the request's own.
packet::Request MakeRequest (const City& city, size_t samples, size_t steps)
{
    packet::Request request;
    const size_t side = std::max<size_t> (1, size_t (std::ceil (std::sqrt (double (samples)))));
    const double pitch = city.extent / double (side);
    for (size_t i = 0; i < samples; ++i) {
        const double x = (double (i % side) + 0.5) * pitch, y = (double (i / side) + 0.5) * pitch;
        request.positions.insert (request.positions.end (), { x, y, 0.05 });
        request.normals.insert (request.normals.end (), { 0.0, 0.0, 1.0 });
    }
    request.totalSteps = steps;
    for (uint32_t k = 0; k < steps; ++k) {
        const double t = steps == 1 ? 0.5 : double (k) / double (steps - 1);
        const double azimuth = 1.2 + 3.6 * t, elevation = 0.15 + 0.9 * std::sin (3.14159265358979 * t);
        request.steps.push_back (k);
        request.directions.insert (request.directions.end (),
                                   { std::cos (elevation) * std::cos (azimuth),
                                     std::cos (elevation) * std::sin (azimuth), std::sin (elevation) });
    }
    return request;
}

double Percentile (std::vector<double> values, double p)
{
    if (values.empty ())
        return 0;
    std::sort (values.begin (), values.end ());
    const size_t index = std::min (values.size () - 1, size_t (std::ceil (p * double (values.size ()))) - 1);
    return values[index];
}

std::wstring BesideThisExe (const std::string& name)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW (nullptr, path, MAX_PATH);
    return (std::filesystem::path (path).parent_path () / std::filesystem::path (name)).wstring ();
}

struct CpuBaseline {
    double buildMs = 0;
    double traceP50 = 0;
    packet::Result result;
};

CpuBaseline MeasureCpu (const packet::Scene& scene, const packet::Request& request, size_t runs)
{
    CpuBaseline out;
    auto cpu = MakeCpuReferenceBackend ();
    protocol::SceneReadyPayload ready;
    std::string error;
    const auto built = Clock::now ();
    cpu->BuildScene (scene, ready, error);
    out.buildMs = MsSince (built);
    std::vector<double> traces;
    for (size_t r = 0; r < std::max<size_t> (1, runs / 3); ++r) {
        protocol::RunTimings timings;
        packet::Result result;
        const auto started = Clock::now ();
        cpu->Trace (request, result, timings, {}, error);
        traces.push_back (MsSince (started));
        out.result = std::move (result);
    }
    out.traceP50 = Percentile (traces, 0.5);
    return out;
}

// The client conversation, with the frames the bench wants to time looked at
// on their way in.
struct Session {
    win32::WorkerProcess host;
    RayWorkerClient client;
    uint64_t generation = 0;
    double uploadMs = 0;  // OnFrame(Hello): the scene packet written and put
    uint32_t buildUs = 0; // SceneReady
    double readMs = 0;    // OnFrame(RunResult): read, checksum, publish
    protocol::HelloPayload hello;

    explicit Session (std::wstring exe) : host (std::move (exe)), client (host.Segments (), host.Hooks ())
    {
    }

    bool Run (const std::shared_ptr<const packet::Scene>& scene, const packet::Request& request,
              RayWorkerOutcome& outcome, double& submitMs)
    {
        const auto submitted = Clock::now ();
        client.Submit (++generation, 1, scene, request, NowMs ());
        submitMs = MsSince (submitted);
        const int64_t deadline = NowMs () + 180000;
        while (NowMs () < deadline) {
            auto events = host.Take ();
            for (const auto& frame : events.frames) {
                protocol::Header header;
                std::string error;
                const bool known = protocol::DecodeHeader (frame.data (), frame.size (), header, error);
                const uint8_t* payload = frame.data () + protocol::HeaderSize;
                if (known && header.messageType == protocol::MessageType::Hello)
                    protocol::Decode (payload, header.payloadBytes, hello, error);
                if (known && header.messageType == protocol::MessageType::SceneReady) {
                    protocol::SceneReadyPayload ready;
                    if (protocol::Decode (payload, header.payloadBytes, ready, error))
                        buildUs = ready.buildMicroseconds;
                }
                const auto handled = Clock::now ();
                client.OnFrame (frame, NowMs ());
                if (known && header.messageType == protocol::MessageType::Hello)
                    uploadMs = MsSince (handled);
                if (known && header.messageType == protocol::MessageType::RunResult)
                    readMs = MsSince (handled);
            }
            if (events.exited)
                client.OnProcessExited (NowMs ());
            client.Tick (NowMs ());
            for (auto& taken : client.TakeOutcomes ())
                if (taken.generation == generation) {
                    outcome = std::move (taken);
                    return true;
                }
            host.Wait (5); // as RayWorkerChannel::Run waits
        }
        return false;
    }
};

size_t DifferingSamples (const packet::Result& a, const packet::Result& b)
{
    if (a.bits.size () != b.bits.size ())
        return size_t (-1);
    const size_t words = a.sampleCount == 0 ? 0 : a.bits.size () / size_t (a.sampleCount);
    size_t differing = 0;
    for (size_t s = 0; s < a.sampleCount; ++s)
        if (!std::equal (a.bits.begin () + s * words, a.bits.begin () + (s + 1) * words, b.bits.begin () + s * words))
            ++differing;
    return differing;
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<size_t> sizes = { 10000, 200000, 2000000 };
    std::vector<size_t> stepCounts = { 1, 24 };
    std::vector<std::string> workers = { "dxr", "optix" };
    size_t samples = 40000, runs = 15;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp (argv[i], "--sizes") == 0)
            sizes = ParseList (argv[i + 1]);
        else if (std::strcmp (argv[i], "--steps") == 0)
            stepCounts = ParseList (argv[i + 1]);
        else if (std::strcmp (argv[i], "--samples") == 0)
            samples = size_t (std::strtoull (argv[i + 1], nullptr, 10));
        else if (std::strcmp (argv[i], "--runs") == 0)
            runs = std::max<size_t> (1, size_t (std::strtoull (argv[i + 1], nullptr, 10)));
        else if (std::strcmp (argv[i], "--workers") == 0)
            workers = ParseNames (argv[i + 1]);
        else {
            std::fprintf (stderr, "unknown argument %s\n", argv[i]);
            return 64;
        }
    }
    std::printf ("config=%s threads=%u samples=%zu runs=%zu\n", TAPIOCA_BENCH_CONFIG,
                 std::thread::hardware_concurrency (), samples, runs);
    std::printf ("%-6s %9s %5s %9s %8s %8s %8s | %8s %8s %8s %8s %8s %8s | %8s %8s %8s | %s\n", "worker", "triangles",
                 "steps", "rays", "cold", "upload", "build", "warmP50", "warmP95", "write", "transfer", "trace", "read",
                 "cpuBuild", "cpuTrace", "speedup", "bits");

    for (const size_t size : sizes) {
        const City city = MakeCity (size);
        const std::shared_ptr<const packet::Scene> scene = city.scene;
        const size_t triangles = scene->triangleMesh.size ();
        std::vector<packet::Request> requests;
        std::vector<CpuBaseline> baselines;
        for (const size_t steps : stepCounts) {
            requests.push_back (MakeRequest (city, samples, steps));
            baselines.push_back (MeasureCpu (*scene, requests.back (), runs));
        }
        for (const std::string& name : workers) {
            const std::wstring exe =
                BesideThisExe (name == "optix" ? "TapiocaOptixWorker.exe" : "TapiocaDxrWorker.exe");
            if (GetFileAttributesW (exe.c_str ()) == INVALID_FILE_ATTRIBUTES) {
                std::printf ("%-6s not built beside the bench\n", name.c_str ());
                continue;
            }
            Session session (exe);
            for (size_t k = 0; k < requests.size (); ++k) {
                const packet::Request& request = requests[k];
                const CpuBaseline& cpu = baselines[k];
                const size_t rays = samples * request.steps.size ();
                // The first run of a size is the cold one: a fresh worker and scene.
                RayWorkerOutcome outcome;
                double submitMs = 0;
                const auto started = Clock::now ();
                if (!session.Run (scene, request, outcome, submitMs) || !outcome.published) {
                    std::printf ("%-6s %9zu %5zu refused: %s %s\n", name.c_str (), triangles, request.steps.size (),
                                 GpuReasonCode (outcome.reason), outcome.message.c_str ());
                    break;
                }
                const double coldMs = k == 0 ? MsSince (started) : 0;
                std::vector<double> wall, write, transfer, trace, read;
                size_t differing = DifferingSamples (outcome.result, cpu.result);
                for (size_t r = 0; r < runs; ++r) {
                    const auto run = Clock::now ();
                    if (!session.Run (scene, request, outcome, submitMs) || !outcome.published) {
                        std::printf ("%-6s run failed: %s %s\n", name.c_str (), GpuReasonCode (outcome.reason),
                                     outcome.message.c_str ());
                        break;
                    }
                    wall.push_back (MsSince (run));
                    write.push_back (submitMs);
                    transfer.push_back (outcome.timings.transfer / 1000.0);
                    trace.push_back (outcome.timings.trace / 1000.0);
                    read.push_back (session.readMs);
                    differing = std::max (differing, DifferingSamples (outcome.result, cpu.result));
                }
                const double warm = Percentile (wall, 0.5);
                std::printf (
                    "%-6s %9zu %5zu %9zu %8.1f %8.1f %8.1f | %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f | %8.1f %8.2f "
                    "%7.1fx | %s\n",
                    name.c_str (), triangles, request.steps.size (), rays, coldMs, k == 0 ? session.uploadMs : 0.0,
                    k == 0 ? session.buildUs / 1000.0 : 0.0, warm, Percentile (wall, 0.95), Percentile (write, 0.5),
                    Percentile (transfer, 0.5), Percentile (trace, 0.5), Percentile (read, 0.5), cpu.buildMs,
                    cpu.traceP50, warm > 0 ? cpu.traceP50 / warm : 0.0,
                    differing == 0 ? "equal" : ("DIFFER samples=" + std::to_string (differing)).c_str ());
                std::fflush (stdout);
            }
            if (session.hello.deviceName.size () != 0 && size == sizes.front ())
                std::printf ("%-6s device=\"%s\" driver=%u runtime=%u compute=%u memoryBytes=%llu\n", name.c_str (),
                             session.hello.deviceName.c_str (), session.hello.driverVersion,
                             session.hello.runtimeVersion, session.hello.computeCapability,
                             static_cast<unsigned long long> (session.hello.deviceMemoryBytes));
            session.client.Shutdown ();
        }
    }
    return 0;
}
