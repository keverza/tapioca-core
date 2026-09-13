#ifndef EVP_ARCHVIZ_AUTOORBIT_HPP
#define EVP_ARCHVIZ_AUTOORBIT_HPP

// Turn Archicad's own 3D camera at a fixed rate, from the camera-sync tick
// (PLAT-RE153, docs/architecture/api/HANDOFF-OverlayPatch.md stage 1).
//
// WHY IT EXISTS. Stage 1 asks what the GPU-state hooks cost Archicad, and three
// live runs of the SAME BUILD on the SAME MACHINE answered +25%, -13% and +18%.
// The phases were paced by a hand on a mouse, so the measurement could not
// resolve anything smaller than how hard it was being moved -- and a rung whose
// gate is "no collapse" was being asked to defend percentages it had no right
// to. A phase that issues a fixed number of fixed-size camera changes is the
// same amount of work every time and in every phase, which is the only way a few
// percent of frame cost can be seen at all.
//
// ⚠️ WHY IT IS NOT A LOOP IN THE DIAGNOSTIC, WHICH IS WHERE IT STARTED. The
// first version drove `Tapioca.Set3DProjection` from Python at 20 Hz. Every step
// was a synchronous round trip to the main thread, so a twelve-second phase
// managed less than half the frames a hand does, one phase overran to sixteen
// seconds, and the motion was jerky enough that the user reported the view had
// not orbited at all and orbited it themselves -- which put a hand back in the
// measurement it was built to remove. Stepping from the tick that is already
// running costs no round trip and cannot drift.
//
// ⚠️ AND WHY IT IS NOT A BLOCKING LOOP ON THE MAIN THREAD. Holding the main
// thread for twelve seconds stops Archicad's message loop, so it would redraw
// nothing and the phase would measure an idle application very precisely.
//
// ⚠️ THIS IS THE USER'S OWN 3D VIEW AND THERE IS NO UNDO FOR A VIEW SETTING.
// `Set3DProjection`'s own header says so. `Start` saves the WHOLE
// `API_3DProjectionInfo` and `Stop` writes it back verbatim -- not field by
// field, which is how the first version lost `viewCone` and left the overlay
// drawing at a different field of view from the model it was supposed to be
// tracking. The save lives in the add-on rather than in the diagnostic's
// `finally` on purpose: after a Stop the bus refuses the very calls that block
// would make, and a cancelled run must not be able to leave the view rotated.

#include <cstdint>

namespace GS {
class UniString;
}

namespace geomsrv {
namespace archviz {
namespace autoorbit {

// MAIN THREAD. Save the current projection and begin turning by
// `degreesPerStep` on every camera-sync tick. Idempotent: a second Start while
// running only changes the rate, so the saved projection is never overwritten
// with an already-rotated one.
//
// Refuses an axonometric window by name: `azimuth` exists there but the rest of
// the perspective fields do not, and stage 3 has no camera to score against in
// one anyway.
bool Start (double degreesPerStep, GS::UniString& error);

// MAIN THREAD. Stop, and put the projection back exactly as it was. Idempotent,
// and safe to call when it was never started.
void Stop ();

// MAIN THREAD, from the camera-sync tick. One step, or nothing if not running.
void StepIfRunning ();

bool     IsRunning ();
uint64_t StepsTaken ();

}   // namespace autoorbit
}   // namespace archviz
}   // namespace geomsrv

#endif
