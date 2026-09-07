#ifndef EVP_RHINOCOMPUTE_RHINOCOMPUTEMANAGER_HPP
#define EVP_RHINOCOMPUTE_RHINOCOMPUTEMANAGER_HPP

// The supervisor of ONE compute.geometry.exe, and Tapioca's second Grasshopper
// backend.
//
// The first backend is Tapioca.GhWorker.exe over a named pipe: stateful,
// interactive, a live canvas. This one is stateless HTTP with no canvas, no
// window and no message loop. They are ALTERNATIVES BEHIND ONE CONTRACT — see
// docs/architecture/api/SPEC-RhinoCompute.md — and every rule GhWorkerHost.hpp
// states about supervision applies here unchanged:
//
//   1. ONE WORKER. Start is idempotent rather than merely safe to call twice.
//   2. THE WORKER IS EXPENDABLE. A definition that hangs, a third-party .gha
//      that wedges, an access violation inside Rhino: each costs the worker and
//      nothing else. Archicad must survive its death without a modal, a lost
//      model or an add-on unload.
//   3. ARCHICAD MUST SURVIVE ITS ABSENCE. No Rhino, no Hops package, an
//      unlicensed Rhino: each is a message and a log line, never a failed
//      Initialize and never a crash.
//   4. NOTHING WAITS FOR IT ON ARCHICAD'S MAIN THREAD. Cold start here was
//      MEASURED at 50-90 s, almost all of it third-party .gha loading. A menu
//      command that blocked on that would freeze Archicad for a minute and a
//      half. Start spawns and returns; readiness arrives on the poll thread.
//
// ⚠️ WE SPAWN THE CHILD, NOT rhino.compute.exe. The parent is a reverse proxy
// and load balancer for multi-child deployments. It abandons a child that takes
// longer than 60 s to start — which this machine's does — and its /healthcheck
// answers "Healthy" with zero working children. Both measured. See the spec.
//
// ⚠️ READINESS IS GET /version, NEVER /healthcheck. /healthcheck proves an
// ASP.NET pipeline is listening. /version proxies to something that can actually
// solve, and fails with "No compute server found" until one exists.

#include "Grasshopper/HostState.hpp"
#include "RhinoCompute/RequestSequence.hpp"
#include "RhinoCompute/RhinoComputeProtocol.hpp"

#include "UniString.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace evp {
namespace rhinocompute {

// How the worker is launched and reached. Every field has a working default;
// the settings surface may override any of them.
struct ComputeConfig {
    // Loopback only. The worker has no authentication worth the name and binding
    // it anywhere else would publish a Rhino to the network.
    std::string host = "127.0.0.1";

    // compute.geometry.exe's own default. It IGNORES --port; the port is set
    // through RHINO_COMPUTE_URLS, which the manager builds from this.
    uint16_t port = 5000;

    // Absolute path to compute.geometry.exe. Empty means "discover it", which
    // looks where the Hops package puts it.
    GS::UniString executable;

    // How long to wait for GET /version to answer before calling the start
    // failed. Generous because the measured cold start is 50-90 s and a machine
    // with more plugins will be slower.
    uint32_t readinessTimeoutMs = 180000;

    // Per-request timeout for a solve. A definition slower than this is a
    // refusal the user can act on, not a hang.
    uint32_t solveTimeoutMs = 60000;
};

// What the panel shows. Distinct from HostState because Busy is not a lifecycle
// transition — a solve in flight does not change what the worker IS — and
// folding it in would make "ready" and "working" race each other.
enum class ComputeState {
    Offline,
    Starting,
    Ready,
    Busy,
    Failed,
};

const char* DescribeComputeState (ComputeState state);

class RhinoComputeManager {
  public:
    static RhinoComputeManager& Get ();

    ComputeState State () const;
    bool IsReady () const;

    // Spawns the worker if it is not up and returns IMMEDIATELY. True means the
    // request was ACCEPTED, not that the worker is ready — on a cold start those
    // are 90 seconds apart, which is exactly why this does not block.
    //
    // ⚠️ CALL THIS WHEN THE WORKFLOW PANEL OPENS, NOT ON THE FIRST SOLVE. A user
    // who changes a slider and waits 90 s for a preview will conclude the
    // feature is broken. Starting on panel open spends that time while they are
    // still choosing a definition.
    bool Start (GS::UniString& message);

    // Cooperative shutdown, then the guarantee. Idempotent, safe during teardown,
    // and never dependent on an Archicad service still being alive: called from
    // APINotify_Quit and FreeData.
    void Stop ();

    bool Restart (GS::UniString& message);

    // ── menu entry points ────────────────────────────────────────────────────
    //
    // ⚠️ MAIN THREAD ONLY. Both report through ACAPI and are wired to
    // "Tapioca > Start rhino.compute" and "Tapioca > Stop rhino.compute".
    //
    // ⚠️ TWO ITEMS RATHER THAN ONE THAT TOGGLES, for the same reason
    // ResourceIds.hpp gives for the 3D viewer and overlay: a toggle has to name
    // one state, and this subsystem spends up to 90 SECONDS in a third one. A
    // single "rhino.compute" item would read "Stop" while the worker was still
    // starting, and clicking it would look like it did nothing.
    static void StartFromMenu ();
    static void StopFromMenu ();

    // Runs when readiness is won or lost. Called on the POLL THREAD, never the
    // main thread — marshal through MainThreadGate before touching ACAPI or DG.
    void OnStateChanged (std::function<void (ComputeState)> handler);

    // ── solving ──────────────────────────────────────────────────────────────
    //
    // ⚠️ BOTH OF THESE BLOCK ON HTTP AND MUST NOT BE CALLED FROM THE MAIN THREAD.
    // They are for the workflow controller's worker thread. The main thread's
    // only jobs here are Start, Stop and reading State.

    // Uploads the definition and reads back its public inputs. Always carries the
    // definition rather than a pointer: a schema answered from a cache could
    // predate an edit the author just made.
    bool InspectDefinition (const GS::UniString& ghFile, WorkflowSchema& schema, GS::UniString& message);

    // Solves. Prefer request.pointer, which the schema carried: the same solve
    // measured 52 ms carrying the definition and 4.5 ms carrying the pointer, and
    // a 100 ms debounce that re-uploads spends its entire budget on transport.
    bool Solve (const SolveRequest& request, SolveResult& result, GS::UniString& message);

    // The ordering gate for preview results. Take an id before issuing a solve;
    // offer the result to Accept before drawing it.
    RequestSequence& Sequence ();

    // Multi-line: state, pid, generation, endpoint, readiness age, last error.
    // What a support question should ask for first.
    GS::UniString Describe () const;

    // Where compute.geometry.exe was found, or why it was not. Separated from
    // Start so a settings page can report it without launching anything.
    static bool DiscoverExecutable (GS::UniString& path, GS::UniString& message);

  private:
    RhinoComputeManager () = default;
};

} // namespace rhinocompute
} // namespace evp

#endif
