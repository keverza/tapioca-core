#ifndef EVP_RAYWORKER_RAYWORKERCLIENT_HPP
#define EVP_RAYWORKER_RAYWORKERCLIENT_HPP

// RayWorker/RayWorkerClient -- the add-on's side of the GPU worker without the
// operating system: it turns the supervisor's decisions into frames and
// segments, and the worker's frames into supervisor events and outcomes.
//
// Process launch/kill and the pipe are hooks the platform host supplies, so
// the whole conversation -- handshake, scene upload, runs, supersession,
// crashes, refusals, stale or corrupt segments -- is tested offline against
// RayWorkerService. One thread at a time.
//
// ⚠️ AN OUTCOME IS PUBLISHED ONLY AFTER THE SUPERVISOR ACCEPTED THE RUN AND
// THE RESULT SEGMENT PASSED EVERY PACKET CHECK. Everything else is reported
// failed with a reason, and its caller runs the study on the CPU.

#include "RayWorker/RayWorkerPacket.hpp"
#include "RayWorker/RayWorkerProtocol.hpp"
#include "RayWorker/RayWorkerService.hpp"
#include "RayWorker/RayWorkerSupervisor.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace evp::rayworker {

struct RayWorkerHooks {
    // Start the worker process; false when it could not be started.
    std::function<bool ()> launch;
    // Terminate and join it (idempotent).
    std::function<void ()> kill;
    // Write one frame to the worker.
    std::function<void (std::vector<uint8_t>)> send;
};

struct RayWorkerOutcome {
    uint64_t generation = 0;
    bool published = false;  // `result` is valid and may be shown
    bool superseded = false; // replaced by a newer run; nothing to fall back
    packet::Result result;
    GpuReason reason = GpuReason::None; // why it was not published
    std::string message;
    protocol::RunTimings timings;
};

class RayWorkerClient final {
  public:
    RayWorkerClient (SegmentStore& segments, RayWorkerHooks hooks);

    // Ask for run `generation` of `request` against scene `sceneEpoch`.
    // `scene` must stay valid until the outcome; it is uploaded only when the
    // worker does not already hold that epoch.
    void Submit (uint64_t generation, uint64_t sceneEpoch, std::shared_ptr<const packet::Scene> scene,
                 packet::Request request, int64_t nowMs);
    void OnFrame (const std::vector<uint8_t>& frame, int64_t nowMs);
    void OnProcessExited (int64_t nowMs);
    void Tick (int64_t nowMs);
    void Shutdown ();

    std::vector<RayWorkerOutcome> TakeOutcomes ();
    const RayWorkerSupervisor& Supervisor () const
    {
        return supervisor_;
    }

  private:
    void Apply (const SupervisorAction& action, int64_t nowMs);
    void Finish (uint64_t generation, GpuReason reason, const std::string& message, bool superseded = false);
    uint32_t Send (protocol::MessageType type, const std::vector<uint8_t>& payload);

    SegmentStore& segments_;
    RayWorkerHooks hooks_;
    RayWorkerSupervisor supervisor_;
    uint32_t nextRequestId_ = 1;
    // The run waiting or in flight.
    uint64_t generation_ = 0;
    uint64_t sceneEpoch_ = 0;
    std::shared_ptr<const packet::Scene> scene_;
    packet::Request request_;
    // requestId of a sent UploadScene/RunStudy -> the run it serves.
    std::map<uint32_t, uint64_t> sent_;
    std::vector<RayWorkerOutcome> outcomes_;
};

// Segment names, stable so the worker and diagnostics can find them.
std::string SceneSegmentName (uint64_t sceneEpoch);
std::string RequestSegmentName (uint64_t generation);
std::string ResultSegmentName (uint64_t generation);

} // namespace evp::rayworker

#endif
