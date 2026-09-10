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

// Who owns the peer on the other end of the bridge.
//
// ⚠️ THE HOST HAS ALWAYS CONFLATED "THE PEER" WITH "THE PROCESS I STARTED", AND
// EVERY MODE BEYOND spawn-and-kill NEEDS THEM APART. The bridge is a named-pipe
// SERVER: the add-on listens and the peer connects. Nothing in that arrangement
// requires the peer to have been spawned here -- a Grasshopper already running
// in the user's own Rhino can complete the identical handshake -- but the host
// answered a stop by terminating a process and closing a Job Object, which for a
// peer it does not own would be killing the user's Rhino.
//
// So ownership is recorded per start and the rules follow from it:
//   * Spawned  -- ours: cooperative Shutdown, then TerminateProcess, then the
//                 Job Object closes over anything the worker itself started.
//   * Attached -- someone else's: a stop DISCONNECTS. No signal, no kill; the
//                 peer sees its pipe close and decides for itself what that
//                 means.
// A generation still advances either way, because staleness is about which
// conversation a message belongs to and not about who started it.
enum class PeerOwnership {
    None = 0, // nothing is up
    Spawned = 1,
    Attached = 2,
};

const char* DescribePeerOwnership (PeerOwnership ownership);

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
    StartDecision BeginStart (PeerOwnership claim = PeerOwnership::Spawned);
    // Generation-aware because bridge and process callbacks are asynchronous. A
    // callback from a worker that has already been stopped must not complete or
    // fail its replacement's start.
    bool CompleteStart (uint32_t startGeneration);

    // Who owns whatever is up. None once a stop or a failure has completed.
    PeerOwnership Ownership () const;

    // Whether a stop may terminate a process. FALSE for an attached peer, which
    // is the one question the whole distinction exists to answer.
    bool OwnsPeerProcess () const;
    bool Fail (uint32_t failedGeneration, const std::string& reason);

    // Starting is stoppable: quitting Archicad while Rhino is booting must revoke
    // late bridge callbacks immediately rather than waiting for a handshake.
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
    PeerOwnership ownership = PeerOwnership::None;
    std::string lastError;
};

} // namespace grasshopper
} // namespace evp

#endif
