#ifndef EVP_GRASSHOPPER_HOSTSTATE_HPP
#define EVP_GRASSHOPPER_HOSTSTATE_HPP

// The Grasshopper host's lifecycle rules, on their own, with nothing else in them.
//
// Deliberately DevKit-free, Win32-free and CLR-free — only <mutex> and <string> —
// for the same reason Python/PathUtils.hpp is ACAPI-free: this is the half of the
// host that can be exercised offline. Everything the lifecycle probe has to be
// sure of (a second Start cannot create a second RhinoCore, a failed start does
// not leave the host looking Running, nothing calls back into the add-on after a
// Stop) is a rule about state, not about Rhino, and a rule about state can be
// proved by a test instead of by a live Archicad run.
//
// ⚠️ Stopped IS TERMINAL. HANDOFF-GrasshopperInsideArchicad.md, "Host lifecycle
// prerequisite": same-process RhinoCore reconstruction is unsupported until it
// has been measured, so a restart after a stop is REFUSED here rather than
// attempted and hoped for. An Archicad restart is the documented fallback. When
// reconstruction is one day evidenced, this is the one place that changes.

#include <mutex>
#include <string>

namespace evp {
namespace grasshopper {

// Mirrors TapiocaGhState in GrasshopperHostApi.h; the managed session reports
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
// success for a menu command that only wants the host up, while InProgress and
// Stopped are messages for the user.
enum class StartDecision {
    Proceed, // this call owns the start; it must Complete or Fail it
    AlreadyRunning,
    InProgress, // another thread is starting it right now
    Terminal,   // stopped once; this process cannot host Rhino again
};

class HostLifecycle {
  public:
    HostState State () const;
    bool IsRunning () const;

    // True only while the host is fully up. Every managed->native callback is
    // gated on this, so the answer flips to false the instant a stop BEGINS —
    // not when it finishes — which is what stops a late callback from arriving
    // in an add-on that is already tearing its own state down.
    bool AcceptsCallbacks () const;

    // Exactly one caller can get Proceed. Everyone else is told why not.
    StartDecision BeginStart ();
    void CompleteStart ();
    void FailStart (const std::string& reason);

    // True when this call owns the stop. False when there is nothing to stop, so
    // a shutdown path can call it unconditionally.
    bool BeginStop ();
    void CompleteStop ();

    std::string LastError () const;

  private:
    mutable std::mutex mutex;
    HostState state = HostState::NotStarted;
    std::string lastError;
};

} // namespace grasshopper
} // namespace evp

#endif
