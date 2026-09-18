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
// ⚠️ RAISED AFTER RUN THIRTY-NINE OVERFLOWED IT BY 4012 DRAWS. The cause was a
// key that fragmented, and that is fixed; the headroom is so the next surprise
// reports itself as a few dropped draws rather than a table of 2% coverage.
constexpr size_t kGroupCapacity = 48;
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

// ⚠️ THE CAMERA IDENTITY IS ATOMIC: SIGNATURE **AND** OCCURRENCE, SCORED
// TOGETHER. Choosing a group first and then inspecting its occurrences is what
// run thirty-eight got wrong: a group's aggregate mixes six occurrences, five of
// which project nothing, so the aggregate can rank well while every occurrence
// inside it fails -- and the phase-A winner then had 2% of its anchors inside
// the clip volume. There is no group winner before this table any more.
struct Group {
    // ⚠️ RUN-LOCAL, AND THAT IS ALL A GROUP IDENTITY MAY BE. It exists so every
    // injection row can name the group its camera came from; it means nothing in
    // the next session and is never written anywhere that outlives one.
    uint32_t groupId = 0;

    // Which draw of this signature within the model frame. Reset to zero when the
    // model generation changes; `firstConstant` is never used to derive it,
    // because Archicad's ring window advances every frame by design.
    uint32_t occurrenceIndex = 0;

    // ---- the signature, which is what makes this a group ------------------
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    float viewportX = 0.0f, viewportY = 0.0f;
    float viewportWidth = 0.0f, viewportHeight = 0.0f;
    uint64_t viewBuffer = 0;
    uint32_t viewNumConstants = 0;
    uint64_t projectionBuffer = 0;
    uint32_t projectionNumConstants = 0;

    // ---- where it sits in the frame ---------------------------------------
    uint64_t firstPresent = 0, lastPresent = 0;
    uint64_t passGeneration = 0, targetEpoch = 0;
    uint64_t drawSequenceFirst = 0, drawSequenceLast = 0;
    uint64_t drawsObserved = 0;
    uint64_t framesObserved = 0; // Presents. Informational only.

    // ⚠️ THE DENOMINATOR THAT MATTERS IS MODEL FRAMES, NOT PRESENTS. Archicad
    // presents constantly without re-rendering the model -- a UI repaint, a
    // cursor, a palette -- so dividing by Presents understated the real model
    // camera at 61% when it was present on 259 of 267 model frames, and let a
    // group seen ONCE outrank it. A camera group can only appear on a frame
    // where the model was drawn.
    uint64_t modelFramesObserved = 0;
    uint32_t drawKindMask = 0;
    uint32_t lastIndexCount = 0;

    // ⚠️ WHAT THE TARGETS ARE, BESIDE WHAT THEY ARE CALLED. The
    // pointers above name this session's views; these describe the textures
    // behind them, and they are the only part of a target identity that survives
    // Archicad rebuilding its views. See `Fingerprint`.
    uint32_t renderTargetWidth = 0, renderTargetHeight = 0;
    uint32_t renderTargetFormat = 0, renderTargetSamples = 0;
    bool depthPresent = false;
    uint32_t depthWidth = 0, depthHeight = 0;
    uint32_t depthFormat = 0, depthSamples = 0;
    uint32_t viewFirstConstant = 0; // last seen; the ring window advances
    uint32_t projectionFirstConstant = 0;

    // ⚠️ RECORDED, NOT USED. See the header note on `b0`.
    bool b0Bound = false;
    uint64_t b0Buffer = 0;
    uint32_t b0FirstConstant = 0;
    uint32_t b0NumConstants = 0;

    // ---- what its bytes actually project to -------------------------------
    uint32_t samplesScored = 0;
    // How many of those produced a best VALID variant, which is the denominator
    // the centre-error gate uses. Folded in by `CopyGroups`.
    uint32_t errorSamples = 0;
    uint32_t winningVariant = 0;
    uint32_t winningVariantValid = 0; // samples where THAT variant was valid

    // ⚠️ THE HARD METRICS, MEASURED ON THE WHOLE TRIANGLE AT INTERPRETATION 0.
    // The transform question is settled; these answer whether THIS draw carries
    // the model camera, and a candidate is invalid if the triangle collapses
    // however perfectly its anchor sits at the centre.
    uint32_t anchorInside = 0;    // samples whose anchor was inside clip
    uint32_t trianglesFinite = 0; // ... and all three vertices finite
    float medianAreaPixels = 0.0f;
    float medianMaxEdgePixels = 0.0f;
    float medianCentreError = 0.0f; // over the best valid variant per sample
    float worstCentreError = 0.0f;
    float meanCentreError = 0.0f;

    // ⚠️ HOW BIG THE PRIMITIVE IS ON SCREEN, which is what separates a transform
    // from a transform that has collapsed. A two-metre triangle rendering as one
    // pixel was invisible for six runs while every other number looked perfect.
    float meanSpreadPixels = 0.0f;
    uint32_t variantValid[kVariantCount] = {};
};

struct Stats {
    uint64_t drawsSeen = 0;
    uint64_t drawsQualified = 0;
    uint64_t framesSeen = 0;      // Presents
    uint64_t modelFramesSeen = 0; // ⚠️ the coverage denominator
    uint32_t groupsUsed = 0;
    uint64_t groupsOverflowed = 0;
    uint64_t copiesIssued = 0;
    uint64_t readbacksServed = 0;
    uint64_t readbacksBusy = 0;
    bool enabled = false;
    bool ready = false;

