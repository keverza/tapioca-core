#include "RayWorker/RayWorkerService.hpp"

// The CPU reference backend lives apart from the service so a worker
// executable links the wire without the geometry engine; tests and
// diagnostics link both.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace evp::rayworker {

namespace {

uint32_t Microseconds (std::chrono::steady_clock::time_point since)
{
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - since);
    return static_cast<uint32_t> (std::min<long long> (elapsed.count (), UINT32_MAX));
}

// One snapshot mesh per scene mesh, with only the vertices its triangles use,
// in first-use order: the triangles (and so the index built over them) are the
// snapshot's own, in its own order.
std::shared_ptr<geomsrv::Snapshot> SnapshotOf (const packet::Scene& scene, uint64_t epoch)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = epoch;
    snapshot->meshes.resize (scene.meshCount);
    std::vector<std::unordered_map<uint32_t, uint32_t>> remap (scene.meshCount);
    for (size_t t = 0; t < scene.triangleMesh.size (); ++t) {
        const uint32_t m = scene.triangleMesh[t];
        geomsrv::Mesh& mesh = snapshot->meshes[m];
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t global = scene.triangles[t * 3 + corner];
            const auto [it, added] = remap[m].emplace (global, static_cast<uint32_t> (mesh.vertices.size () / 3));
            if (added)
                mesh.vertices.insert (mesh.vertices.end (), scene.vertices.begin () + global * 3,
                                      scene.vertices.begin () + global * 3 + 3);
            mesh.triangles.push_back (it->second);
        }
        mesh.triMaterial.push_back (0);
    }
    return snapshot;
}

class CpuReferenceBackend final : public RayBackend {
  public:
    protocol::HelloPayload Describe () const override
    {
        protocol::HelloPayload hello;
        // A test double: it claims the capability so the supervisor accepts it.
        hello.capabilities = protocol::CapabilityHardwareRayTracing | protocol::CapabilityDoublePrecision;
        hello.deviceName = "cpu-reference";
        return hello;
    }

    GpuReason BuildScene (const packet::Scene& scene, protocol::SceneReadyPayload& ready, std::string& error) override
    {
        const auto started = std::chrono::steady_clock::now ();
        auto engine = std::make_shared<geomsrv::QueryEngine> (SnapshotOf (scene, ready.sceneEpoch));
        traversal_ = std::make_shared<evp::sunstudy::CpuTraversal> (engine);
        (void) error;
        ready.buildMicroseconds = Microseconds (started);
        ready.deviceBytes = scene.vertices.size () * sizeof (double) + scene.triangles.size () * sizeof (uint32_t);
        return GpuReason::None;
    }

    GpuReason Trace (const packet::Request& request, packet::Result& result, protocol::RunTimings& timings,
                     const std::function<bool ()>& isCancelled, std::string& error) override
    {
        if (traversal_ == nullptr) {
            error = "no scene is resident";
            return GpuReason::WorkerFailed;
        }
        const auto started = std::chrono::steady_clock::now ();
        const size_t samples = request.positions.size () / 3;
        evp::sunstudy::OcclusionAccumulator accumulator (samples, static_cast<size_t> (request.totalSteps));
        evp::sunstudy::SampleSet set;
        set.positions = request.positions.data ();
        set.normals = request.normals.empty () ? nullptr : request.normals.data ();
        set.count = samples;
        for (size_t i = 0; i < request.steps.size (); ++i) {
            if (!accumulator.AccumulateStep (*traversal_, set, request.steps[i], &request.directions[i * 3],
                                             request.tmin, request.tmax, 0, isCancelled)) {
                error = "the run was cancelled";
                return GpuReason::WorkerFailed;
            }
        }
        timings.trace = Microseconds (started);
        result.sampleCount = samples;
        result.stepCount = request.totalSteps;
        result.bits = accumulator.Bits ();
        result.resolved.resize (static_cast<size_t> (request.totalSteps));
        for (size_t step = 0; step < result.resolved.size (); ++step)
            result.resolved[step] = accumulator.StepResolved (step) ? 1 : 0;
        return GpuReason::None;
    }

  private:
    std::shared_ptr<const evp::sunstudy::CpuTraversal> traversal_;
};

} // namespace

std::unique_ptr<RayBackend> MakeCpuReferenceBackend ()
{
    return std::make_unique<CpuReferenceBackend> ();
}

} // namespace evp::rayworker
