#ifndef EVP_ARCHVIZ_OVERLAYCAMERAKEEP_HPP
#define EVP_ARCHVIZ_OVERLAYCAMERAKEEP_HPP

// ArchViz/OverlayCameraKeep -- the 3D overlay's camera, kept across a view change.
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md BEFORE EDITING.
// §3: a view rebuilt goes Locked -> Reacquiring -> Locked and is never relearned.
// §8: what one session keeps for the next is named here, checked at the next arm, and
// forgotten by every stop that is not a view change.
//
// ⚠️ A VIEW CHANGE SUSPENDS THE SESSION; IT DOES NOT KEEP IT RUNNING. The hooks come
// out as before -- left in, they would be the 3D session's hooks in the plan's frames
// and its Present (§12) -- and what is kept is what the session LEARNED, not anything
// it held: the camera's fingerprint, occurrence and interpretation (CameraRecognizer
// `KeptCamera`), the extent of its scene pass, and the window it was drawn in. The
// next arm commits the camera as Reacquiring, lets the first frame of that extent
// learn the scene signature, nominates the chain presenting into that window at its
// first Present, and asks for its frames a tick apart; the first model frame then
// rebinds it. Before: 3D -> plan -> 3D took 22 s and an orbit (the user, 2026-09-30
// 15:09:44-15:10:06).
//
// ⚠️ A WINDOW THAT IS GONE TAKES ITS CAMERA WITH IT. A 3D window closed while the plan
// was in front comes back as another window with its own chain; the kept camera is
// not offered to it, and one no draw matches is dropped by the recognizer.
//
// MAIN THREAD, every entry point.

#include "ArchViz/Dxgi/CameraRecognizer.hpp"

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {
namespace camerakeep {

struct Kept {
    dxgi::census::KeptCamera camera;
    float sceneWidth = 0.0f;
    float sceneHeight = 0.0f;
    uint64_t window = 0; // the HWND Archicad's 3D chain presented into
};

// What the running session learned, taken before it stops for a view change. Not
// valid unless a camera was selected on a known window.
Kept Take ();
// Held for the next arm.
void Hold (const Kept& kept);
// Every stop that is not a view change: the menu, a project event, the unload.
void Forget ();
// At the arm, after the census was reset and before the context hook is in: the held
// camera resumed if its window still exists. One use.
void Apply ();
// From the runtime's tick: a resumed camera that no draw matched, said once.
void Narrate ();

} // namespace camerakeep
} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv

#endif
