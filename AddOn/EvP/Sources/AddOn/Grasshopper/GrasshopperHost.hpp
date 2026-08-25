#ifndef EVP_GRASSHOPPER_GRASSHOPPERHOST_HPP
#define EVP_GRASSHOPPER_GRASSHOPPERHOST_HPP

// The one process-wide Rhino.Inside host (PLAT-RHINO-INSIDE, slice 0).
//
// What it is: the native owner of a single .NET runtime, a single hidden
// RhinoCore and a single stock Grasshopper, started LAZILY from a menu command
// and never from DLL attach, CheckEnvironment, RegisterInterface or a global
// constructor. What it is not: anything that knows what a Grasshopper document
// is. Definitions, Player inputs and Tapir come later and on top.
//
// Three rules decide the shape of this class, all of them from
// docs/architecture/api/HANDOFF-GrasshopperInsideArchicad.md:
//
//   1. ONE core. Editor and Player will share it, so the ownership lives here
//      and not in whatever opens a window. That is why this is a singleton and
//      why Start is idempotent rather than merely safe to call twice.
//   2. ARCHICAD MUST SURVIVE ITS ABSENCE. No Rhino, no .NET, an unlicensed
//      Rhino, a stale managed assembly: each is a message in a dialog and a
//      line in the log, never a failed Initialize and never a crash. The .apx
//      links nothing from Rhino or from .NET, so a machine without either
//      loads Tapioca exactly as before.
//   3. QUIT IS CLEAN. Shutdown detaches everything of ours that a foreign
//      runtime holds a pointer to, in the same spirit as AddOnMain's FreeData.
//      Restarting the host inside one Archicad session is NOT supported and is
//      refused rather than attempted; see HostState.hpp.
//
// ⚠️ MAIN THREAD ONLY. Every method here must be called from Archicad's main
// thread — it is the STA owner, and RhinoCore's ownership is thread-affine.
// Nothing in this class marshals for you: a worker that wants the host must go
// through MainThreadGate like every other Archicad access.

#include "UniString.hpp"

namespace evp {
namespace grasshopper {

enum class HostState;

class GrasshopperHost {
  public:
    static GrasshopperHost& Get ();

    // What "Tapioca > Rhino.Inside" does: start the host if it is not up, then
    // report the outcome to the user and to logs\grasshopper.log. Safe to
    // invoke repeatedly; a second invocation on a running host reports the
    // running host rather than building a second one.
    static void OpenFromMenu ();

    // Starts the runtime, the core and (per the flags baked in below) stock
    // Grasshopper. Returns true when the host is up afterwards — including the
    // case where it already was. `message` always carries something worth
    // showing a user, on success as well as on failure.
    bool Start (GS::UniString& message);

    // Idempotent and safe during teardown: does nothing when nothing is up.
    // Called from APINotify_Quit and FreeData, where it must not depend on any
    // Archicad service still being alive.
    void Stop ();

    bool IsRunning () const;
    HostState State () const;

    // A multi-line report: state, runtime, assembly paths, and the last
    // message from either side. This is what the lifecycle probe records and
    // what a support question should ask for first.
    GS::UniString Describe () const;

  private:
    GrasshopperHost () = default;
};

} // namespace grasshopper
} // namespace evp

#endif
