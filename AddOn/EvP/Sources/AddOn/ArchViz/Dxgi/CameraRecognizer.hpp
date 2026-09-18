#ifndef EVP_ARCHVIZ_DXGI_CAMERARECOGNIZER_HPP
#define EVP_ARCHVIZ_DXGI_CAMERARECOGNIZER_HPP

// WHICH measured draw group is the model camera, and HOW TO FIND IT AGAIN
// (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ SPLIT OUT OF `CameraCensus` ALONG THE SEAM RUN FORTY-FIVE CUT.
// The census measures: it groups every draw that binds both camera windows, and
// scores each group's own bytes against the orbit-target invariant. It did that
// correctly in phase A and again in phase B of that run. What failed was
// everything in THIS file -- the decision was recorded as a set of COM addresses,
// Archicad rebuilt its render target and depth views between the two phases, and
// the camera the census could still see was one no recognizer could name.
//
// Measuring and recognising are different questions with different failure
// modes, and every failure since run forty has been in the second.
//
// ⚠️ THIS IS ALSO THE SHAPE PRODUCTION NEEDS. The census, the oracle
// and the probes are discovery machinery that will leave the hot path; a logical
// recognizer, a camera snapshot and a draw are what stay.
//
// THREAD SAFETY. `MatchesSelection`, `MatchesSelectionSignature` and
// `MaintainBinding` run on Archicad's render thread from the census's draw
// detour, under `ScopedInjectionGuard`. `SelectCandidate`, `ClearSelection` and
// the getters are main thread. Nothing here owns a device object, so nothing
// here can outlive Archicad's device.

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/ContextStateTracker.hpp"

#include <cstddef>
#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace census {

// ⚠️ WHAT A GROUP MUST CLEAR BEFORE IT MAY BE RANKED AT ALL. Ranking without a
// gate is what let a group with ONE sample and no coverage come first, ahead of
// a group that carried 1497 draws across 259 model frames at a median of 0.002.
// A rank is an ordering among candidates; it is not a test of candidacy.
struct Eligibility {
    uint32_t minSamples = 32;
    float minModelCoverage = 0.80f;
    float minInsideClip = 0.95f;

    // ⚠️ THE GATES A COLLAPSE CANNOT PASS. Centre error is now a weak ranking
    // term; these two are the discriminators. A two-metre triangle that renders
    // under ten pixels across, or encloses almost no area, is not the model
    // camera whatever its anchor does.
    float minFiniteTriangles = 0.95f;
    float minMedianAreaPixels = 50.0f;
    float minMedianMaxEdgePixels = 10.0f;

    // ⚠️ A QUARTER OF THE HALF-EXTENT, NOT A TWENTIETH, AND THE OLD NUMBER WAS
    // CALIBRATED AGAINST A LIE. 0.05 was set while the winning interpretation
    // collapsed the primitive to a point -- and a transform that maps all of
    // space to the viewport centre scores essentially ZERO on this metric. So
    // the tight threshold was actively selecting FOR degeneracy: run
    // thirty-seven rejected the real model camera at 0.069 (96% coverage, 99%
    // inside the clip volume, 2089 draws) while the collapsing transform had
    // sailed through at 0.002 for six runs.
    //
    // The orbit target is set by a human hand on a mouse and is not the anchor
    // to the pixel; a few percent of the viewport is what an honest transform
    // looks like. The discriminating work is done by `minInsideClip` and by the
    // spread gate in the scorer, both of which a degenerate transform fails.
    // ⚠️ AND IT IS 0.60, NOT 0.25, BECAUSE THIS IS THE WEAK TERM AND THE HAND ON
    // THE MOUSE IS NOT A ROBOT. Run thirty-nine's candidate was a genuinely
    // correct camera -- 100% of anchors inside the clip volume, 100% finite
    // triangles, a 1303 px2 primitive with a 70 px longest edge -- rejected at
    // 0.275. The orbit target is wherever the user last clicked, not the anchor
    // to the pixel. Off-screen is beyond 1.0, so 0.60 still refuses a wrong
    // camera while the area, edge and inside-clip gates do the real work.
    float maxMedianCentreError = 0.60f;
    // ⚠️ THE WINNING INTERPRETATION MUST HOLD ACROSS THE SAMPLES, not merely win
    // once: this is the fraction of scored samples on which THAT interpretation
    // produced a valid projection. By construction it is the same number as the
    // inside-clip rate, and it is stated separately because they are different
    // claims -- "the anchor was on screen" and "the same reading of the bytes
    // kept putting it there".
    float minInterpretationAgreement = 0.90f;
};

