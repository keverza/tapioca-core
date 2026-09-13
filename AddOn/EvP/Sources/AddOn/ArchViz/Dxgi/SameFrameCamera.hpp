#ifndef EVP_ARCHVIZ_DXGI_SAMEFRAMECAMERA_HPP
#define EVP_ARCHVIZ_DXGI_SAMEFRAMECAMERA_HPP

// Stage 4: tie a captured view and a captured projection to ONE ARCHICAD SCENE
// PASS (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md stage 4).
//
// WHY IT IS ITS OWN FILE. `ViewMatrixCandidates` is stage 3's discovery scorer:
// it asks what the captured bytes COULD be, by scoring every block four ways and
// every pair four more. That question is answered and its code is frozen
// evidence. This file asks the production question -- which two of them describe
// the frame Archicad is drawing right now -- and it is the one that keeps
// changing. They were one file until 2026-09-13, when the size cap made the seam
// obvious; the interface between them is `SnapshotCameraBlocks`.
//
// ⚠️ A PRESENT INTERVAL IS NOT A RENDERING PASS, AND PAIRING ON ONE IS WRONG
// TWICE OVER. It asks the main thread to be in the same frame as the render
// thread, which it can never be -- run seventeen resolved 0 of 2720 attempts
// because the render thread had advanced the counter before the tick read it.
// And it would ALLOW a pairing that must be refused: a shadow pass, a preview, a
// thumbnail and the main pass can all live between two Presents, each with its
// own camera, so a view from one spliced to a projection from another is a
// plausible matrix that describes nothing. The identity is:
//
//     Present generation
//         scene-pass generation
//             viewport
//             RTV / DSV
//             view binding
//             projection binding
//             final scene draw
//
// ⚠️ AND THIS FILE REFUSES RATHER THAN GUESSES. A pair it cannot vouch for is a
// frame stage 5 skips. A skipped frame is invisible; a stale or spliced frame is
// the bug the whole rung exists to remove.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace viewmatrix {

struct ScenePassCamera {
    bool     valid = false;
    uint64_t scenePass = 0;      // the pass BOTH halves were bound in
    uint32_t agePasses = 0;      // how far behind the newest completed pass
    float    viewProj[16] = {};  // their product, ready to draw with
    float    view[16] = {};
    float    projection[16] = {};
    uint32_t viewBindSlot = 0;
    uint32_t projectionBindSlot = 0;
    uint64_t colorTarget = 0;    // what that pass drew into
    uint64_t depthTarget = 0;
};

// MAIN THREAD. The newest camera that can be attributed to a single completed
// scene pass, or an invalid one.
ScenePassCamera ResolveScenePassPair (uint32_t maxAgePasses);

struct PairingStats {
    uint64_t resolved = 0;
    uint64_t refusedNoView = 0;
    uint64_t refusedNoProj = 0;
    uint64_t refusedStraddle = 0;   // both halves present, different passes
    uint32_t lastAgePasses = 0;
    uint64_t lastScenePass = 0;
};
PairingStats GetPairingStats ();
void         ResetPairing ();

}   // namespace viewmatrix
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