    // ⚠️ THE FOUR NUMBERS THAT SAY WHETHER THE CAMERA SURVIVED THE
    // PHASE CHANGE, and run forty-five had no way to ask. `selectionMatches`
    // counts draws that matched the PINNED pointers, `logicalMatches` draws that
    // matched the FINGERPRINT, and `rebinds` how often the pin was re-acquired
    // from a logical match. `logicalMatches > 0` with `rebinds == 0` would mean
    // the fingerprint found the camera and the rebind rule refused it; both at
    // zero means the fingerprint itself is wrong.
    uint64_t selectionMatches = 0;
    uint64_t logicalMatches = 0;
    uint64_t rebinds = 0;
    uint64_t rebindsRefused = 0;
    uint64_t resizeRebinds = 0;      // see CameraRecognizer::BindingStats
    uint64_t modelEditRebinds = 0;   // see CameraRecognizer::BindingStats
    uint64_t modelEditReselects = 0; // see CameraRecognizer::BindingStats
    uint32_t lastMissMask = 0;       // see CameraRecognizer::BindingStats
    bool fingerprintValid = false;
    // How many times the census chose for itself. See SetAutoSelect: nonzero
    // means production locked without anyone performing a measurement.
    uint64_t autoSelections = 0;
    // See `CameraRecognizer::EligibleCandidates`.
    uint32_t eligibleCandidates = 0;
    // ⚠️ HOW MANY TIMES THE GATE WAS EVEN RUN. `eligible == 0` with
    // `attempts == 0` is not a gate refusal, it is a selection that was never
    // attempted -- and those are different faults that looked identical.
    uint64_t autoSelectAttempts = 0;

    // ⚠️ THE LOCK AS A STATE: Unknown / Learning / Locked / Reacquiring.
    // See `CameraRecognizer::Lifecycle`. One bit -- "did it survive" -- cannot
    // tell "never found" from "found and lost" from "found, the view was
    // rebuilt, and it is already coming back", and only the last is ordinary.
    const char* lifecycle = "Unknown";
};

// MAIN THREAD. Off by default. Observation only: arming this draws nothing.
void SetEnabled (bool enabled);
bool Enabled ();

// MAIN THREAD. Clear the table. Called immediately before the gesture, so the
// census describes the gesture and nothing before it.
void Reset ();

// MAIN THREAD. The primitive every group is scored against: during an orbit its
// anchor is the orbit target, and its SIZE matters as much as its position --
// the scorer projects the whole triangle, because a transform that collapses it
// to a point passes any test that looks at one corner. See
// `InjectionOracle::VariantScore::spreadPixels`.
void SetAnchor (float x, float y, float z, float sizeMetres);

// RENDER THREAD, from every draw detour, before the call is forwarded. Admits
// the draw if it binds both camera windows; ignores it otherwise.
void OnDraw (ID3D11DeviceContext* context, DrawKind kind, uint32_t indexCount);

// MAIN THREAD, at teardown. Nothing here may outlive Archicad's device.
void Shutdown ();

// MAIN THREAD. Clear the measurements but KEEP the selection AND THE
// FINGERPRINT, so phase B can carry on scoring the group it locked on to.
//
// ⚠️ WHAT SURVIVES THIS CALL IS NOT AN IMPLEMENTATION DETAIL, IT IS
// THE CONTRACT. Statistics, groups, signature counters and readback slots are
// cleared; the fingerprint, the occurrence and the interpretation are not. A
// reset that dropped the fingerprint would throw away the one thing phase A
// exists to produce, and phase B would have to relearn a settled question.
void ResetCounts ();

// MAIN THREAD. Phase A's decision, forwarded to `CameraRecognizer` with a copy
// of the measured table. Fails closed: no eligible group means no selection.
bool SelectCandidate ();

// MAIN THREAD. Let the census choose for itself, and choose again if the choice
// is ever lost.
//
// ⚠️ THIS IS WHAT REMOVES THE TWELVE-SECOND CEREMONY FROM PRODUCTION.
// The diagnostic orbits for a fixed interval, prints a ranking and calls
// `SelectCandidate` by hand, because a REGRESSION test must show its working.
// A user opening an overlay must not be asked to perform a measurement: the
// census already scores every group on every model frame, so the moment one of
// them clears the eligibility gate there is nothing left to wait for.
//
// ⚠️ AND IT IS WHAT MAKES THE LOCK SELF-HEALING. A selection that is
// lost -- a 3D window recreated, a projection changed, a device replaced -- is
// simply an absent selection again, and this re-runs. `MaintainBinding` already
// handles the easier case where the fingerprint still matches and only the
// pointers moved; this covers the case where even the fingerprint is gone.
void SetAutoSelect (bool enabled);
bool AutoSelect ();

// ANY THREAD.
size_t CopyGroups (Group* out, size_t capacity);
Stats GetStats ();

// ⚠️ CHOOSING AND RE-FINDING THE CAMERA LIVE IN `CameraRecognizer`,
// AND THE SEAM IS NOT THE LINE COUNT. This file answers "what draw groups are
// there and what do their bytes project to"; that one answers "which of them is
// the camera, and where did it go when Archicad rebuilt its views". Run
// forty-five was a failure entirely within the second question while the first
// was never in doubt -- the census measured the camera correctly in both phases
// and the recognizer could not connect them.

} // namespace census
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