// The camera group this run has committed to. ⚠️ RUNTIME IDENTITIES ONLY, AND
// THEY ARE NEVER WRITTEN TO THE BUILD PROFILE. A render-target or buffer pointer
// is a live COM address: it survives a gesture and not a resize, a device reset
// or a new session. Proving one group is what this is for; the eventual
// production recognizer has to learn a LOGICAL signature each session -- full
// viewport, camera window shape, draw and pass position, resource descriptions --
// and this is deliberately not that.
struct Selection {
    bool valid = false;
    uint32_t groupId = 0;
    uint32_t occurrenceIndex = 0;
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    float viewportX = 0.0f, viewportY = 0.0f;
    float viewportWidth = 0.0f, viewportHeight = 0.0f;
    // See `MatchesSelection`: the surfaces are identified by description,
    // because their pointers change between frames.
    uint32_t renderTargetWidth = 0, renderTargetHeight = 0, renderTargetFormat = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthFormat = 0;
    uint64_t viewBuffer = 0;
    uint32_t viewNumConstants = 0;
    uint64_t projectionBuffer = 0;
    uint32_t projectionNumConstants = 0;

    // What it scored when it was chosen, so the report can state the evidence
    // rather than the decision alone.
    uint32_t variant = 0;
    uint32_t samples = 0;
    float modelCoverage = 0.0f;
    float insideClip = 0.0f;
    float medianCentreError = 0.0f;
    float medianAreaPixels = 0.0f;
    float medianMaxEdgePixels = 0.0f;
    uint64_t snapshotsTaken = 0; // draws matched since the lock
};

// ⚠️ THE CAMERA IDENTITY THAT OUTLIVES ARCHICAD'S RESOURCES, AND
// RUN FORTY-FIVE IS THE ENTIRE ARGUMENT FOR IT. Phase A selected a group scoring
// 91% model-frame coverage, 100% of anchors inside the clip volume and a median
// of 0.051 over 626 draws -- an unambiguous camera. Phase B then reported ZERO
// matching draws, and its own census showed why: the same shaders, the same
// 3432x1803 viewport, the same six-index draw, through NEW render-target and
// depth-view addresses. Archicad had rebuilt its views between the two phases.
//
// So `Selection` above -- pointers, buffers, a run-local group id -- is a
// BINDING, valid only while those objects live. This is the IDENTITY, and it is
// deliberately made of nothing that can be freed:
//
//     the viewport rectangle
//     the draw kind and its index count
//     whether a depth view is bound at all
//     the render target's and depth buffer's width, height, format, samples
//     the two camera constant windows' sizes
//     which occurrence within the model frame
//     the matrix interpretation that was learned
//
// ⚠️ THE VERTEX SHADER IS NOT IN IT, for the reason it is not in the
// group key: run thirty-nine proved Archicad shares one camera across many
// shaders. ⚠️ NEITHER IS THE DRAW ORDINAL, and that one is measured
// rather than assumed -- phase A recorded ordinal 6..6 for the selected group
// and phase B recorded 0..6 for the same draw family, so an ordinal match would
// have failed for a camera that was present the whole time. It is RECORDED, so
// the report can show the pattern, and kept out of the predicate.
struct Fingerprint {
    bool valid = false;
    uint32_t occurrenceIndex = 0;
    float viewportX = 0.0f, viewportY = 0.0f;
    float viewportWidth = 0.0f, viewportHeight = 0.0f;
    uint32_t drawKindMask = 0;
    uint32_t indexCount = 0;
    uint32_t viewNumConstants = 0;
    uint32_t projectionNumConstants = 0;
    bool depthPresent = false;
    uint32_t renderTargetWidth = 0, renderTargetHeight = 0;
    uint32_t renderTargetFormat = 0, renderTargetSamples = 0;
    uint32_t depthWidth = 0, depthHeight = 0;
    uint32_t depthFormat = 0, depthSamples = 0;
    uint32_t variant = 0;

    // Recorded, not matched on. See the note above.
    uint32_t drawOrdinalFirst = 0, drawOrdinalLast = 0;
};

// ANY THREAD. What phase A committed to, in terms that survive phase B.
Fingerprint GetFingerprint ();

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

// MAIN THREAD. Choose the best ELIGIBLE group from a copied table and lock on to
// it. Returns false and changes nothing when none qualifies.
bool SelectCandidate (const Group* groups, size_t count, uint64_t modelFrames);
void ClearSelection ();

// MAIN THREAD, from the runtime heartbeat: the model revision Archicad is at.
//
// ⚠️ `indexCount` IS A FINGERPRINT TERM AND A MODEL EDIT
// CHANGES IT. Creating a slab froze the overlay outright: the selected draw's
// index count moved, `MatchesFingerprint` missed on `kTermIndexCount` forever
// (`miss=0x09`), no draw could ever rebind, and Present kept composing with the
// last accepted camera -- 110 model frames stale by the end. New geometry still
// appeared, because extraction is a different service, so the overlay looked
// alive and was not.
//
// This is the evidence that separates "the geometry changed" from "this is a
// different camera". Without it the index count cannot be adopted safely,
// because a DIFFERENT DRAW OF THE SAME FAMILY also differs only in index count.
void NoteModelRevision (uint32_t revision);
Selection GetSelection ();
Fingerprint GetFingerprint ();
Eligibility GetEligibility ();

