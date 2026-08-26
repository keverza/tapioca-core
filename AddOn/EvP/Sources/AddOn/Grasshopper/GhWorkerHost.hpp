#ifndef EVP_GRASSHOPPER_GHWORKERHOST_HPP
#define EVP_GRASSHOPPER_GHWORKERHOST_HPP

// The supervisor of Tapioca.GhWorker.exe (PLAT-RHINO-INSIDE, P0/P0b).
//
// What it is: the native owner of ONE worker process, spawned LAZILY from a menu
// command and never from DLL attach, CheckEnvironment, RegisterInterface or a
// global constructor. What it is not: anything that knows what a Grasshopper
// document is. Definitions, Player inputs and Tapir live in the worker.
//
// Four rules decide the shape of this class, all of them from
// docs/architecture/api/HANDOFF-GrasshopperInsideArchicad.md:
//
//   1. ONE WORKER. Editor and Player share it, so ownership lives here and not
//      in whatever opens a window. Start is idempotent rather than merely safe
//      to call twice.
//   2. THE WORKER IS EXPENDABLE, AND THAT IS THE PRODUCT FEATURE. An infinite
//      loop in a script component, a hung third-party .gha, a blocking dialog or
//      a native access violation inside Rhino must cost the WORKER and nothing
//      else. Cancel is the cooperative first attempt; TerminateProcess is the
//      guaranteed second, and it must leave Archicad untouched — no modal, no
//      lost model, no add-on unload.
//   3. ARCHICAD MUST SURVIVE ITS ABSENCE. No .NET, no Rhino, an unlicensed
//      Rhino, a worker that was never built: each is a message in a dialog and a
//      line in the log, never a failed Initialize and never a crash. The .apx
//      links nothing from Rhino or .NET and never loads a CLR.
//   4. NOTHING HERE WAITS FOR THE WORKER ON ARCHICAD'S MAIN THREAD. A cold start
//      is a .NET runtime plus RhinoCore plus Grasshopper; a menu command that
//      blocked on that would freeze Archicad for exactly as long as the process
//      boundary exists to stop it freezing. Start spawns and returns; the follow
//      up runs on the bridge's IO thread when the handshake completes.
//
// ⚠️ MAIN THREAD ONLY for the public methods below — they read ACAPI (the JSON
// port) and report through it. The supervisor's own thread never touches ACAPI
// except through MainThreadGate.

#include "UniString.hpp"

namespace evp {
namespace grasshopper {

enum class HostState;

class GhWorkerHost {
  public:
    static GhWorkerHost& Get ();

    // What "Tapioca > Grasshopper Editor" does: spawn the worker if it is not up
    // and ask it for its canvas. Idempotent — a second click on a running worker
    // brings the canvas forward rather than building anything.
    //
    // Returns true when the request was ACCEPTED, which on a cold start means
    // "the worker is starting and will show its canvas when it is up", not "the
    // canvas is on screen". Rule 4: the difference is the whole reason this does
    // not block.
    bool OpenEditor (GS::UniString& message);
    bool HideEditor (GS::UniString& message);

    static void OpenEditorFromMenu ();

    // Asks the worker to solve the definition on its canvas once, and returns
    // IMMEDIATELY. A solve takes as long as the definition takes, and Archicad's
    // main thread is not available to wait for it -- that is the entire point of
    // the worker -- so the report arrives later, over the bridge, and is shown
    // from there.
    bool RunDefinition (GS::UniString& message);

    // The cooperative half of cancellation. Grasshopper honours an abort only
    // BETWEEN components, so this cannot stop a component wedged in native code
    // or a blocking call; "Close Grasshopper" is the half that always works.
    bool CancelRun (GS::UniString& message);

    static void RunFromMenu ();

    // What "Tapioca > Close Grasshopper" does, and the user-facing half of rule 2.
    //
    // ⚠️ THIS EXISTS BECAUSE THE CANVAS'S OWN X DOES NOT CLOSE THE WORKER, AND
    // THAT IS DELIBERATE RATHER THAN AN OVERSIGHT. Grasshopper's close button
    // hides its editor; Rhino, Grasshopper and the loaded packages stay up, which
    // is what makes reopening the canvas instant instead of a fresh ten-second
    // start, and what will let the Player share one worker with the Editor. But a
    // hidden process holding a Rhino licence with no way to stop it short of
    // quitting Archicad is not a defensible product either. So closing is an
    // explicit command: cooperative shutdown first, TerminateProcess second, and
    // a report of which one was needed.
    static void CloseFromMenu ();

    // Cooperative shutdown, then the guarantee. Idempotent and safe during
    // teardown: does nothing when nothing is up, and never depends on an
    // Archicad service still being alive. Called from APINotify_Quit and
    // FreeData.
    void Stop ();

    bool IsRunning () const;
    HostState State () const;

    // A multi-line report: state, worker pid and restart generation, pipe name,
    // heartbeat age, the Archicad JSON port a Tapir ConnectArchicad component
    // needs, and the last message from either side. This is what a support
    // question should ask for first.
    GS::UniString Describe () const;

  private:
    GhWorkerHost () = default;
};

} // namespace grasshopper
} // namespace evp

#endif
