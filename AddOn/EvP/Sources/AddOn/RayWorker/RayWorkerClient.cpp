#include "RayWorker/RayWorkerClient.hpp"

namespace evp::rayworker {

std::string SceneSegmentName (uint64_t sceneEpoch)
{
    return "tapioca-rayworker-scene-" + std::to_string (sceneEpoch);
}

std::string RequestSegmentName (uint64_t generation)
{
    return "tapioca-rayworker-request-" + std::to_string (generation);
}

std::string ResultSegmentName (uint64_t generation)
{
    return "tapioca-rayworker-result-" + std::to_string (generation);
}

RayWorkerClient::RayWorkerClient (SegmentStore& segments, RayWorkerHooks hooks)
    : segments_ (segments), hooks_ (std::move (hooks))
{
}

uint32_t RayWorkerClient::Send (protocol::MessageType type, const std::vector<uint8_t>& payload)
{
    const uint32_t id = nextRequestId_++;
    if (hooks_.send)
        hooks_.send (protocol::EncodeFrame (type, id, 0, payload));
    return id;
}

void RayWorkerClient::Finish (uint64_t generation, GpuReason reason, const std::string& message, bool superseded)
{
    if (generation == 0)
        return;
    RayWorkerOutcome outcome;
    outcome.generation = generation;
    outcome.reason = reason;
    outcome.superseded = superseded;
    outcome.message = message.empty () ? DescribeGpuReason (reason) : message;
    outcomes_.push_back (std::move (outcome));
    if (generation == generation_) {
        generation_ = 0;
        scene_.reset ();
    }
}

void RayWorkerClient::Apply (const SupervisorAction& action, int64_t nowMs)
{
    if (action.kill && hooks_.kill)
        hooks_.kill ();
    if (action.failedRun != 0) {
        // A run failed without a fallback reason was superseded by a newer one.
        const bool superseded = !action.fallback;
        Finish (action.failedRun, superseded ? GpuReason::None : action.reason,
                superseded ? "superseded by a newer run" : std::string (), superseded);
    }
    else if (action.fallback && generation_ != 0) {
        Finish (generation_, action.reason, std::string ());
    }
    if (action.sendHelloAck)
        Send (protocol::MessageType::HelloAck, protocol::Encode (action.ack));
    if (action.uploadScene && scene_ != nullptr) {
        std::vector<uint8_t> packet = packet::Write (sceneEpoch_, *scene_);
        protocol::UploadScenePayload upload;
        upload.sceneEpoch = sceneEpoch_;
        upload.scene = { SceneSegmentName (sceneEpoch_), packet.size (), packet::StoredChecksum (packet) };
        segments_.Put (upload.scene.name, std::move (packet));
        sent_[Send (protocol::MessageType::UploadScene, protocol::Encode (upload))] = generation_;
    }
    if (action.sendRun) {
        std::vector<uint8_t> packet = packet::Write (sceneEpoch_, generation_, request_);
        protocol::RunStudyPayload run;
        run.runGeneration = generation_;
        run.sceneEpoch = sceneEpoch_;
        run.request = { RequestSegmentName (generation_), packet.size (), packet::StoredChecksum (packet) };
        // Header, two counts, the bitset and one flag per step.
        const uint64_t samples = request_.positions.size () / 3;
        const uint64_t words = (request_.totalSteps + 63) / 64;
        run.result = { ResultSegmentName (generation_),
                       packet::HeaderBytes + 16 + samples * words * 8 + request_.totalSteps, 0 };
        segments_.Put (run.request.name, std::move (packet));
        sent_[Send (protocol::MessageType::RunStudy, protocol::Encode (run))] = generation_;
    }
    if (action.launch) {
        if (!hooks_.launch || !hooks_.launch ())
            Apply (supervisor_.LaunchFailed (GpuReason::WorkerMissing, nowMs), nowMs);
    }
}

void RayWorkerClient::Submit (uint64_t generation, uint64_t sceneEpoch, std::shared_ptr<const packet::Scene> scene,
                              packet::Request request, int64_t nowMs)
{
    generation_ = generation;
    sceneEpoch_ = sceneEpoch;
    scene_ = std::move (scene);
    request_ = std::move (request);
    Apply (supervisor_.RequestRun (generation, sceneEpoch, nowMs), nowMs);
}

void RayWorkerClient::OnFrame (const std::vector<uint8_t>& frame, int64_t nowMs)
{
    protocol::Header header;
    std::string error;
    if (!protocol::DecodeHeader (frame.data (), frame.size (), header, error) ||
        frame.size () != protocol::HeaderSize + header.payloadBytes) {
        Apply (supervisor_.ProtocolRefused (nowMs), nowMs);
        return;
    }
    const uint8_t* payload = frame.data () + protocol::HeaderSize;
    const size_t size = header.payloadBytes;
    switch (header.messageType) {
        case protocol::MessageType::Hello: {
            protocol::HelloPayload hello;
            if (!protocol::Decode (payload, size, hello, error))
                Apply (supervisor_.ProtocolRefused (nowMs), nowMs);
            else
                Apply (supervisor_.HelloReceived (hello, nowMs), nowMs);
            return;
        }
        case protocol::MessageType::Heartbeat:
            supervisor_.Heartbeat (nowMs);
            return;
        case protocol::MessageType::SceneReady: {
            protocol::SceneReadyPayload ready;
            sent_.erase (header.correlationId);
            if (protocol::Decode (payload, size, ready, error))
                Apply (supervisor_.SceneReady (ready.sceneEpoch, nowMs), nowMs);
            return;
        }
        case protocol::MessageType::RunResult: {
            protocol::RunResultPayload done;
            sent_.erase (header.correlationId);
            if (!protocol::Decode (payload, size, done, error)) {
                Apply (supervisor_.ProtocolRefused (nowMs), nowMs);
                return;
            }
            const SupervisorAction action = supervisor_.RunCompleted (done.runGeneration, done.sceneEpoch, nowMs);
            if (action.publish) {
                std::vector<uint8_t> bytes;
                RayWorkerOutcome outcome;
                outcome.generation = done.runGeneration;
                outcome.timings = done.timings;
                if (!segments_.Get (ResultSegmentName (done.runGeneration), bytes))
                    error = "the result segment does not exist";
                else if (bytes.size () != done.resultBytes)
                    error = "the result segment holds " + std::to_string (bytes.size ()) + " bytes, the worker said " +
                            std::to_string (done.resultBytes);
                else if (packet::Read (bytes.data (), bytes.size (), done.sceneEpoch, done.runGeneration, done.checksum,
                                       outcome.result, error) &&
                         outcome.result.sampleCount == request_.positions.size () / 3 &&
                         outcome.result.stepCount == request_.totalSteps)
                    outcome.published = true;
                else if (error.empty ())
                    error = "the result does not describe the request";
                if (!outcome.published) {
                    outcome.reason = GpuReason::WorkerFailed;
                    outcome.message = error;
                }
                outcomes_.push_back (std::move (outcome));
                generation_ = 0;
                scene_.reset ();
            }
            Apply (action, nowMs);
            return;
        }
        case protocol::MessageType::Failed: {
            protocol::FailedPayload failed;
            const auto found = sent_.find (header.correlationId);
            const uint64_t generation = found != sent_.end () ? found->second : 0;
            if (found != sent_.end ())
                sent_.erase (found);
            if (!protocol::Decode (payload, size, failed, error) || generation == 0) {
                Apply (supervisor_.ProtocolRefused (nowMs), nowMs);
                return;
            }
            const SupervisorAction action = supervisor_.RunFailed (generation, failed.reason, nowMs);
            if (action.failedRun != 0 && action.fallback) {
                Finish (action.failedRun, failed.reason, failed.message);
                SupervisorAction rest = action;
                rest.failedRun = 0;
                rest.fallback = false;
                Apply (rest, nowMs);
            }
            else
                Apply (action, nowMs);
            return;
        }
        case protocol::MessageType::Log:
            supervisor_.Heartbeat (nowMs);
            return;
        default:
            Apply (supervisor_.ProtocolRefused (nowMs), nowMs);
            return;
    }
}

void RayWorkerClient::OnProcessExited (int64_t nowMs)
{
    sent_.clear ();
    Apply (supervisor_.ProcessExited (nowMs), nowMs);
}

void RayWorkerClient::Tick (int64_t nowMs)
{
    const SupervisorAction action = supervisor_.Tick (nowMs);
    if (action.kill)
        sent_.clear ();
    Apply (action, nowMs);
}

void RayWorkerClient::Shutdown ()
{
    sent_.clear ();
    if (supervisor_.State () == WorkerState::Ready || supervisor_.State () == WorkerState::Busy)
        Send (protocol::MessageType::Shutdown, {});
    SupervisorAction action = supervisor_.Shutdown ();
    Finish (action.failedRun, GpuReason::WorkerFailed, "the GPU worker was shut down");
    action.failedRun = 0;
    Apply (action, 0);
}

std::vector<RayWorkerOutcome> RayWorkerClient::TakeOutcomes ()
{
    std::vector<RayWorkerOutcome> out;
    out.swap (outcomes_);
    return out;
}

} // namespace evp::rayworker
