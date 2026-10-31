#include "RayWorker/RayWorkerService.hpp"

#include <algorithm>
#include <chrono>

namespace evp::rayworker {

namespace {

uint32_t Microseconds (std::chrono::steady_clock::time_point since)
{
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - since);
    return static_cast<uint32_t> (std::min<long long> (elapsed.count (), UINT32_MAX));
}

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

bool RayWorkerService::NoteCancel (const std::vector<uint8_t>& frame)
{
    protocol::Header header;
    protocol::CancelPayload cancel;
    std::string error;
    if (!protocol::DecodeHeader (frame.data (), frame.size (), header, error) ||
        header.messageType != protocol::MessageType::Cancel ||
        frame.size () != protocol::HeaderSize + header.payloadBytes ||
        !protocol::Decode (frame.data () + protocol::HeaderSize, header.payloadBytes, cancel, error))
        return false;
    cancelledRun_.store (cancel.runGeneration);
    return true;
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
            // `transfer` is the request's way in: the segment read and every packet check.
            const auto received = std::chrono::steady_clock::now ();
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
                const uint32_t transfer = static_cast<uint32_t> (
                    std::chrono::duration_cast<std::chrono::microseconds> (started - received).count ());
                const uint64_t generation = run.runGeneration;
                const GpuReason reason = backend_->Trace (
                    request, result, done.timings, [this, generation] { return cancelledRun_.load () == generation; },
                    error);
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
                done.timings.transfer = transfer;
                out.push_back (Reply (protocol::MessageType::RunResult, id, protocol::Encode (done)));
            }
            return out;
        }
        case protocol::MessageType::Cancel:
            NoteCancel (frame);
            return out;
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
