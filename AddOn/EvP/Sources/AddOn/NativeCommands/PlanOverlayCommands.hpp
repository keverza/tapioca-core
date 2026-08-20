#ifndef EVP_NATIVECOMMANDS_PLANOVERLAYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_PLANOVERLAYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Win32 child-window enumeration, test overlay window creation, and state
// queries for the PlanOverlayProbe. Phase 0 of the bgfx plan overlay: these
// answer whether the plan canvas is a real HWND, whether a click-through child
// stays put, and whether Archicad tools pass through it.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetPlanOverlayCommandRegistrations ();

// Register this domain's READ commands on Archicad's JSON port — see
// SnapshotCommands.hpp for why installation is per-domain.
GSErrCode InstallPlanOverlayJsonCommands ();

// Destroy the probe overlay, restore any style bit it set on a host window, and
// unregister the window class. MUST run from FreeData: an overlay window whose
// WndProc lives in this DLL outliving the unload takes Archicad down on exit,
// and that is not theoretical — it crashed on close during the first live run.
void ShutdownPlanOverlay ();

} // namespace geomsrv

#endif
