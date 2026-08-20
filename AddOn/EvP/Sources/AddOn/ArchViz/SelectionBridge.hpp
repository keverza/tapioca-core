#ifndef EVP_ARCHVIZ_SELECTIONBRIDGE_HPP
#define EVP_ARCHVIZ_SELECTIONBRIDGE_HPP

// ArchViz/SelectionBridge — selection, crossing between the viewer and Archicad.
//
// WHY IT IS A MAIN-THREAD WIN32 TIMER. Selection has to cross in both
// directions, and each direction is blocked by a different rule:
//
//   viewer -> Archicad   the pick happens on the RENDER thread, which may never
//                        call ACAPI (RenderThread.hpp's own contract) and may
//                        not use the gate either;
//   Archicad -> viewer   Archicad's selection is NOT an element change, so it
//                        never appears in ChangeTracker and live sync cannot
//                        carry it.
//
// A WM_TIMER runs on the main thread, so ACAPI is legal here directly — no gate
// hop, no worker. Same mechanism as the nav log and the overlay camera sync, for
// the same reason, and it is bounded work: one atomic read plus at most one
// selection call per tick.
//
// ⚠️ IT MUST BE KILLED ON TEARDOWN, UNCONDITIONALLY. A ::SetTimer is a
// process-wide Win32 resource that outlives the palette, and this one calls
// ACAPI — leaving it armed after the add-on unloads calls into freed code.
// ArchVizPanel::StopRenderer does that for both renderers.
//
// ⚠️ IT IS RENDERER-AGNOSTIC ON PURPOSE. Both the bgfx viewer and the Diligent
// viewport publish `pickSeq`/`pickedGuid`, only one can be open at a time, and
// the bridge reads whichever is running. A second copy for the Diligent path
// would have to be taught the owner walk below separately, and that is exactly
// the pair that drifts.
//
// ⚠️ THE OWNER WALK AND THE PART EXPANSION ARE NOT SYMMETRY FOR ITS OWN SAKE —
// they are the fix for the reported "picking does not work with column, railing,
// curtain wall or stairs". Those five are the hierarchical types: the 3D model
// enumerates their SUB-PARTS, so a GPU pick can only return a sub-part guid,
// which Archicad refuses to select; and Archicad selects the OWNER, whose guid
// matches nothing in the scene. `GeometryExtractor` owns both conversions.
//
// MAIN THREAD ONLY, all of it.

namespace geomsrv {
namespace archviz {
namespace selectionbridge {

// The two directions are SEPARATELY switchable, because they are separately
// wanted: a user inspecting the model wants Archicad's selection to light up in
// the viewer without the viewer touching their selection, and a user picking in
// the viewer wants the opposite. Bit flags, so "both ways" is one value rather
// than a fourth enum case to keep in step.
enum Flags {
    Off = 0,
    ToArchicad = 1,   // a click in the viewer selects in Archicad
    ToViewer = 2,     // Archicad's selection tints in the viewer
    Both = 3,
};

// Arm the bridge (idempotent) and set which directions are live. `Off` stops it.
// Returns whether the timer is running.
bool Start (int mode);
void Stop ();

// Which directions are live RIGHT NOW; `Off` when the timer is not armed.
int Mode ();

}   // namespace selectionbridge
}   // namespace archviz
}   // namespace geomsrv

#endif
