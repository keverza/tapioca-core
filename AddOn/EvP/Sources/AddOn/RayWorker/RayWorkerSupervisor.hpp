#ifndef EVP_RAYWORKER_RAYWORKERSUPERVISOR_HPP
#define EVP_RAYWORKER_RAYWORKERSUPERVISOR_HPP

// RayWorker/RayWorkerSupervisor -- the GPU worker's lifecycle as a state
// machine over events and a millisecond stamp the caller supplies (raycast
// decisions 13, 14, 17, 18, 19). No process, pipe or clock of its own: the
// host owns those, and this decides.
//
// The rules it encodes:
// - the worker launches lazily, on the first GPU request, never at load;
// - a worker that connects but reports it cannot serve (old driver, no RT
//   device) is told to exit and GPU is off for the session with that reason --
//   neither changes while Archicad runs, so retrying would only repeat it;
// - a missing executable is the same: off for the session, `worker_missing`;
// - death or silence while a run is in flight marks THAT run failed (its
//   partial results are discarded, never published), keeps the add-on
//   running, and restarts the worker -- rebuilding its scene -- on the next
//   request;
// - repeated crashes disable GPU for the session (`gpu_disabled_for_session`);
// - a result is accepted only for the run in flight and the resident scene;
//   anything else is stale and discarded;
// - every refusal names a stable reason code, and the study falls back to
//   the CPU (decision 14: say so, never degrade silently).

#include "RayWorker/RayWorkerProtocol.hpp"
#include "RayWorker/RayWorkerReason.hpp"

#include <cstdint>
#include <string>

namespace evp::rayworker {

// The second crash in one session disables GPU execution until restart.
constexpr uint32_t kCrashesBeforeDisable = 2;
// Silence longer than this from a live worker counts as a crash.
constexpr int64_t kHeartbeatTimeoutMs = 5000;

enum class WorkerState : uint8_t {
    Stopped = 0,   // no worker; the next GPU request launches one
    Launching = 1, // process started, waiting for Hello
    Ready = 2,     // handshake done, idle
    Busy = 3,      // a run is in flight
    Disabled = 4,  // GPU off for this session; every request falls back
};

// What the host must do after an event. Several may be set at once.
struct SupervisorAction {
    bool launch = false;       // start the worker process
    bool kill = false;         // terminate (and join) the worker process
    bool sendHelloAck = false; // answer Hello with `ack`
    bool uploadScene = false;  // the run needs the scene of its epoch first
    bool sendRun = false;      // the run may be sent now
    bool publish = false;      // the completed result may be published
    bool fallback = false;     // run this study on the CPU, citing `reason`
    uint64_t failedRun = 0;    // nonzero: this run failed; discard its results
    GpuReason reason = GpuReason::None;
    protocol::HelloAckPayload ack;
};

class RayWorkerSupervisor final {
  public:
    // A study wants the GPU for run `generation` against scene `sceneEpoch`.
    SupervisorAction RequestRun (uint64_t generation, uint64_t sceneEpoch, int64_t nowMs);
    // The launch itself failed (executable absent or not startable).
    SupervisorAction LaunchFailed (GpuReason reason, int64_t nowMs);
    // The worker's first message.
    SupervisorAction HelloReceived (const protocol::HelloPayload& hello, int64_t nowMs);
    // A frame refused by the protocol layer (version, framing): stale worker.
    SupervisorAction ProtocolRefused (int64_t nowMs);
    SupervisorAction SceneReady (uint64_t sceneEpoch, int64_t nowMs);
    SupervisorAction RunCompleted (uint64_t generation, uint64_t sceneEpoch, int64_t nowMs);
    SupervisorAction RunFailed (uint64_t generation, GpuReason reason, int64_t nowMs);
    void Heartbeat (int64_t nowMs);
    // Periodic: silence past the timeout is treated as a crash.
    SupervisorAction Tick (int64_t nowMs);
    // The process ended (or was found dead).
    SupervisorAction ProcessExited (int64_t nowMs);
    // Add-on unload / quit: kill and forget, without counting a crash.
    SupervisorAction Shutdown ();

    WorkerState State () const
    {
        return state_;
    }
    GpuReason LastReason () const
    {
        return lastReason_;
    }
    uint32_t Crashes () const
    {
        return crashes_;
    }
    uint32_t Launches () const
    {
        return launches_;
    }
    uint64_t ActiveRun () const
    {
        return activeRun_;
    }
    uint64_t ResidentScene () const
    {
        return residentScene_;
    }
    // The notice line, e.g. "gpu=unavailable reason=worker_crashed: ...".
    std::string Describe () const;

  private:
    SupervisorAction Disable (GpuReason reason);
    SupervisorAction Crashed ();
    SupervisorAction Dispatch ();

    WorkerState state_ = WorkerState::Stopped;
    GpuReason lastReason_ = GpuReason::None;
    uint32_t crashes_ = 0;
    uint32_t launches_ = 0;
    uint64_t activeRun_ = 0;     // the run in flight (Busy), or waiting to go
    uint64_t pendingScene_ = 0;  // the scene the pending/active run needs
    uint64_t residentScene_ = 0; // the scene the worker holds (0: none)
    bool waitingForScene_ = false;
    int64_t lastHeard_ = 0;
};

} // namespace evp::rayworker

#endif
