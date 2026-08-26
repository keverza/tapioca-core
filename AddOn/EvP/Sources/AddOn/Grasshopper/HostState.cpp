#include "HostState.hpp"

namespace evp {
namespace grasshopper {

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

StartDecision HostLifecycle::BeginStart ()
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
    // Incremented here rather than on success, so a worker that dies during its
    // own start still owns a distinct generation in the log.
    ++generation;
    lastError.clear ();
    return StartDecision::Proceed;
}

void HostLifecycle::CompleteStart ()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state == HostState::Starting)
        state = HostState::Running;
}

void HostLifecycle::FailStart (const std::string& reason)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state == HostState::Starting)
        state = HostState::Failed;
    lastError = reason;
}

bool HostLifecycle::BeginStop ()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state != HostState::Running)
        return false;
    state = HostState::Stopping;
    return true;
}

void HostLifecycle::CompleteStop ()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state == HostState::Stopping)
        state = HostState::Stopped;
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
