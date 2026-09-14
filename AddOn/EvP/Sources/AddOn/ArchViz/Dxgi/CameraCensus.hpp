#ifndef EVP_ARCHVIZ_DXGI_CAMERACENSUS_HPP
#define EVP_ARCHVIZ_DXGI_CAMERACENSUS_HPP

// Which draws in Archicad's frame carry the MODEL camera -- measured, grouped
// and ranked, rather than inferred from a learned pass signature (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ THIS EXISTS BECAUSE RUN THIRTY PROVED THE LEARNED MODEL PASS IS NOT ONE
// CAMERA. Inside a single orbit, with the same pass accepted by the same
// signature, the oracle's rows split cleanly into two populations:
//
//     anchor NDC (-0.10, -0.13, 0.697)   inside clip   2180 samples
//     anchor NDC (-1.0035, 1.006, 52.7)  OUTSIDE       0 samples
//
// A clip z of fifty-two is not a near miss and a convention error cannot be
// intermittent. So the learned pass contains at least two different camera-
// bearing draw groups, we have been snapshotting whichever drew last, and the
// answer is not a better guess about passes -- it is a census.
//
// ⚠️ SO THE LEARNER IS DEMOTED TO AN OBSERVER HERE, AND NOTHING IN THIS FILE
// CONSULTS IT. Every draw that binds both `b1` and `b2` as 256-byte windows is
// admitted, whatever pass the learner believes it belongs to, and the draws are
// then GROUPED BY WHAT THEY ARE rather than by where we think they are:
//
//     vertex shader + render target + depth view + viewport + window shape
//
// Each group is scored independently against the orbit-target invariant. The
// true model camera should then be obvious rather than argued: present on most
// moving frames, full viewport, anchor near the centre, and a stable winning
// interpretation.
//
// ⚠️ IT OBSERVES AND RANKS. IT DOES NOT INJECT. Nothing here changes what the
// triangle is drawn with; the recognizer is only re-pinned once a group has won
// on the numbers.
//
// ⚠️ `b0` IS RECORDED AND DELIBERATELY NOT USED. If a group has plausible x/y
// but impossible depth, an additional world or model transform in another
// constant is the first thing to suspect -- but suspecting it before the draw
// group is identified would be the same mistake in a new place. Identify the
// group first.
//
// THREAD SAFETY. `OnDraw` runs on Archicad's render thread inside a draw detour,
// under `ScopedInjectionGuard`, and never waits: the readback is
// `D3D11_MAP_FLAG_DO_NOT_WAIT` on a copy the GPU has already finished with, and
// a slot that is not ready is retried on a later draw. `CopyGroups` is the
// cross-thread read and it takes a copy.

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace census {

// ⚠️ THIRTY-TWO IS A CEILING, AND OVERFLOW IS REPORTED RATHER THAN WRAPPED. A
// census that silently dropped the group it was looking for would be worse than
// no census; `groupsOverflowed` says when the table filled.
constexpr size_t kGroupCapacity = 32;
constexpr size_t kVariantCount = 8;

enum class DrawKind : uint32_t {
    Indexed = 0,
    Direct = 1,
    IndexedInstanced = 2,
    Instanced = 3,
    Auto = 4,
    IndexedInstancedIndirect = 5,
    InstancedIndirect = 6,
};

struct Group {
    // ⚠️ RUN-LOCAL, AND THAT IS ALL A GROUP IDENTITY MAY BE. It exists so every
    // injection row can name the group its camera came from; it means nothing in
    // the next session and is never written anywhere that outlives one.
    uint32_t groupId = 0;

    // ---- the signature, which is what makes this a group ------------------
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    float    viewportX = 0.0f, viewportY = 0.0f;
    float    viewportWidth = 0.0f, viewportHeight = 0.0f;
    uint64_t viewBuffer = 0;
    uint32_t viewNumConstants = 0;
    uint64_t projectionBuffer = 0;
    uint32_t projectionNumConstants = 0;

    // ---- where it sits in the frame ---------------------------------------
    uint64_t firstPresent = 0, lastPresent = 0;
    uint64_t passGeneration = 0, targetEpoch = 0;
    uint64_t drawSequenceFirst = 0, drawSequenceLast = 0;
    uint64_t drawsObserved = 0;
    uint64_t framesObserved = 0;        // Presents. Informational only.

    // ⚠️ THE DENOMINATOR THAT MATTERS IS MODEL FRAMES, NOT PRESENTS. Archicad
    // presents constantly without re-rendering the model -- a UI repaint, a
    // cursor, a palette -- so dividing by Presents understated the real model
    // camera at 61% when it was present on 259 of 267 model frames, and let a
    // group seen ONCE outrank it. A camera group can only appear on a frame
    // where the model was drawn.
    uint64_t modelFramesObserved = 0;
    uint32_t drawKindMask = 0;
    uint32_t lastIndexCount = 0;
    uint32_t viewFirstConstant = 0;     // last seen; the ring window advances
    uint32_t projectionFirstConstant = 0;

    // ⚠️ RECORDED, NOT USED. See the header note on `b0`.
    bool     b0Bound = false;
    uint64_t b0Buffer = 0;
    uint32_t b0FirstConstant = 0;
    uint32_t b0NumConstants = 0;

    // ---- what its bytes actually project to -------------------------------
    uint32_t samplesScored = 0;
    uint32_t winningVariant = 0;
    uint32_t winningVariantValid = 0;   // samples where THAT variant was valid
    float    medianCentreError = 0.0f;  // over the best valid variant per sample
    float    worstCentreError = 0.0f;
    float    meanCentreError = 0.0f;
    uint32_t variantValid[kVariantCount] = {};
};

struct Stats {
    uint64_t drawsSeen = 0;
    uint64_t drawsQualified = 0;
    uint64_t framesSeen = 0;          // Presents
    uint64_t modelFramesSeen = 0;     // ⚠️ the coverage denominator
    uint32_t groupsUsed = 0;
    uint64_t groupsOverflowed = 0;
    uint64_t copiesIssued = 0;
    uint64_t readbacksServed = 0;
    uint64_t readbacksBusy = 0;
    bool     enabled = false;
    bool     ready = false;
};

// MAIN THREAD. Off by default. Observation only: arming this draws nothing.
void SetEnabled (bool enabled);
bool Enabled ();

// MAIN THREAD. Clear the table. Called immediately before the gesture, so the
// census describes the gesture and nothing before it.
void Reset ();

// MAIN THREAD. The world point every group is scored against -- during an orbit,
// the orbit target.
void SetAnchor (float x, float y, float z);

// RENDER THREAD, from every draw detour, before the call is forwarded. Admits
// the draw if it binds both camera windows; ignores it otherwise.
void OnDraw (ID3D11DeviceContext* context, DrawKind kind, uint32_t indexCount);

// MAIN THREAD, at teardown. Nothing here may outlive Archicad's device.
void Shutdown ();

// ⚠️ WHAT A GROUP MUST CLEAR BEFORE IT MAY BE RANKED AT ALL. Ranking without a
// gate is what let a group with ONE sample and no coverage come first, ahead of
// a group that carried 1497 draws across 259 model frames at a median of 0.002.
// A rank is an ordering among candidates; it is not a test of candidacy.
struct Eligibility {
    uint32_t minSamples = 32;
    float    minModelCoverage = 0.80f;
    float    minInsideClip = 0.95f;
    float    maxMedianCentreError = 0.05f;
    // ⚠️ THE WINNING INTERPRETATION MUST HOLD ACROSS THE SAMPLES, not merely win
    // once: this is the fraction of scored samples on which THAT interpretation
    // produced a valid projection. By construction it is the same number as the
    // inside-clip rate, and it is stated separately because they are different
    // claims -- "the anchor was on screen" and "the same reading of the bytes
    // kept putting it there".
    float    minInterpretationAgreement = 0.90f;
};

// The camera group this run has committed to. ⚠️ RUNTIME IDENTITIES ONLY, AND
// THEY ARE NEVER WRITTEN TO THE BUILD PROFILE. A render-target or buffer pointer
// is a live COM address: it survives a gesture and not a resize, a device reset
// or a new session. Proving one group is what this is for; the eventual
// production recognizer has to learn a LOGICAL signature each session -- full
// viewport, camera window shape, draw and pass position, resource descriptions --
// and this is deliberately not that.
struct Selection {
    bool     valid = false;
    uint32_t groupId = 0;
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    float    viewportX = 0.0f, viewportY = 0.0f;
    float    viewportWidth = 0.0f, viewportHeight = 0.0f;
    uint64_t viewBuffer = 0;
    uint32_t viewNumConstants = 0;
    uint64_t projectionBuffer = 0;
    uint32_t projectionNumConstants = 0;

    // What it scored when it was chosen, so the report can state the evidence
    // rather than the decision alone.
    uint32_t variant = 0;
    uint32_t samples = 0;
    float    modelCoverage = 0.0f;
    float    insideClip = 0.0f;
    float    medianCentreError = 0.0f;
    uint64_t snapshotsTaken = 0;    // draws matched since the lock
};

// MAIN THREAD. Phase A's decision: choose the best ELIGIBLE group and lock on to
// it. Returns false and changes nothing when none qualifies -- fail closed, so a
// run that could not identify the camera injects nothing rather than injecting
// with whatever came last.
bool SelectCandidate ();
void ClearSelection ();
Selection GetSelection ();

// MAIN THREAD. Clear the measurements but KEEP the selection, so phase B can
// carry on scoring the group it locked on to.
void ResetCounts ();

// ANY THREAD.
size_t CopyGroups (Group* out, size_t capacity);
Stats  GetStats ();
Eligibility GetEligibility ();

}   // namespace census
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
