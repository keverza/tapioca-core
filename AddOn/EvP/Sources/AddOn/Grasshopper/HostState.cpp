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

bool HostLifecycle::AcceptsCallbacks () const
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
        case HostState::Stopped:
            // See the header: reconstruction in the same process is unmeasured,
            // so it is refused rather than attempted.
            return StartDecision::Terminal;
        case HostState::NotStarted:
        case HostState::Failed:
            // A start that never got a core up left nothing behind to collide
            // with, so a retry (a missing Rhino installed since, a licence
            // fixed) is allowed. Only a start that SUCCEEDED and was then
            // stopped is terminal.
            break;
    }
    state = HostState::Starting;
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

std::string HostLifecycle::LastError () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return lastError;
}

} // namespace grasshopper
} // namespace evp