// MAIN THREAD. Count a snapshot taken from the selected group.
void NoteSnapshot ();

// ---------------------------------------------------------------------------
// Finding it again
// ---------------------------------------------------------------------------

// RENDER THREAD. The pinned resources, tested against what is bound right now.
bool MatchesSelectionSignature (const contextstate::ContextState& live);
bool MatchesSelection (const contextstate::ContextState& live, uint32_t occurrence);

// RENDER THREAD, from every qualifying draw, BEFORE anything reads the
// selection. Re-acquires the runtime resources when the pin has gone stale and
// a draw matches the fingerprint. ⚠️ IT CANNOT CHOOSE A DIFFERENT
// CAMERA -- only move the one already chosen onto the resources now carrying it.
void MaintainBinding (const contextstate::ContextState& live, DrawKind kind, uint32_t indexCount, uint32_t occurrence,
                      uint64_t modelGeneration);

// ⚠️ WHETHER THE CAMERA SURVIVED THE PHASE CHANGE, WHICH RUN
// FORTY-FIVE HAD NO WAY TO ASK. `selectionMatches` counts draws that matched the
// PINNED pointers, `logicalMatches` draws that matched the FINGERPRINT, and
// `rebinds` how often the pin was re-acquired from a logical match.
// `logicalMatches > 0` with `rebinds == 0` means the fingerprint found the
// camera and the rebind rule refused it; both at zero means the fingerprint
// itself is wrong.
// ⚠️ A FINGERPRINT THAT MATCHES NOTHING MUST SAY WHICH TERM
// REFUSED, OR IT COSTS A WHOLE RUN TO FIND OUT. `logicalMatches == 0` is the
// failure this file exists to prevent and also the least informative thing it
// could report: eight terms, one verdict, and no way to tell "the depth format
// changed" from "the view was never navigated".
//
// So every term is evaluated on every qualifying draw and the failures are
// counted apart. `missed` is how often a term disagreed at all -- mostly noise,
// because thousands of unrelated draws disagree on the viewport. `soleMiss` is
// the diagnosis: how often a draw matched the fingerprint in EVERY term but
// this one. A non-zero `soleMiss` names the field to look at, and `observed`
// carries the first such draw's value beside the fingerprint's.
enum FingerprintTerm : uint32_t {
    kTermOccurrence = 0,
    kTermViewport,
    kTermDrawKind,
    kTermIndexCount,
    kTermCameraWindows,
    kTermDepthPresence,
    kTermRenderTargetDesc,
    kTermDepthDesc,
    kFingerprintTermCount,
};

struct FingerprintDiagnosis {
    uint64_t evaluated = 0;
    uint64_t missed[kFingerprintTermCount] = {};
    uint64_t soleMiss[kFingerprintTermCount] = {};

    // The first sole-miss on each term, so a mismatch can be read rather than
    // guessed at. Four numbers is enough for every term here: a viewport, a
    // description, a window pair, an occurrence.
    uint32_t observed[kFingerprintTermCount][4] = {};
    bool sampled[kFingerprintTermCount] = {};
};
FingerprintDiagnosis GetFingerprintDiagnosis ();

// ⚠️ THE LOCK IS A STATE, NOT AN EVENT, AND SAYING SO IS THE POINT.
// Every run so far has asked "did phase A's camera survive?" at the end and got
// one bit back, which cannot distinguish "never found" from "found and lost" from
// "found, the view was rebuilt, and it is coming back" -- the last being ORDINARY
// and already handled by `MaintainBinding`. A resize, a device reset and a window
// rebuild all pass through `Reacquiring` and back to `Locked` on their own.
//
// Unknown      no camera has ever been selected
// Learning     the census is scoring candidates; none chosen yet
// Locked       a selection exists and its pinned resources are drawing
// Reacquiring  a selection exists, the pin has gone quiet, the fingerprint holds
enum class Lifecycle { Unknown, Learning, Locked, Reacquiring };

// ANY THREAD. `learning` is the census's own enabled state; this file does not
// own it, so the caller passes it rather than this reaching across for it.
Lifecycle GetLifecycle (bool learning, uint64_t modelGeneration);
const char* LifecycleName (Lifecycle state);

