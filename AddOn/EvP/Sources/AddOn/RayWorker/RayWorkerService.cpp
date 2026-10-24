#include "RayWorker/RayWorkerService.hpp"

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

bool MemorySegmentStore::Put (const std::string& name, std::vector<uint8_t> bytes)
{
    segments_[name] = std::move (bytes);
    return true;
}

bool MemorySegmentStore::Get (const std::string& name, std::vector<uint8_t>& bytes) const
{
    const auto found = segments_.find (name);
    if (found == segments_.end ())
        return false;
    bytes = found->second;
    return true;
}

std::unique_ptr<RayBackend> MakeCpuReferenceBackend ()
{
    return std::make_unique<CpuReferenceBackend> ();
}

RayWorkerService::RayWorkerService (std::unique_ptr<RayBackend> backend, SegmentStore& segments)
    : backend_ (std::move (backend)), segments_ (segments)
{
}

std::vector<uint8_t> RayWorkerService::Reply (protocol::MessageType type, uint32_t correlation,
                                              const std::vector<uint8_t>& payload)
{
    return protocol::EncodeFrame (type, nextRequestId_++, correlation, payload);
}

std::vector<uint8_t> RayWorkerService::Fail (uint32_t correlation, GpuReason reason, const std::string& message)
{
    protocol::FailedPayload failed;
    failed.reason = reason;
    failed.message = message.substr (0, protocol::MaxTextBytes);
    return Reply (protocol::MessageType::Failed, correlation, protocol::Encode (failed));
}

std::vector<uint8_t> RayWorkerService::Hello ()
{
    return Reply (protocol::MessageType::Hello, 0, protocol::Encode (backend_->Describe ()));
}

std::vector<std::vector<uint8_t>> RayWorkerService::OnFrame (const std::vector<uint8_t>& frame)
{
    std::vector<std::vector<uint8_t>> out;
    protocol::Header header;
    std::string error;
    if (!protocol::DecodeHeader (frame.data (), frame.size (), header, error) ||
        frame.size () != protocol::HeaderSize + header.payloadBytes) {
        // A frame this side cannot read is answered with nothing it could
        // misread back; the host's own decoder reports its side.
        out.push_back (Fail (0, GpuReason::ProtocolMismatch, error.empty () ? "the frame length is wrong" : error));
        return out;
    }
    const uint8_t* payload = frame.data () + protocol::HeaderSize;
    const size_t size = header.payloadBytes;
    const uint32_t id = header.requestId;

    switch (header.messageType) {
        case protocol::MessageType::HelloAck: {
            protocol::HelloAckPayload ack;
            if (protocol::Decode (payload, size, ack, error))
                accepted_ = ack.refusal.empty ();
            if (!accepted_)
                shutdown_ = true;
            return out;
        }
        case protocol::MessageType::UploadScene: {
            protocol::UploadScenePayload upload;
            std::vector<uint8_t> bytes;
            packet::Scene scene;
            if (!protocol::Decode (payload, size, upload, error))
                out.push_back (Fail (id, GpuReason::ProtocolMismatch, error));
            else if (!segments_.Get (upload.scene.name, bytes))
                out.push_back (Fail (id, GpuReason::WorkerFailed, "the scene segment does not exist"));
            else if (!packet::Read (bytes.data (), bytes.size (), upload.sceneEpoch, upload.scene.checksum, scene,
                                    error))
                out.push_back (Fail (id, GpuReason::WorkerFailed, error));
            else {
                protocol::SceneReadyPayload ready;
                ready.sceneEpoch = upload.sceneEpoch;
                const GpuReason reason = backend_->BuildScene (scene, ready, error);
                if (reason != GpuReason::None) {
                    residentScene_ = 0;
                    out.push_back (Fail (id, reason, error));
                }
                else {
                    residentScene_ = upload.sceneEpoch;
                    out.push_back (Reply (protocol::MessageType::SceneReady, id, protocol::Encode (ready)));
                }
            }
            return out;
        }
        case protocol::MessageType::RunStudy: {
            protocol::RunStudyPayload run;
            std::vector<uint8_t> bytes;
            packet::Request request;
            if (!protocol::Decode (payload, size, run, error))
                out.push_back (Fail (id, GpuReason::ProtocolMismatch, error));
            else if (run.sceneEpoch != residentScene_ || residentScene_ == 0)
                out.push_back (Fail (id, GpuReason::WorkerFailed, "the run names a scene that is not resident"));
            else if (!segments_.Get (run.request.name, bytes))
                out.push_back (Fail (id, GpuReason::WorkerFailed, "the request segment does not exist"));
            else if (!packet::Read (bytes.data (), bytes.size (), run.sceneEpoch, run.runGeneration,
                                    run.request.checksum, request, error))
                out.push_back (Fail (id, GpuReason::WorkerFailed, error));
            else {
                const auto started = std::chrono::steady_clock::now ();
                packet::Result result;
                protocol::RunResultPayload done;
                const uint64_t generation = run.runGeneration;
                const GpuReason reason = backend_->Trace (
                    request, result, done.timings, [this, generation] { return cancelledRun_ == generation; }, error);
                if (reason != GpuReason::None) {
                    out.push_back (Fail (id, reason, error));
                    return out;
                }
                std::vector<uint8_t> written = packet::Write (run.sceneEpoch, generation, result);
                if (written.size () > run.result.bytes) {
                    out.push_back (Fail (id, GpuReason::WorkerFailed, "the result is larger than its segment"));
                    return out;
                }
                done.runGeneration = generation;
                done.sceneEpoch = run.sceneEpoch;
                done.resultBytes = written.size ();
                done.checksum = packet::StoredChecksum (written);
                segments_.Put (run.result.name, std::move (written));
                done.timings.readback = Microseconds (started) - done.timings.trace;
                out.push_back (Reply (protocol::MessageType::RunResult, id, protocol::Encode (done)));
            }
            return out;
        }
        case protocol::MessageType::Cancel: {
            protocol::CancelPayload cancel;
            if (protocol::Decode (payload, size, cancel, error))
                cancelledRun_ = cancel.runGeneration;
            return out;
        }
        case protocol::MessageType::Shutdown:
            shutdown_ = true;
            return out;
        default:
            out.push_back (Fail (id, GpuReason::ProtocolMismatch,
                                 std::string ("the worker does not accept ") +
                                     protocol::DescribeMessageType (header.messageType)));
            return out;
    }
}

} // namespace evp::rayworker
