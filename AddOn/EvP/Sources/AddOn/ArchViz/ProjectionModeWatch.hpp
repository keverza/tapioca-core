#ifndef EVP_ARCHVIZ_PROJECTIONMODEWATCH_HPP
#define EVP_ARCHVIZ_PROJECTIONMODEWATCH_HPP

// ArchViz/ProjectionModeWatch -- a MEASUREMENT (2026-10-02): what the 3D overlay sees when
// Archicad's projection mode changes, two-point perspective above all.
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md BEFORE EDITING. This
// reads and says; it changes no camera, no pin and no selection (§9: nothing depends on it).
//
// Two-point perspective is a still mode: Archicad leaves it when the view is navigated. Over
// a session spent toggling it (2026-10-02 13:46-13:49) the overlay held its camera Locked and
// did not follow, and nothing logged could say why: the census's matrix ledger had filled its
// 48 rows by 13:46:19, so a refused pair could no longer be shown, and `FRESH_NOT_ADOPTED`
// also counts a readback that merely lags the snapshot. Which of three it is decides the
// repair, and each is visible here:
// - the camera Archicad draws a two-point image with does not DECODE (finding 1 refuses it --
//   its view axis level in `b0` and tilted in `b1`, say), and the census reads it but cannot
//   take it;
// - it decodes, but the draw it comes from is not the PINNED one (another occurrence, target,
//   depth or viewport), so no snapshot is taken;
// - the model's camera does not change at all, and the two-point image is made after it.
//
// So, read every tick while the 3D window is in front: `ACAPI_View_Get3DProjectionSets`'s
// `isPersp` and `isTwoPointPersp`. On a change, say so with the settings, then for a few
// seconds say every camera the census reads -- the selected group's and the any-group one the
// anchor learns from -- as `camerashape::Of` describes it, the first with its raw floats, and
// finally what moved meanwhile: model frames, snapshots, adoption, the pin's terms, and the
// census groups that drew.
//
// MAIN THREAD.

namespace geomsrv {
namespace archviz {
namespace projectionmodewatch {

void Tick (bool threeDInFront);

// §8: what one session learned is not the next one's baseline.
void Reset ();

} // namespace projectionmodewatch
} // namespace archviz
} // namespace geomsrv

#endif
