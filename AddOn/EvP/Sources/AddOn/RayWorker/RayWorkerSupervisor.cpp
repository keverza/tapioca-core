#include "RayWorker/RayWorkerSupervisor.hpp"

namespace evp::rayworker {

namespace {

SupervisorAction FallBack (GpuReason reason)
{
    SupervisorAction action;
    action.fallback = true;
    action.reason = reason;
    return action;
}

} // namespace

SupervisorAction RayWorkerSupervisor::Disable (GpuReason reason)
{
    SupervisorAction action = FallBack (reason);
    action.kill = state_ != WorkerState::Stopped && state_ != WorkerState::Disabled;
    action.failedRun = activeRun_; // whatever was outstanding falls back
    state_ = WorkerState::Disabled;
    lastReason_ = reason;
    activeRun_ = 0;
    sentRun_ = 0;
    residentScene_ = 0;
    waitingForScene_ = false;
    return action;
}

SupervisorAction RayWorkerSupervisor::Crashed ()
{
    ++crashes_;
    const uint64_t lost = activeRun_; // in flight, waiting for its scene or for the launch
    if (crashes_ >= kCrashesBeforeDisable) {
        SupervisorAction action = Disable (GpuReason::DisabledForSession);
        action.kill = true;
        action.failedRun = lost;
        return action;
    }
    SupervisorAction action = FallBack (GpuReason::WorkerCrashed);
    action.kill = true; // make sure a hung worker is gone before the restart
    action.failedRun = lost;
    state_ = WorkerState::Stopped;
    lastReason_ = GpuReason::WorkerCrashed;
    activeRun_ = 0;
    sentRun_ = 0;
    residentScene_ = 0; // a new worker holds nothing
    waitingForScene_ = false;
    return action;
}

// Ready with a pending run: upload its scene first or send it.
SupervisorAction RayWorkerSupervisor::Dispatch ()
{
    SupervisorAction action;
    if (activeRun_ == 0)
        return action;
    if (residentScene_ != pendingScene_) {
        action.uploadScene = true;
        waitingForScene_ = true;
        return action;
    }
    action.sendRun = true;
    waitingForScene_ = false;
    state_ = WorkerState::Busy;
    sentRun_ = activeRun_;
    return action;
}

SupervisorAction RayWorkerSupervisor::RequestRun (uint64_t generation, uint64_t sceneEpoch, int64_t nowMs)
{
    switch (state_) {
        case WorkerState::Disabled:
            return FallBack (lastReason_);
        case WorkerState::Busy:
            // One study at a time: the newer run supersedes the one in flight,
            // which is reported failed so its results are never published.
            {
                const uint64_t superseded = activeRun_;
                activeRun_ = generation;
                pendingScene_ = sceneEpoch;
                SupervisorAction action;
                action.failedRun = superseded;
                action.reason = GpuReason::None;
                // Only the first supersession of the traced run stops it.
                if (superseded != 0 && superseded == sentRun_)
                    action.cancelRun = superseded;
                return action; // sent when the worker answers the old run
            }
        case WorkerState::Stopped: {
            activeRun_ = generation;
            pendingScene_ = sceneEpoch;
            state_ = WorkerState::Launching;
            lastHeard_ = nowMs;
            ++launches_;
            SupervisorAction action;
            action.launch = true;
            return action;
        }
        case WorkerState::Launching:
            activeRun_ = generation;
            pendingScene_ = sceneEpoch;
            return SupervisorAction ();
        case WorkerState::Ready:
            activeRun_ = generation;
            pendingScene_ = sceneEpoch;
            return Dispatch ();
    }
    return SupervisorAction ();
}

SupervisorAction RayWorkerSupervisor::LaunchFailed (GpuReason reason, int64_t nowMs)
{
    (void) nowMs;
    // A missing or unstartable executable does not appear mid-session.
    return Disable (reason == GpuReason::None ? GpuReason::WorkerMissing : reason);
}

SupervisorAction RayWorkerSupervisor::HelloReceived (const protocol::HelloPayload& hello, int64_t nowMs)
{
    lastHeard_ = nowMs;
    if (state_ != WorkerState::Launching)
        return SupervisorAction (); // a second Hello is ignored
    if (hello.reason != GpuReason::None || (hello.capabilities & protocol::CapabilityHardwareRayTracing) == 0) {
        SupervisorAction action = Disable (hello.reason != GpuReason::None ? hello.reason : GpuReason::GpuUnsupported);
        action.sendHelloAck = true;
        action.ack.refusal = DescribeGpuReason (lastReason_);
        return action;
    }
    state_ = WorkerState::Ready;
    lastReason_ = GpuReason::None;
    SupervisorAction action = Dispatch ();
    action.sendHelloAck = true;
    action.ack.capabilities = hello.capabilities;
    return action;
}

SupervisorAction RayWorkerSupervisor::ProtocolRefused (int64_t nowMs)
{
    (void) nowMs;
    // A stale worker beside a newer add-on stays stale until reinstalled.
    return Disable (GpuReason::ProtocolMismatch);
}

SupervisorAction RayWorkerSupervisor::SceneReady (uint64_t sceneEpoch, int64_t nowMs)
{
    lastHeard_ = nowMs;
    if (state_ != WorkerState::Ready && state_ != WorkerState::Busy)
        return SupervisorAction ();
    residentScene_ = sceneEpoch;
    if (state_ == WorkerState::Ready && waitingForScene_)
        return Dispatch ();
    return SupervisorAction ();
}

SupervisorAction RayWorkerSupervisor::RunCompleted (uint64_t generation, uint64_t sceneEpoch, int64_t nowMs)
{
    lastHeard_ = nowMs;
    if (state_ != WorkerState::Busy)
        return SupervisorAction ();
    SupervisorAction action;
    // The worker is free either way.
    state_ = WorkerState::Ready;
    sentRun_ = 0;
    if (generation == activeRun_ && sceneEpoch == pendingScene_ && sceneEpoch == residentScene_) {
        action.publish = true;
        activeRun_ = 0;
        return action;
    }
    // A superseded run finished (it was reported when superseded): discard it
    // and send the newer one.
    return Dispatch ();
}

SupervisorAction RayWorkerSupervisor::RunFailed (uint64_t generation, GpuReason reason, int64_t nowMs)
{
    lastHeard_ = nowMs;
    if (state_ != WorkerState::Busy && !waitingForScene_)
        return SupervisorAction ();
    if (state_ == WorkerState::Busy && generation == sentRun_)
        sentRun_ = 0;
    if (generation != activeRun_) {
        // A superseded or cancelled run failed (a cancelled trace answers
        // so); the newer one, if any, still goes.
        state_ = WorkerState::Ready;
        return Dispatch ();
    }
    // The worker survived: this study falls back, the worker stays.
    SupervisorAction action = FallBack (reason);
    action.failedRun = generation;
    state_ = WorkerState::Ready;
    activeRun_ = 0;
    waitingForScene_ = false;
    if (reason == GpuReason::OutOfMemory)
        residentScene_ = 0; // the scene may not have survived either
    lastReason_ = reason;
    return action;
}

SupervisorAction RayWorkerSupervisor::CancelRun (uint64_t generation, int64_t nowMs)
{
    (void) nowMs;
    SupervisorAction action;
    if (generation == 0 || generation != activeRun_)
        return action;
    // Launching or waiting for the scene: nothing was sent, so nothing is
    // dispatched when the worker gets there. Busy: the worker is still
    // tracing; it answers, and that answer only frees it.
    if (state_ == WorkerState::Busy && sentRun_ == generation)
        action.cancelRun = generation;
    activeRun_ = 0;
    waitingForScene_ = false;
    return action;
}

void RayWorkerSupervisor::Heartbeat (int64_t nowMs)
{
    lastHeard_ = nowMs;
}

SupervisorAction RayWorkerSupervisor::Tick (int64_t nowMs)
{
    const bool alive = state_ == WorkerState::Launching || state_ == WorkerState::Ready || state_ == WorkerState::Busy;
    if (alive && nowMs - lastHeard_ > kHeartbeatTimeoutMs)
        return Crashed ();
    return SupervisorAction ();
}

SupervisorAction RayWorkerSupervisor::ProcessExited (int64_t nowMs)
{
    (void) nowMs;
    if (state_ == WorkerState::Stopped || state_ == WorkerState::Disabled)
        return SupervisorAction (); // an exit we asked for, or already handled
    return Crashed ();
}

SupervisorAction RayWorkerSupervisor::Shutdown ()
{
    SupervisorAction action;
    action.kill = state_ != WorkerState::Stopped && state_ != WorkerState::Disabled;
    action.failedRun = activeRun_;
    if (state_ != WorkerState::Disabled)
        state_ = WorkerState::Stopped;
    activeRun_ = 0;
    sentRun_ = 0;
    residentScene_ = 0;
    waitingForScene_ = false;
    return action;
}

std::string RayWorkerSupervisor::Describe () const
{
    switch (state_) {
        case WorkerState::Stopped:
            return lastReason_ == GpuReason::None
                       ? "gpu=idle"
                       : std::string ("gpu=restarting reason=") + GpuReasonCode (lastReason_) + ": " +
                             DescribeGpuReason (lastReason_);
        case WorkerState::Launching:
            return "gpu=starting";
        case WorkerState::Ready:
            return "gpu=ready";
        case WorkerState::Busy:
            return "gpu=tracing run " + std::to_string (activeRun_);
        case WorkerState::Disabled:
            return std::string ("gpu=unavailable reason=") + GpuReasonCode (lastReason_) + ": " +
                   DescribeGpuReason (lastReason_);
    }
    return "gpu=unknown";
}

} // namespace evp::rayworker
