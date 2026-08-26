#ifndef EVP_GRASSHOPPER_HOSTSTATE_HPP
#define EVP_GRASSHOPPER_HOSTSTATE_HPP

// The Grasshopper worker's lifecycle rules, on their own, with nothing else in
// them.
//
// Deliberately DevKit-free, Win32-free and CLR-free — only <cstdint>, <mutex>
// and <string> — for the same reason Python/PathUtils.hpp is ACAPI-free: this is
// the half of the host that can be exercised offline. Everything the lifecycle
// evidence has to be sure of (a second Start cannot spawn a second worker, a
// failed start does not leave the host looking Running, nothing accepts a worker
// message after a stop has begun) is a rule about state, not about processes,
// and a rule about state can be proved by a test instead of by a live Archicad
// run.
//
// ⚠️ Stopped IS NO LONGER TERMINAL, AND THAT IS THE WHOLE POINT OF THE PROCESS
// BOUNDARY. In process, a stopped host was terminal: hostfxr_close does not
// unload a CLR, RhinoCore could not be reconstructed in the same process, and
// the only honest answer to a second click was "restart Archicad". Out of
// process every one of those constraints belongs to the WORKER, which is
// expendable — killing it and spawning another is the recovery primitive
// HANDOFF-GrasshopperInsideArchicad.md ("Supervision is the point") requires.
// So a stop is followed by a start, and each start gets a new GENERATION so the
// log can say which worker a line came from.

#include <cstdint>
#include <mutex>
#include <string>

namespace evp {
namespace grasshopper {

// Mirrors WorkerState in Sources/GhWorker/BridgeProtocol.cs; the worker reports
// its own copy of these and the two are compared, never assumed equal.
enum class HostState {
    NotStarted = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Stopped = 4,
    Failed = 5,
};

const char* DescribeHostState (HostState state);

// Why a BeginStart was refused. The caller needs to tell these apart: Running is
// success for a menu command that only wants a worker up, while InProgress is a
// message for the user.
enum class StartDecision {
    Proceed, // this call owns the start; it must Complete or Fail it
    AlreadyRunning,
    InProgress, // another thread is starting or stopping it right now
};

class HostLifecycle {
  public:
    HostState State () const;
    bool IsRunning () const;

    // True only while the worker is fully up. Every worker-originated message is
    // gated on this, so the answer flips to false the instant a stop BEGINS —
    // not when it finishes — which is what stops a late request from arriving in
    // an add-on that is already tearing its own state down.
    bool AcceptsMessages () const;

    // Exactly one caller can get Proceed. Everyone else is told why not.
    StartDecision BeginStart ();
    void CompleteStart ();
    void FailStart (const std::string& reason);

    // True when this call owns the stop. False when there is nothing to stop, so
    // a shutdown path can call it unconditionally.
    bool BeginStop ();
    void CompleteStop ();

    // 1 for the first worker of the session, 2 for the one that replaced it, and
    // so on. Stamped on every bridge log line beside the pid, because "the
    // worker died and came back" and "the worker never died" produce identical
    // logs without it.
    uint32_t Generation () const;

    std::string LastError () const;

  private:
    mutable std::mutex mutex;
    HostState state = HostState::NotStarted;
    uint32_t generation = 0;
    std::string lastError;
};

} // namespace grasshopper
} // namespace evp

#endif
