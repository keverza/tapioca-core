#ifndef EVP_GRASSHOPPER_GHBRIDGE_HPP
#define EVP_GRASSHOPPER_GHBRIDGE_HPP

// The Archicad end of the Archicad <-> Tapioca.GhWorker.exe transport.
//
// ⚠️ WHY THIS EXISTS INSTEAD OF THE IN-PROCESS FUNCTION-POINTER TABLE IT
// REPLACES. Two independent findings, recorded in
// docs/architecture/api/HANDOFF-GrasshopperInsideArchicad.md, "Execution
// boundary: Grasshopper runs OUT of process":
//
//   1. Measured 2026-08-26: while a Grasshopper solution holds Archicad's main
//      thread inside an add-on callback, Archicad executes NO JSON command that
//      needs that thread. That one has in-process answers, and the direct native
//      call this file replaces was one of them.
//   2. There is no fault isolation in process. An infinite loop in a C# script
//      component, a hung third-party .gha, a blocking dialog or an access
//      violation inside Rhino takes ARCHICAD down, unsaved model included.
//      Finding 2 has exactly one answer, and it is a process boundary.
//
// So the solver lives in a worker Tapioca owns and can kill. This class is how
// the two halves talk, and the rules it keeps are the ones that make the
// boundary worth having:
//
// * ARCHICAD'S MAIN THREAD NEVER BLOCKS ON THE WORKER. The pipe is served on a
//   Tapioca-owned IO thread. Worker requests reach Archicad through
//   MainThreadGate like every other off-thread caller already does, and the main
//   thread waits on the GATE (which has a timeout) rather than on a socket.
// * EVERY ACAPI CALL STAYS NATIVE AND IN THIS PROCESS. The worker has no
//   Archicad SDK and cannot be given one; it asks, by name, over the wire.
// * IDENTITY CROSSES AS TEXT AND GUIDS, NEVER AS POINTERS. The payloads here are
//   POD headers and UTF-8; no API_*, GS::* or STL type crosses the boundary.
// * NOTHING IS TRUSTED. Every message is length-checked and version-checked
//   before it is acted on (GhProtocol.hpp), and a request that arrives after a
//   stop has begun is refused rather than served.
//
// Reads happen on the IO thread; Send() writes from whatever thread calls it,
// under its own mutex. That is legal on one OVERLAPPED duplex pipe and is what
// keeps a control message (cancel, hide editor) from queueing behind an
// in-flight ACAPI round trip.

#include "GhProtocol.hpp"

#include "Preview/GhPreviewIngest.hpp"
#include "Preview/GhPreviewSegmentView.hpp"

#include "UniString.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace evp {
namespace grasshopper {

class GhBridge {
  public:
    static GhBridge& Get ();

    // Creates the server pipe and starts the IO thread. Call BEFORE spawning the
    // worker: the worker connects to a name it is given on its command line, and
    // a name that is not listening yet is a startup race with no upside.
    // `generation` only names the pipe and the log lines; the lifecycle owns it.
    bool Start (uint32_t generation, GS::UniString& error);

    // Idempotent and safe during teardown. Cancels the IO, joins the thread and
    // closes the pipe; it does NOT kill the worker, because a bridge that is
    // down and a worker that is dead are two different things and GhWorkerHost
    // owns the second.
    void Stop ();

    GS::UniString PipeName () const;

    // True once a worker has completed the handshake on this pipe.
    bool IsConnected () const;

    // The worker's own pid, as it reported it in its hello. 0 until then.
    uint32_t WorkerProcessId () const;

    // Milliseconds since the last heartbeat, or a very large number when none
    // has ever arrived. This is what GhWorkerHost's liveness deadline reads.
    uint64_t MillisecondsSinceHeartbeat () const;

    // Host -> worker control (ShowEditor, HideEditor, Shutdown). Returns false
    // with a reason when there is no connected worker to send to. Never blocks
    // on a reply: the answer, if there is one, arrives later as an Ack.
    bool Send (protocol::MessageType type, GS::UniString& error);

    // The worker's last Ack or refusal, for the status report. Never merged with
    // the native side's own account of the state.
    GS::UniString LastWorkerMessage () const;

    // Run once, on the IO thread, the moment a worker completes the handshake.
    //
    // ⚠️ THIS IS WHAT KEEPS "OPEN THE EDITOR" OFF ARCHICAD'S MAIN THREAD. A
    // worker takes as long as a cold .NET start plus RhinoCore to answer, and a
    // menu command that waited for that would freeze Archicad for exactly as
    // long as the thing the process boundary exists to stop freezing it.
    // GhWorkerHost therefore spawns, returns, and leaves the follow-up here.
    // The handler must be self-contained and must not touch ACAPI directly.
    void SetConnectedHandler (std::function<void ()> handler);

    // Run once, on the IO thread, when a worker answers a RunDefinition.
    //
    // A Run is asynchronous by construction: the menu command returns as soon as
    // the request is sent, because a solve takes as long as the definition takes
    // and Archicad's main thread is not available to wait for it. The report
    // therefore arrives later and unsolicited, and this is what shows it. The
    // handler must be self-contained and must reach ACAPI only through
    // MainThreadGate.
    void SetRunResultHandler (std::function<void (const protocol::RunReportPayload&)> handler);

  private:
    GhBridge () = default;
    ~GhBridge ();
    GhBridge (const GhBridge&) = delete;
    GhBridge& operator= (const GhBridge&) = delete;

    void Run ();
    void NotifyConnected ();

    std::atomic<bool> stopping { false };
    std::atomic<bool> connected { false };
    std::atomic<uint32_t> workerProcessId { 0 };
    std::atomic<uint64_t> lastHeartbeatTick { 0 };
    std::atomic<uint32_t> generation { 0 };
    std::thread io;
    void* pipe = nullptr; // HANDLE, kept opaque so <windows.h> stays out of this header
    mutable std::mutex writeMutex;
    mutable std::mutex messageMutex;
    // ⚠️ IO THREAD ONLY, AND THAT IS WHY THERE IS NO LOCK ON THEM. Every
    // preview message arrives on the pipe, is decoded on the IO thread, and is
    // applied here on the IO thread; the only thing that crosses to another
    // thread is the immutable snapshot GhPreviewCache publishes. The segment is
    // a mapped view of the WORKER's memory and must never outlive the batch it
    // belongs to, which is what makes single-threaded ownership here the point
    // rather than a convenience.
    evp::preview::GhPreviewSegmentView previewSegment;
    evp::preview::GhPreviewIngest previewIngest { evp::preview::GhPreviewCache::Get (), previewSegment };

    std::function<void ()> connectedHandler;
    std::function<void (const protocol::RunReportPayload&)> runResultHandler;
    GS::UniString lastWorkerMessage;
    GS::UniString pipeName;
};

} // namespace grasshopper
} // namespace evp

#endif