struct BindingStats {
    uint64_t selectionMatches = 0;
    uint64_t logicalMatches = 0;
    uint64_t rebinds = 0;
    uint64_t rebindsRefused = 0;
    // ⚠️ HOW OFTEN A RESIZE WAS ADOPTED IN PLACE. The window
    // size is part of the fingerprint, so a resize makes the identity
    // unmatchable; the repair is to take the new size into the fingerprint and
    // keep the camera, NOT to relearn. Nonzero is healthy after a resize;
    // growing while the window is still is not.
    uint64_t resizeRebinds = 0;
    // ⚠️ HOW OFTEN A MODEL EDIT WAS ADOPTED IN PLACE. Nonzero
    // after creating or deleting geometry is healthy; growing while the model is
    // untouched means something else is moving the index count.
    uint64_t modelEditRebinds = 0;
    // ⚠️ HOW OFTEN AN EDIT MOVED THE OCCURRENCE AND THE
    // CAMERA WAS RE-CHOSEN RATHER THAN GUESSED. Nonzero after creating or
    // deleting geometry is healthy. Climbing while the model is untouched means
    // the model-edit rule is firing on navigation and must be WITHDRAWN.
    // ⚠️ HOW OFTEN AN EDIT SILENCED THE LOCKED OCCURRENCE
    // LONG ENOUGH TO REPLACE IT. This is a REPLACEMENT, not a relearn: the
    // fingerprint, the group and the interpretation all survive. Climbing while
    // the model is untouched means the model-edit rule is firing on navigation
    // and must be withdrawn.
    uint64_t modelEditReselects = 0;
    // The terms the last evaluated draw missed, one bit per fingerprint term, so
    // a refusal to adopt can be read rather than inferred.
    uint32_t lastMissMask = 0;
    // ⚠️ WHY THE PIN FAILED, PER TERM. `selectionMatches`
    // counts the successes and said nothing about the two thirds that are not.
    // Order is `PinTerm` in CameraRecognizer.cpp.
    uint32_t pinMissMask = 0;
    uint64_t pinMissed[8] = {};
    bool fingerprintValid = false;
};
BindingStats GetBindingStats ();

// ANY THREAD. How many groups cleared the eligibility gate on the last selection
// attempt. ⚠️ A PROMOTION THAT NEVER HAPPENS IS TWO DIFFERENT FAULTS
// AND THIS TELLS THEM APART: zero means no candidate qualified, so the gate or
// the scoring anchor is the subject; more than zero with no selection means the
// commit refused.
uint32_t EligibleCandidates ();

// ⚠️ WHICH TERM OF THE GATE REFUSED, AND WHAT THE CLOSEST CANDIDATE
// ACTUALLY MEASURED. `Qualifies` short-circuits, so it can only ever report the
// FIRST term that failed -- which for a camera still accumulating samples is
// always `samples` and never the interesting one. This is the same lesson
// `MatchesFingerprint` learned: evaluating every term costs a handful of
// compares and buys a diagnosis that otherwise costs a run.
//
// Run sixty-six reported `eligible=0` at `frames=28` and could say nothing more.
enum GateTerm : uint32_t {
    kGateSamples = 0,
    kGateCoverage,
    kGateInsideClip,
    kGateFiniteTriangles,
    kGateAreaPixels,
    kGateEdgePixels,
    kGateCentreError,
    kGateTermCount,
};
const char* GateTermName (uint32_t term);

struct EligibilityDiagnosis {
    uint32_t evaluated = 0;
    uint32_t missed[kGateTermCount] = {};
    // ⚠️ ONLY A SOLE MISS IS EVIDENCE, as with the fingerprint: a
    // group failing six terms is some unrelated draw family; one failing exactly
    // one is the camera, and that term is the subject.
    uint32_t soleMiss[kGateTermCount] = {};

    // The candidate that came closest -- fewest failures, then most samples --
    // with the numbers it actually measured, so the gate can be argued with.
    bool haveClosest = false;
    uint32_t closestGroupId = 0;
    uint32_t closestFailures = 0;
    uint32_t closestSamples = 0;
    float closestCoverage = 0.0f;
    float closestInsideClip = 0.0f;
    float closestFinite = 0.0f;
    float closestAreaPixels = 0.0f;
    float closestEdgePixels = 0.0f;
    float closestCentreError = 0.0f;
};
EligibilityDiagnosis GetEligibilityDiagnosis ();

// MAIN THREAD, from `census::ResetCounts`. Clears the COUNTERS and the memory of
// where the pin was last seen. ⚠️ IT DOES NOT CLEAR THE FINGERPRINT
// OR THE SELECTION, and that is the contract phase B depends on.
void ResetBindingStats ();

// MAIN THREAD, at teardown. ⚠️ NAMED APART FROM
// `census::Shutdown`, which releases device objects: these are two different
// teardowns in one namespace and one name for both would be an ODR violation
// the linker would resolve silently and wrongly.
void ShutdownRecognizer ();

} // namespace census
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
