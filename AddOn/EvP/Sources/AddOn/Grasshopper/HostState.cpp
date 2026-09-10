#include "HostState.hpp"

namespace evp {
namespace grasshopper {

const char* DescribePeerOwnership (PeerOwnership ownership)
{
    switch (ownership) {
        case PeerOwnership::None:
            return "none";
        case PeerOwnership::Spawned:
            return "spawned by Tapioca";
        case PeerOwnership::Attached:
            return "attached to a peer Tapioca did not start";
    }
    return "unknown";
}

const char* DescribeHostState (HostState state)
{
    switch (state) {
        case HostState::NotStarted:
            return "not started";
        case HostState::Starting:
            return "starting";
        case HostState::Running:
            return "running";
        case HostState::Stopping:
            return "stopping";
        case HostState::Stopped:
            return "stopped";
        case HostState::Failed:
            return "failed";
    }
    return "unknown";
}

HostState HostLifecycle::State () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return state;
}

bool HostLifecycle::IsRunning () const
{
    return State () == HostState::Running;
}

bool HostLifecycle::AcceptsMessages () const
{
    return State () == HostState::Running;
}

StartDecision HostLifecycle::BeginStart (PeerOwnership claim)
{
    std::lock_guard<std::mutex> lock (mutex);
    switch (state) {
        case HostState::Running:
            return StartDecision::AlreadyRunning;
        case HostState::Starting:
        case HostState::Stopping:
            return StartDecision::InProgress;
        case HostState::NotStarted:
        case HostState::Failed:
        case HostState::Stopped:
            // Stopped included: see the header. The worker is a separate,
            // expendable process, so a stop leaves nothing in Archicad to
            // collide with and a restart is an ordinary start.
            break;
    }
    state = HostState::Starting;
    // ⚠️ RECORDED AT THE START AND NOT AT THE HANDSHAKE. A start that fails half
    // way still has to be torn down the right way, and "was this ours to kill"
    // is a question the failure path asks before any peer has said hello.
    ownership = claim;
    // Incremented here rather than on success, so a worker that dies during its
    // own start still owns a distinct generation in the log.
    ++generation;
    lastError.clear ();
    return StartDecision::Proceed;
}

bool HostLifecycle::CompleteStart (uint32_t startGeneration)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state != HostState::Starting || generation != startGeneration)
        return false;
    state = HostState::Running;
    return true;
}

bool HostLifecycle::Fail (uint32_t failedGeneration, const std::string& reason)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (generation != failedGeneration || (state != HostState::Starting && state != HostState::Running))
        return false;
    state = HostState::Failed;
    lastError = reason;
    // Nothing is up, so nothing is owned. Leaving Spawned here would let a
    // later stop go looking for a process this generation never had.
    ownership = PeerOwnership::None;
    return true;
}

bool HostLifecycle::BeginStop ()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state != HostState::Starting && state != HostState::Running)
        return false;
    state = HostState::Stopping;
    return true;
}

void HostLifecycle::CompleteStop ()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state == HostState::Stopping) {
        state = HostState::Stopped;
        ownership = PeerOwnership::None;
    }
}

PeerOwnership HostLifecycle::Ownership () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return ownership;
}

bool HostLifecycle::OwnsPeerProcess () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return ownership == PeerOwnership::Spawned;
}

uint32_t HostLifecycle::Generation () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return generation;
}

std::string HostLifecycle::LastError () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return lastError;
}

} // namespace grasshopper
} // namespace evp
