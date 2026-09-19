// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/Dxgi/CameraRecognizer -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/CameraRecognizer.hpp"

#include "ArchViz/Dxgi/InjectionCamera.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace census {

namespace {

Selection g_selection;
Eligibility g_eligibility;

// ⚠️ THE FINGERPRINT IS NOT PART OF THE MEASUREMENTS AND IS NEVER
// CLEARED WITH THEM. It is phase A's product; `ResetCounts` clears what phase B
// measures and leaves this alone. Only `ClearSelection` drops it.
Fingerprint g_fingerprint;

// ⚠️ WHEN THE PINNED RESOURCES WERE LAST SEEN ALIVE, which is what
// makes re-acquisition safe. A fingerprint match only re-pins when the current
// pin has gone quiet; without that rule a second draw family sharing the same
// logical shape would steal the binding back and forth every frame.
uint64_t g_selectionLastSeenModel = 0;

// ⚠️ HOW LONG THE CANDIDATES ARE WATCHED BEFORE ANY OF
// THEM IS CHOSEN. See `SelectCandidate`.
const uint64_t kCalibrationModelFrames = 96;
// Whether the committed pin was decided by a completed calibration. A pin taken
// before that is PROVISIONAL and will be re-decided; see below.
bool g_calibrated = false;
BindReport g_bind;
// How many groups cleared the eligibility gate on the last attempt. A promotion
// that never happens is a different fault depending on whether this is zero.
uint32_t g_eligibleCandidates = 0;
EligibilityDiagnosis g_gate;
// Which single term the last evaluated draw missed, or -1. See `SoleMissWasViewport`.
int g_lastSoleMiss = -1;
// Every term the last evaluated draw missed, one bit each. See `LooksLikeResize`.
uint32_t g_lastMissMask = 0;
// The model revision now, and the one the fingerprint was committed at. See
// `NoteModelRevision`.
uint32_t g_modelRevision = 0;
uint32_t g_fingerprintRevision = 0;
// The model generation at which an edit made the locked occurrence suspect, or 0.
// See `SuspectOccurrenceAfterModelEdit`.
uint64_t g_occurrenceSuspectSince = 0;

BindingStats g_binding;
FingerprintDiagnosis g_diagnosis;

bool SameExtent (float a, float b)
{
    return std::fabs (a - b) < 1.5f;
}

} // namespace

// The locked group's signature, tested against what is bound right now. Same
// fields as `Matches`, for the same reasons -- and `firstConstant` is absent from
// both because Archicad's ring window advances every frame by design.
// The signature alone, without the occurrence: the depth proof wants a
// different draw of the same family than the camera snapshot does.
bool MatchesSelectionSignature (const contextstate::ContextState& live)
{
    if (!g_selection.valid)
        return false;
    return g_selection.renderTarget == live.renderTarget && g_selection.depthStencil == live.depthStencil &&
           SameExtent (g_selection.viewportWidth, live.viewportWidth) &&
           SameExtent (g_selection.viewportHeight, live.viewportHeight) &&
           g_selection.viewBuffer == live.vsConstantBuffers[1].buffer &&
           g_selection.projectionBuffer == live.vsConstantBuffers[2].buffer;
}

// ⚠️ WHICH PIN TERM FAILED, BECAUSE THE PIN FAILS TWO
// FRAMES IN THREE AND NOBODY KNOWS WHY. A run measured `snapshots +60` against
// `frames +174` with the camera Locked and the occurrence never moving: the
// overlay is correct and a third of the time it is three or more frames old.
// `age3+ 38%` is the same fact seen from Present.
//
// ⚠️ AND THE OBVIOUS SUSPECT IS ALREADY CONTRADICTED.
// `MatchesSelection` compares both constant-buffer POINTERS, which
// OverlayGuidance section 3 lists as a runtime identity -- but
// `ContextHookDetours` records that Archicad 29 keeps its constants in ONE 8 MiB
// ring and binds windows of it, so the pointer is probably CONSTANT and the
// offset is what moves. Changing the predicate on that guess would be the third
// occurrence repair made by reasoning instead of measurement, and the first two
// each made it worse. So: count the terms, then decide.
enum PinTerm : uint32_t {
    kPinOccurrence = 0,
    kPinRenderTarget,
    kPinDepthStencil,
    kPinViewport,
    kPinWindows,
    kPinTermCount
};

bool MatchesSelection (const contextstate::ContextState& live, uint32_t occurrence)
{
    if (!g_selection.valid)
        return false;

    bool term[kPinTermCount];
    term[kPinOccurrence] = g_selection.occurrenceIndex == occurrence;
    // ⚠️ DESCRIPTIONS, NOT POINTERS, AND THE MEASUREMENT
    // IS WHY. The pin missed on `occurrence+renderTarget+depthStencil` and only
    // 64% of model frames produced a snapshot; the other 36% composed with an
    // older camera, which is `age3+` and the residual desync being reported.
    // Archicad rotates its offscreen 3D target, so those two pointers change
    // between frames, and `MaintainBinding` can only re-acquire them once every
    // two model generations -- a repair rate-limited below the rate of breakage.
    //
    // ⚠️ AND THIS RESTORES FROZEN FINDING 5 RATHER THAN
    // RELAXING IT: "camera identity is a SEMANTIC fingerprint, never COM pointer
    // identity". The pin was the one place still holding raw pointers. The
    // description -- width, height, format, sample count -- is what identifies
    // the surface, and it is what the fingerprint has always compared.
    term[kPinRenderTarget] = g_selection.renderTargetWidth == live.renderTargetDesc.width &&
                             g_selection.renderTargetHeight == live.renderTargetDesc.height &&
                             g_selection.renderTargetFormat == live.renderTargetDesc.format;
    term[kPinDepthStencil] = g_selection.depthWidth == live.depthStencilDesc.width &&
                             g_selection.depthHeight == live.depthStencilDesc.height &&
                             g_selection.depthFormat == live.depthStencilDesc.format;
    term[kPinViewport] = SameExtent (g_selection.viewportWidth, live.viewportWidth) &&
                         SameExtent (g_selection.viewportHeight, live.viewportHeight);
    // ⚠️ THE CONSTANT-BUFFER POINTERS ARE GONE, AND
    // MEASUREMENT IS WHY. A run on a 39340-triangle project produced a snapshot
    // for 16 of 157 model frames -- 10%, against 91% and 93% on lighter ones --
    // with `last pin miss: occurrence+viewBuffer` and the logical fingerprint
    // matching ten times in twenty seconds. The overlay was then composing three
    // or more generations behind on 42% of frames, which is what "lower
    // performance" looks like from the outside.
    //
    // ⚠️ AND THIS RESTORES FROZEN FINDING 5 RATHER THAN
    // RELAXING IT, for the second time: "camera identity is a SEMANTIC
    // fingerprint, never COM pointer identity". The render target and depth
    // stencil were converted from pointers to descriptions in a5fdfed and the
    // snapshot rate went from 64% to 91%; these two were the last raw pointers
    // left in the pin, and they fail for the same reason. `ContextHookDetours`
    // records that Archicad 29 keeps its constants in ONE 8 MiB ring and binds
    // WINDOWS of it -- so which buffer object the window happens to live in is
    // an allocation detail, and on a heavier scene it rotates.
    //
    // What identifies the camera window semantically is its SIZE, which
    // `kPinWindows` already compares, alongside the occurrence, the viewport and
    // both surface descriptions. Nothing here weakens to compensate: the terms
    // that remain are the ones that describe the draw rather than the memory it
    // was assembled in.
    term[kPinWindows] = g_selection.viewNumConstants == live.vsConstantBuffers[1].numConstants &&
                        g_selection.projectionNumConstants == live.vsConstantBuffers[2].numConstants;

    uint32_t missMask = 0;
    for (uint32_t i = 0; i < kPinTermCount; ++i) {
        if (!term[i]) {
            missMask |= 1u << i;
            ++g_binding.pinMissed[i];
        }
    }
    g_binding.pinMissMask = missMask;
    return missMask == 0;
}

// ⚠️ THE LOGICAL MATCH, AND IT IS WHAT A PRODUCTION RECOGNIZER
// WILL BE. Not one COM address appears in it. Every term is either a number
// Archicad chose about the rendering -- viewport, draw shape, target format --
// or a position within the frame.
// ⚠️ NO EARLY RETURN, AND THAT IS THE POINT. A short-circuiting
// predicate can only ever report the FIRST term that disagreed, which for a
// draw from an unrelated pass is always the viewport and never the interesting
// one. Evaluating all eight costs a handful of integer compares on a path that
// already does a COM call, and buys a diagnosis that would otherwise cost a run.
static bool MatchesFingerprint (const contextstate::ContextState& live, DrawKind kind, uint32_t indexCount,
                                uint32_t occurrence)
{
    if (!g_fingerprint.valid)
        return false;

    bool term[kFingerprintTermCount];
    term[kTermOccurrence] = g_fingerprint.occurrenceIndex == occurrence;
    term[kTermViewport] = SameExtent (g_fingerprint.viewportWidth, live.viewportWidth) &&
                          SameExtent (g_fingerprint.viewportHeight, live.viewportHeight) &&
                          SameExtent (g_fingerprint.viewportX, live.viewportX) &&
                          SameExtent (g_fingerprint.viewportY, live.viewportY);
    term[kTermDrawKind] = (g_fingerprint.drawKindMask & (1u << uint32_t (kind))) != 0;
    term[kTermIndexCount] = g_fingerprint.indexCount == indexCount;
    term[kTermCameraWindows] = g_fingerprint.viewNumConstants == live.vsConstantBuffers[1].numConstants &&
                               g_fingerprint.projectionNumConstants == live.vsConstantBuffers[2].numConstants;
    term[kTermDepthPresence] = g_fingerprint.depthPresent == (live.depthStencil != 0);

    // The resources themselves, described rather than named.
    term[kTermRenderTargetDesc] = g_fingerprint.renderTargetWidth == live.renderTargetDesc.width &&
                                  g_fingerprint.renderTargetHeight == live.renderTargetDesc.height &&
                                  g_fingerprint.renderTargetFormat == live.renderTargetDesc.format &&
                                  g_fingerprint.renderTargetSamples == live.renderTargetDesc.sampleCount;
    term[kTermDepthDesc] = g_fingerprint.depthWidth == live.depthStencilDesc.width &&
                           g_fingerprint.depthHeight == live.depthStencilDesc.height &&
                           g_fingerprint.depthFormat == live.depthStencilDesc.format &&
                           g_fingerprint.depthSamples == live.depthStencilDesc.sampleCount;

    ++g_diagnosis.evaluated;
    uint32_t failures = 0;
    uint32_t lastFailure = 0;
    uint32_t missMask = 0;
    for (uint32_t i = 0; i < kFingerprintTermCount; ++i) {
        if (term[i])
            continue;
        ++g_diagnosis.missed[i];
        ++failures;
        lastFailure = i;
        missMask |= 1u << i;
    }
    g_lastMissMask = missMask;
    g_binding.lastMissMask = missMask;
    if (failures == 0)
        return true;

    // ⚠️ ONLY A SOLE MISS IS EVIDENCE. A draw that disagrees on six
    // terms is some other pass and says nothing; a draw that agrees on seven and
    // disagrees on one is the camera, and that one term is the bug.
    g_lastSoleMiss = failures == 1 ? int (lastFailure) : -1;
    if (failures == 1) {
        ++g_diagnosis.soleMiss[lastFailure];
        if (!g_diagnosis.sampled[lastFailure]) {
            g_diagnosis.sampled[lastFailure] = true;
            uint32_t* out = g_diagnosis.observed[lastFailure];
            switch (lastFailure) {
                case kTermOccurrence:
                    out[0] = occurrence;
                    break;
                case kTermViewport:
                    out[0] = uint32_t (live.viewportWidth);
                    out[1] = uint32_t (live.viewportHeight);
                    out[2] = uint32_t (live.viewportX);
                    out[3] = uint32_t (live.viewportY);
                    break;
                case kTermDrawKind:
                    out[0] = uint32_t (kind);
                    break;
                case kTermIndexCount:
                    out[0] = indexCount;
                    break;
                case kTermCameraWindows:
                    out[0] = live.vsConstantBuffers[1].numConstants;
                    out[1] = live.vsConstantBuffers[2].numConstants;
                    break;
                case kTermDepthPresence:
                    out[0] = live.depthStencil != 0 ? 1u : 0u;
                    break;
                case kTermRenderTargetDesc:
                    out[0] = live.renderTargetDesc.width;
                    out[1] = live.renderTargetDesc.height;
                    out[2] = live.renderTargetDesc.format;
                    out[3] = live.renderTargetDesc.sampleCount;
                    break;
                default:
                    out[0] = live.depthStencilDesc.width;
                    out[1] = live.depthStencilDesc.height;
                    out[2] = live.depthStencilDesc.format;
                    out[3] = live.depthStencilDesc.sampleCount;
                    break;
            }
        }
    }
    return false;
}

// ⚠️ A RESIZE MOVES THREE TERMS AT ONCE, AND ASKING FOR A
// SOLE VIEWPORT MISS IS WHY THE RE-LEARN NEVER FIRED. The window's pixel size is
// carried by the viewport rectangle, by the back buffer's description AND by the
// depth buffer's description -- they are resized together, so a resize produces
// THREE failures and `failures == 1` was never true. The reported symptom was
// exactly that: the overlay froze at the old rectangle instead of re-learning.
//
// ⚠️ AND THE SAFETY THE SOLE-MISS RULE PROVIDED IS KEPT, NOT
// DROPPED. The misses must be a SUBSET of the three size-bearing terms, so every
// other term still has to agree -- including `indexCount`, which is the model's
// exact index count, and `occurrenceIndex`. A pass that agrees on those and
// differs only in pixel size is this camera at a new window size and nothing
// else. Formats and sample counts are checked explicitly because they live
// inside the same coarse terms and do NOT change with the window: if they moved
// too, this is a different target.
static const uint32_t kSizeTermMask = (1u << kTermViewport) | (1u << kTermRenderTargetDesc) | (1u << kTermDepthDesc);

// ⚠️ AND THE OCCURRENCE MOVES WITH THE SIZE, SO IT IS
// ADOPTABLE TOO. `SameSignatureCounter` keys the per-frame occurrence counter on
// the depth and target dimensions, so a resize allocates a FRESH counter that
// restarts at zero -- the occurrence term therefore misses alongside the three
// size terms and is not independent evidence of a different camera.
static const uint32_t kAdoptableMask = kSizeTermMask | (1u << kTermOccurrence);

// ⚠️ THE INDEX COUNT IS ADOPTABLE ONLY ONCE PER MODEL
// REVISION, AND THE OCCURRENCE IS NOT ADOPTED AT ALL. A different draw of the
// same family also differs only in index count, so without the revision gate
// this would let the camera wander between draws. With it, the relaxation fires
// exactly once per reported edit and every other term must still agree --
// including the shader, the viewport, both target descriptions and the
// constant-buffer window shape.
//
// The occurrence stays LOCKED (invariants section 1, item 6). It is allowed to
// MISS here, because adding elements changes how many times the family draws,
// but it is not overwritten: the per-frame counter restarts every frame, so the
// locked index is present again on the next whole frame and the rebind lands on
// the same draw it always did.
static const uint32_t kModelEditMask = (1u << kTermIndexCount) | (1u << kTermOccurrence);

static bool LooksLikeModelEdit ()
{
    if (g_lastMissMask == 0 || (g_lastMissMask & ~kModelEditMask) != 0)
        return false;
    return g_modelRevision != g_fingerprintRevision;
}

// ⚠️ THE OCCURRENCE IS NEVER INVENTED. TWO ATTEMPTS
// PROVED BOTH EXTREMES WRONG.
//
// Keeping the old index (stage 22) left the camera stuck with `miss=0x01` --
// the occurrence alone -- because an edit changes where in the frame our draw
// falls. Adopting the first post-edit draw's index (stage 23) was worse: the
// log read `Locked g4 occ42`, then `occ0`, then `occ136`, against an original
// lock of `occ2`. Those are arbitrary draws, and the ones that only happen while
// Archicad rebuilds -- so the snapshot arrived once per EDIT and never during
// navigation. That is precisely the reported symptom: place an element and the
// overlay syncs, move the camera and it does not follow.
//
// ⚠️ AN OCCURRENCE IS A MEASURED CHOICE, NOT THE FIRST
// THING TO ARRIVE. `SelectCandidate` picks it from samples, coverage and centre
// error. So an edit that moves it does not get a guess: the selection is
// DROPPED, and the auto-selector re-chooses on the measurements the census has
// been accumulating all along. Those groups already hold their samples, so this
// is a re-selection within a few model frames, not a relearn from zero.
//
// The index count IS adoptable on its own, because an index count has no
// alternative candidates to be confused with -- only a position in the frame
// does.
static void AdoptModelEdit (uint32_t indexCount)
{
    g_fingerprint.indexCount = indexCount;
    g_fingerprintRevision = g_modelRevision;
    g_selectionLastSeenModel = 0;
    ++g_binding.modelEditRebinds;
}

// ⚠️ AN EDIT MAKES THE OCCURRENCE SUSPECT; IT DOES NOT
// MAKE THE CAMERA UNKNOWN. Dropping the selection sent the runtime to `Learning`,
// and a fresh census selection needs MODEL FRAMES -- which do not arrive while
// the user is looking at a still viewport. So an edit blanked the overlay until
// something else happened to redraw the model: "manual move causes desync and
// requires a wait or a new object in the scene", which is precisely what was
// reported.
//
// ⚠️ SO WAIT FOR EVIDENCE INSTEAD OF GUESSING OR
// DISCARDING. The index count is adopted immediately, because it has no rival
// candidates. The occurrence is only marked SUSPECT: the pin keeps drawing, the
// camera stays Locked, and nothing changes unless the OLD occurrence actually
// stops appearing for `kOccurrenceGrace` model generations. Only then is the
// occurrence of a fingerprint-matching draw taken -- by which point that draw has
// demonstrated it is the family's live position rather than a rebuild artefact.
//
// That is what the two failed attempts were reaching for. Keeping the old index
// forever left the camera stuck; taking the first post-edit draw's index gave
// `occ136`; dropping the selection cost a relearn. Waiting two generations costs
// two frames and needs no guess.
static const uint64_t kOccurrenceGrace = 2;

static void SuspectOccurrenceAfterModelEdit (uint32_t indexCount, uint64_t modelGeneration)
{
    if ((g_lastMissMask & (1u << kTermIndexCount)) != 0)
        g_fingerprint.indexCount = indexCount;
    g_fingerprintRevision = g_modelRevision;
    if (g_occurrenceSuspectSince == 0)
        g_occurrenceSuspectSince = modelGeneration;
}

// The old occurrence has been silent long enough to be gone. Take the live one.
static void AdoptSuspectOccurrence (uint32_t occurrence)
{
    g_fingerprint.occurrenceIndex = occurrence;
    g_selection.occurrenceIndex = occurrence;
    g_occurrenceSuspectSince = 0;
    g_selectionLastSeenModel = 0;
    ++g_binding.modelEditReselects;
}

static bool LooksLikeResize (const contextstate::ContextState& live)
{
    if ((g_lastMissMask & kSizeTermMask) == 0 || (g_lastMissMask & ~kAdoptableMask) != 0)
        return false;
    return g_fingerprint.renderTargetFormat == live.renderTargetDesc.format &&
           g_fingerprint.renderTargetSamples == live.renderTargetDesc.sampleCount &&
           g_fingerprint.depthFormat == live.depthStencilDesc.format &&
           g_fingerprint.depthSamples == live.depthStencilDesc.sampleCount;
}

// ⚠️ TAKE THE NEW PIXEL SIZE INTO THE FINGERPRINT AND KEEP THE
// CAMERA. Guidance section 5 freezes the lifecycle: on resize, resource
// recreation or viewport reconstruction the camera goes Locked -> Reacquiring ->
// Locked, and the subsystem is NOT relearned. Clearing the selection and the
// group table instead made the overlay wait for THIRTY-TWO fresh model frames --
// which only arrive while the user orbits, so a resize left the overlay desynced
// until someone navigated. That is the behaviour this replaces.
//
// Nothing about the camera's identity changes here: the shader, the draw shape,
// the index count, the constant-buffer window shape and the formats are all
// still required to agree. Only the numbers that ARE the window size move.
static void AdoptResize (const contextstate::ContextState& live, uint32_t occurrence)
{
    g_fingerprint.viewportX = live.viewportX;
    g_fingerprint.viewportY = live.viewportY;
    g_fingerprint.viewportWidth = live.viewportWidth;
    g_fingerprint.viewportHeight = live.viewportHeight;
    g_fingerprint.renderTargetWidth = live.renderTargetDesc.width;
    g_fingerprint.renderTargetHeight = live.renderTargetDesc.height;
    g_fingerprint.depthWidth = live.depthStencilDesc.width;
    g_fingerprint.depthHeight = live.depthStencilDesc.height;
    // ⚠️ THE OCCURRENCE IS NOT ADOPTED, AND TAKING IT WAS
    // WRONG. Guidance section 4 locks the occurrence as part of the selection
    // transaction. The draw that first reports the new size is whichever one the
    // resize happened to interrupt -- the log showed a camera locked at `occ2`
    // re-emerging as `occ0` -- so adopting it re-points the camera at a
    // DIFFERENT draw of the same family. The per-frame counter restarts at zero
    // for every frame anyway, so the family's numbering is unchanged by a resize
    // and the locked index is still the right one once a whole frame has passed.
    (void) occurrence;
    // The pinned resources are wherever the new size put them, and we no longer
    // know: force the rebind below to treat the pin as stale so it moves THIS
    // frame rather than waiting out the staleness window.
    g_selectionLastSeenModel = 0;
    ++g_binding.resizeRebinds;
}

// ⚠️ RE-ACQUIRE THE RUNTIME RESOURCES, DO NOT RE-DECIDE THE CAMERA.
// This is the whole repair for run forty-five, and it is deliberately narrow: it
// can only ever move the selection onto a draw that already matches the
// fingerprint phase A committed to. It cannot choose a different camera, it
// cannot run before a selection exists, and it changes no score.
void MaintainBinding (const contextstate::ContextState& live, DrawKind kind, uint32_t indexCount, uint32_t occurrence,
                      uint64_t modelGeneration)
{
    if (!g_selection.valid)
        return;

    // The pin is still live. Nothing to do, and that is the common case.
    if (MatchesSelection (live, occurrence)) {
        ++g_binding.selectionMatches;
        g_selectionLastSeenModel = modelGeneration;
        // The pinned occurrence is still drawing, so whatever an edit did, it did
        // not remove it. Nothing is suspect any more.
        g_occurrenceSuspectSince = 0;
        return;
    }
    if (!MatchesFingerprint (live, kind, indexCount, occurrence)) {
        // ⚠️ A RESIZE CANNOT HEAL ITSELF, BECAUSE THE WINDOW
        // SIZE IS PART OF THE IDENTITY. Once it changes, the fingerprint matches
        // nothing ever again: the selection sits there valid and unreachable
        // while Present keeps drawing with the last snapshot, and the overlay
        // stays pinned to a rectangle that no longer exists.
        //
        // ⚠️ SO ADOPT THE NEW SIZE AND FALL THROUGH TO THE
        // REBIND. The draw agreed on everything that is not the window size --
        // including `indexCount`, the model's exact index count -- so it is this
        // camera at a new size and nothing else. See `AdoptResize` for why
        // relearning instead was the wrong repair.
        // ⚠️ A MODEL EDIT IS CHECKED FIRST, BECAUSE IT IS
        // THE ONE THAT CANNOT HEAL AND THE ONE THAT FROZE A LIVE SESSION.
        if (LooksLikeModelEdit ()) {
            if ((g_lastMissMask & (1u << kTermOccurrence)) != 0) {
                SuspectOccurrenceAfterModelEdit (indexCount, modelGeneration);
                // ⚠️ THE PIN IS STILL VALID UNTIL PROVEN
                // OTHERWISE. Returning here keeps the camera Locked and the
                // overlay drawing; only silence from the old occurrence moves it.
                if (g_occurrenceSuspectSince == 0 || modelGeneration <= g_occurrenceSuspectSince + kOccurrenceGrace ||
                    g_selectionLastSeenModel + kOccurrenceGrace >= modelGeneration) {
                    return;
                }
                AdoptSuspectOccurrence (occurrence);
            }
            else {
                AdoptModelEdit (indexCount);
            }
        }
        else if (LooksLikeResize (live)) {
            AdoptResize (live, occurrence);
        }
        else {
            return;
        }
    }
    ++g_binding.logicalMatches;

    // ⚠️ ONLY WHEN THE PIN HAS GONE QUIET. A draw that matches the
    // fingerprint while the pinned resources are still drawing is a SECOND
    // family of the same logical shape, not the same one moved -- and taking the
    // binding from it would make the camera alternate between two draws, which
    // is the fault run thirty-four spent a whole run on.
    const bool pinIsStale = g_selectionLastSeenModel == 0 || modelGeneration > g_selectionLastSeenModel + 1;
    if (!pinIsStale) {
        ++g_binding.rebindsRefused;
        return;
    }

    g_selection.renderTarget = live.renderTarget;
    g_selection.depthStencil = live.depthStencil;
    g_selection.renderTargetWidth = live.renderTargetDesc.width;
    g_selection.renderTargetHeight = live.renderTargetDesc.height;
    g_selection.renderTargetFormat = live.renderTargetDesc.format;
    g_selection.depthWidth = live.depthStencilDesc.width;
    g_selection.depthHeight = live.depthStencilDesc.height;
    g_selection.depthFormat = live.depthStencilDesc.format;
    g_selection.viewportX = live.viewportX;
    g_selection.viewportY = live.viewportY;
    g_selection.viewportWidth = live.viewportWidth;
    g_selection.viewportHeight = live.viewportHeight;
    g_selection.viewBuffer = live.vsConstantBuffers[1].buffer;
    g_selection.viewNumConstants = live.vsConstantBuffers[1].numConstants;
    g_selection.projectionBuffer = live.vsConstantBuffers[2].buffer;
    g_selection.projectionNumConstants = live.vsConstantBuffers[2].numConstants;
    g_selectionLastSeenModel = modelGeneration;
    ++g_binding.rebinds;
}

Lifecycle GetLifecycle (bool learning, uint64_t modelGeneration)
{
    if (!g_selection.valid)
        return learning ? Lifecycle::Learning : Lifecycle::Unknown;
    // ⚠️ THE SAME STALENESS TEST `MaintainBinding` REBINDS ON, so the
    // reported state and the behaviour cannot drift apart. One model generation
    // of grace: a single frame in which the pinned family did not draw is a
    // frame, not a loss.
    if (g_selectionLastSeenModel != 0 && modelGeneration <= g_selectionLastSeenModel + 1)
        return Lifecycle::Locked;
    return Lifecycle::Reacquiring;
}

const char* LifecycleName (Lifecycle state)
{
    switch (state) {
        case Lifecycle::Unknown:
            return "Unknown";
        case Lifecycle::Learning:
            return "Learning";
        case Lifecycle::Locked:
            return "Locked";
        case Lifecycle::Reacquiring:
            return "Reacquiring";
    }
    return "Unknown";
}

// ⚠️ EVERY TERM COMES FROM THE COPIED GROUP. `CopyGroups`
// already folds the slot's medians into it, so nothing here needs the census's
// private sample buffers -- which is what let this file separate at all.
// ⚠️ NO EARLY RETURN, AND THAT IS THE POINT -- the same rule
// `MatchesFingerprint` already follows. A short-circuiting gate can only report
// the FIRST term that failed, which for a camera still accumulating samples is
// always `samples`, and never the one that actually decides.
static bool Qualifies (const Group& group, uint64_t modelFrames, float& coverage, float& insideClip, float& agreement)
{
    coverage = modelFrames > 0 ? float (double (group.modelFramesObserved) / double (modelFrames)) : 0.0f;
    insideClip = group.samplesScored > 0 ? float (double (group.anchorInside) / double (group.samplesScored)) : 0.0f;
    agreement = group.samplesScored > 0 ? float (double (group.trianglesFinite) / double (group.samplesScored)) : 0.0f;

    bool term[kGateTermCount];
    term[kGateSamples] = group.samplesScored >= g_eligibility.minSamples;
    term[kGateCoverage] = coverage >= g_eligibility.minModelCoverage;
    term[kGateInsideClip] = insideClip >= g_eligibility.minInsideClip;
    term[kGateFiniteTriangles] = agreement >= g_eligibility.minFiniteTriangles;
    // The two a collapse cannot pass: everything above was satisfied for six
    // runs by a transform that drew one pixel.
    term[kGateAreaPixels] = group.medianAreaPixels >= g_eligibility.minMedianAreaPixels;
    term[kGateEdgePixels] = group.medianMaxEdgePixels >= g_eligibility.minMedianMaxEdgePixels;
    // Centre error is the weak term: a human hand on a mouse does not put the
    // orbit target on the anchor to the pixel.
    term[kGateCentreError] = !(group.errorSamples > 0 && group.medianCentreError > g_eligibility.maxMedianCentreError);

    ++g_gate.evaluated;
    uint32_t failures = 0;
    uint32_t lastFailure = 0;
    for (uint32_t i = 0; i < kGateTermCount; ++i) {
        if (term[i])
            continue;
        ++g_gate.missed[i];
        ++failures;
        lastFailure = i;
    }
    if (failures == 1)
        ++g_gate.soleMiss[lastFailure];

    // ⚠️ THE CLOSEST CANDIDATE IS KEPT WITH ITS ACTUAL NUMBERS, so a
    // refusal can be argued with rather than believed. Fewest failures wins;
    // ties go to the one with the most samples, because that is the one whose
    // measurements mean the most.
    const bool closer = !g_gate.haveClosest || failures < g_gate.closestFailures ||
                        (failures == g_gate.closestFailures && group.samplesScored > g_gate.closestSamples);
    if (closer) {
        g_gate.haveClosest = true;
        g_gate.closestGroupId = group.groupId;
        g_gate.closestFailures = failures;
        g_gate.closestSamples = group.samplesScored;
        g_gate.closestCoverage = coverage;
        g_gate.closestInsideClip = insideClip;
        g_gate.closestFinite = agreement;
        g_gate.closestAreaPixels = group.medianAreaPixels;
        g_gate.closestEdgePixels = group.medianMaxEdgePixels;
        g_gate.closestCentreError = group.medianCentreError;
    }
    return failures == 0;
}

// ⚠️ THE TABLE ARRIVES AS A COPY AND THIS FILE NEVER TOUCHES
// THE CENSUS'S SLOTS. That is what makes the seam real rather than a file move:
// the decision is a pure function of the measurements plus the gate, and it
// could be re-run against a recorded table with no Archicad at all.
bool SelectCandidate (const Group* groups, size_t count, uint64_t modelFrames, bool allowUncalibrated)
{
    const bool calibrated = modelFrames >= kCalibrationModelFrames;

    // ⚠️ ELIGIBILITY FIRST, RANK SECOND, AND NEVER THE OTHER WAY. Ranking by
    // error alone put a group with ONE sample and no coverage ahead of the real
    // model camera, which carried 1497 draws across 259 model frames.
    const Group* best = nullptr;
    float bestCoverage = 0.0f;
    float bestInside = 0.0f;
    float bestMedian = 0.0f;
    uint32_t bestVariant = 0;
    // The best coverage among the candidates that did NOT win, so a decision
    // can be argued with: a runner-up within a point of the winner is a
    // different situation from one twenty points behind.
    float runnerUpCoverage = 0.0f;
    g_eligibleCandidates = 0;
    // Each attempt is judged on its own; a stale diagnosis from a previous
    // attempt would name a term that has since been satisfied.
    g_gate = EligibilityDiagnosis {};
    for (size_t i = 0; i < count; ++i) {
        float coverage = 0.0f;
        float insideClip = 0.0f;
        float agreement = 0.0f;
        if (!Qualifies (groups[i], modelFrames, coverage, insideClip, agreement))
            continue;
        ++g_eligibleCandidates;
        // ⚠️ COVERAGE, THEN CLIP CONTAINMENT, THEN THE
        // CANONICAL INTERPRETATION, AND ERROR ONLY LAST. Centre error is the
        // weak term -- a hand on a mouse does not put the orbit target on the
        // anchor to the pixel -- so ranking on it promotes noise. Variant 0
        // (View x Projection) is what every run the overlay tracked in chose;
        // preferring it at equal measurement makes a tie deterministic instead
        // of leaving it to table order.
        const bool tiedOnShape = coverage >= bestCoverage - 0.01f && insideClip >= bestInside - 0.005f;
        const bool canonical = groups[i].winningVariant == 0 && bestVariant != 0;
        const bool better =
            best == nullptr || coverage > bestCoverage + 0.01f ||
            (coverage >= bestCoverage - 0.01f && insideClip > bestInside + 0.005f) || (tiedOnShape && canonical) ||
            (tiedOnShape && groups[i].winningVariant == bestVariant && groups[i].medianCentreError < bestMedian);
        if (better) {
            if (best != nullptr && bestCoverage > runnerUpCoverage)
                runnerUpCoverage = bestCoverage;
            best = &groups[i];
            bestCoverage = coverage;
            bestInside = insideClip;
            bestMedian = groups[i].medianCentreError;
            bestVariant = groups[i].winningVariant;
        }
        else if (coverage > runnerUpCoverage) {
            runnerUpCoverage = coverage;
        }
    }
    g_bind.candidates = g_eligibleCandidates;
    g_bind.calibrationFrames = modelFrames;
    g_bind.calibrationTarget = kCalibrationModelFrames;
    g_bind.runnerUpCoverage = runnerUpCoverage;

    if (best == nullptr) {
        // ⚠️ AN ATTEMPT THAT FINDS NOTHING MAY NOT UNDO ONE
        // THAT FOUND SOMETHING. This used to be reachable only with no
        // selection, so clearing cost nothing. It is now reachable while a
        // provisional pin is live, and one frame in which no group happened to
        // qualify would throw it away for 32 fresh model frames (section 3).
        if (g_selection.valid)
            return true;
        g_bind.selected = false;
        g_bind.reason = BindReason::NoCandidate;
        ++g_bind.serial;
        // ⚠️ FAIL CLOSED. A run that could not identify the camera injects
        // nothing, rather than injecting with whatever drew last -- which is the
        // behaviour that produced six inconclusive runs.
        g_selection = Selection {};
        // ⚠️ FAILING TO SELECT MUST ALSO FAIL THE SOURCE, or the
        // injection would keep pointing at a selection that no longer exists.
        injection::SetCameraSource (injection::CameraSource::Learner);
        return false;
    }

    // ⚠️ OBSERVE EVERY CANDIDATE FOR A FIXED WINDOW, THEN
    // COMMIT ONCE. This used to commit the instant anything qualified: every
    // `CAMERA Locked` line in the logs that produced this reads `samples=32`,
    // which is exactly `minSamples`. The winner was therefore whichever group
    // crossed the evidence threshold FIRST, with coverage measured over a
    // handful of model frames and `minModelCoverage` set at 0.80 -- so the same
    // binary chose occ3 at 97% in one run and occ8 at 89% in the next, and
    // which one won was a race between draw order and a counter.
    //
    // ⚠️ AN UPGRADE WINDOW WAS NOT ENOUGH AND IS GONE. It
    // still locked early and then corrected, which is a race with a repair
    // bolted on: the pin, the interpretation and every measurement taken
    // against them differ between the early phase and the late one, and no
    // later experiment can tell those apart. Waiting removes the race instead
    // of compensating for it.
    // ⚠️ A PROVISIONAL PIN MUST ACTUALLY BE RE-DECIDED,
    // and the guard has to be `g_calibrated` rather than "a selection exists".
    // Keyed on the latter, the stationary path would take a provisional pin,
    // `g_selection.valid` would be true ever after, and the calibrated decision
    // this whole change exists for would return early and never happen.
    if (g_calibrated && !allowUncalibrated)
        return true;
    if (!calibrated) {
        // ⚠️ EXCEPT WHEN THERE ARE NO MODEL FRAMES TO WAIT
        // FOR. A viewport nobody has navigated produces none at all, and the
        // logs show exactly that at every start: "asked the 3D window to redraw
        // three times and it produced no model frames". Refusing outright would
        // leave a stationary session with no overlay forever, so the still path
        // may take a PROVISIONAL pin -- and `g_calibrated` stays false, so the
        // first `kCalibrationModelFrames` of real navigation re-decide it.
        if (!allowUncalibrated) {
            g_bind.selected = false;
            g_bind.reason = BindReason::Calibrating;
            ++g_bind.serial;
            return false;
        }
    }

    Selection chosen;
    chosen.valid = true;
    // ⚠️ A RUN-LOCAL ID, WHICH IS ALL A GROUP IDENTITY MAY BE. It exists so every
    // injection row can name the group it came from; it means nothing in the next
    // session and is never written anywhere that outlives one.
    chosen.groupId = best->groupId;
    chosen.occurrenceIndex = best->occurrenceIndex;
    chosen.vertexShader = best->vertexShader;
    chosen.renderTarget = best->renderTarget;
    chosen.depthStencil = best->depthStencil;
    chosen.viewportX = best->viewportX;
    chosen.viewportY = best->viewportY;
    chosen.viewportWidth = best->viewportWidth;
    chosen.viewportHeight = best->viewportHeight;
    chosen.viewBuffer = best->viewBuffer;
    chosen.viewNumConstants = best->viewNumConstants;
    chosen.projectionBuffer = best->projectionBuffer;
    chosen.projectionNumConstants = best->projectionNumConstants;
    chosen.variant = best->winningVariant;
    chosen.samples = best->samplesScored;
    chosen.modelCoverage = bestCoverage;
    chosen.insideClip = bestInside;
    chosen.medianCentreError = best->medianCentreError;
    chosen.medianAreaPixels = best->medianAreaPixels;
    chosen.medianMaxEdgePixels = best->medianMaxEdgePixels;
    g_selection = chosen;
    // The index count in this fingerprint belongs to THIS model revision.
    g_fingerprintRevision = g_modelRevision;

    // ⚠️ SELECTING A CANDIDATE AND POINTING THE INJECTION AT IT ARE
    // ONE TRANSACTION, AND THEY LIVE HERE. They were two acts in two places:
    // `ViewerCameraCensus {select:true}` called this and THEN set the source,
    // while `SetAutoSelect` called this and did not. The runtime therefore
    // reached a state that is not supposed to exist --
    //
    //     selection.valid = true, lifecycle = Locked
    //     BUT cameraSource = Learner
    //
    // -- in which the recognizer truthfully reports a locked camera while the
    // injection sources from the Learner, whose snapshot path stands down once a
    // census group is chosen. No snapshot, no `Active`, no Present, and no skip
    // counter anywhere to say so.
    //
    // ⚠️ SO NO CALLER SETS THE SOURCE ANY MORE. One implementation,
    // reached by both the explicit command and the automatic promotion, is the
    // only arrangement in which the two cannot drift apart again.
    injection::SetCameraSource (injection::CameraSource::CensusSelectedGroup);

    // ⚠️ AND THE SAME DECISION IS RECORDED IN TERMS THAT OUTLIVE THE
    // RESOURCES. `chosen` is how to find the camera right now; this is what the
    // camera IS. Run forty-five kept only the first and lost the camera to a
    // render-target rebuild it never even noticed.
    Fingerprint print;
    print.valid = true;
    print.occurrenceIndex = best->occurrenceIndex;
    print.viewportX = best->viewportX;
    print.viewportY = best->viewportY;
    print.viewportWidth = best->viewportWidth;
    print.viewportHeight = best->viewportHeight;
    print.drawKindMask = best->drawKindMask;
    print.indexCount = best->lastIndexCount;
    print.viewNumConstants = best->viewNumConstants;
    print.projectionNumConstants = best->projectionNumConstants;
    print.depthPresent = best->depthPresent;
    print.renderTargetWidth = best->renderTargetWidth;
    print.renderTargetHeight = best->renderTargetHeight;
    print.renderTargetFormat = best->renderTargetFormat;
    print.renderTargetSamples = best->renderTargetSamples;
    print.depthWidth = best->depthWidth;
    print.depthHeight = best->depthHeight;
    print.depthFormat = best->depthFormat;
    print.depthSamples = best->depthSamples;
    print.variant = best->winningVariant;
    print.drawOrdinalFirst = uint32_t (best->drawSequenceFirst);
    print.drawOrdinalLast = uint32_t (best->drawSequenceLast);
    g_fingerprint = print;
    g_binding.fingerprintValid = true;
    g_selectionLastSeenModel = 0;
    g_calibrated = calibrated;
    g_binding.calibrated = calibrated;
    g_bind.selected = true;
    g_bind.groupId = chosen.groupId;
    g_bind.occurrenceIndex = chosen.occurrenceIndex;
    g_bind.variant = chosen.variant;
    g_bind.coverage = bestCoverage;
    g_bind.insideClip = bestInside;
    g_bind.centreError = best->medianCentreError;
    g_bind.reason = calibrated ? BindReason::HighestCoverage : BindReason::StationaryFallback;
    ++g_bind.serial;

    // ⚠️ THE SHADER IS TOLD WHAT WAS LEARNED, AND REFUSES IF IT CANNOT HONOUR IT.
    // This is the link that was missing for run thirty-three: the census proved
    // `p * V * Pt` twice over while the shader rendered `p * V * P`, and nothing
    // in the pipeline compared the two.
    injection::SetExpectedInterpretation (chosen.variant);
    // ⚠️ THE OCCURRENCE TRAVELS WITH THE SIGNATURE. They are one identity, and
    // handing over only half of it is what let the snapshot take whichever draw
    // of the family came last.
    injection::SetSelectedOccurrence (chosen.occurrenceIndex);
    return true;
}

// ⚠️ THE FIRST GROUP TO REACH 32 SAMPLES USED TO WIN THE
// SESSION. `AttemptAutoSelect` was gated on `!fingerprintValid`, so the moment
// anything qualified the census stopped choosing -- permanently, for that run.
// Which group that was is a race between draw order and the 32-sample
// threshold, which is why the same binary produced an overlay locked to the
// geometry in one run and lagging in the next with no code change between them.
//
// ⚠️ SO THE WINDOW CLOSES; IT DOES NOT STAY OPEN. After
// `kSettleModelFrames` the answer is final and this costs one branch again,
// which is the property the original gate was protecting. Re-ranking forever
// would make the pin a moving target and hand every later diagnosis a variable
// nobody asked for (section 13, one variable per run).
bool WantsSelectionAttempt (uint64_t modelFrames)
{
    if (!g_binding.fingerprintValid)
        return true;
    // A provisional pin keeps being re-decided until there is enough evidence
    // to decide it properly; a calibrated one is final, and the gate costs one
    // branch again (section 13, one variable per run).
    return !g_calibrated;
}

BindReport GetBindReport ()
{
    return g_bind;
}

const char* BindReasonName (BindReason reason)
{
    switch (reason) {
        case BindReason::Calibrating:
            return "calibrating";
        case BindReason::NoCandidate:
            return "noCandidate";
        case BindReason::HighestCoverage:
            return "highestCoverage";
        case BindReason::StationaryFallback:
            return "stationaryFallback";
    }
    return "unknown";
}

void NoteModelRevision (uint32_t revision)
{
    g_modelRevision = revision;
}

void ClearSelection ()
{
    // ⚠️ THIS IS THE ONLY THING THAT DROPS THE FINGERPRINT, and it is
    // called once, at the start of phase A, when the run is about to learn a new
    // one. `census::ResetCounts` does not call it.
    g_selection = Selection {};
    g_fingerprint = Fingerprint {};
    g_binding.fingerprintValid = false;
    g_selectionLastSeenModel = 0;
    // A new learning phase gets a new settling window (section 8: no session
    // state outlives the session that learned it).
    g_calibrated = false;
    g_binding.calibrated = false;
    g_bind = BindReport {};
    injection::SetExpectedInterpretation (0xffffffffu);
}

Selection GetSelection ()
{
    return g_selection;
}

Fingerprint GetFingerprint ()
{
    return g_fingerprint;
}

Eligibility GetEligibility ()
{
    return g_eligibility;
}

void NoteSnapshot ()
{
    ++g_selection.snapshotsTaken;
}

FingerprintDiagnosis GetFingerprintDiagnosis ()
{
    return g_diagnosis;
}

uint32_t EligibleCandidates ()
{
    return g_eligibleCandidates;
}

const char* GateTermName (uint32_t term)
{
    switch (term) {
        case kGateSamples:
            return "samples";
        case kGateCoverage:
            return "coverage";
        case kGateInsideClip:
            return "insideClip";
        case kGateFiniteTriangles:
            return "finiteTriangles";
        case kGateAreaPixels:
            return "areaPixels";
        case kGateEdgePixels:
            return "edgePixels";
        case kGateCentreError:
            return "centreError";
        default:
            return "?";
    }
}

EligibilityDiagnosis GetEligibilityDiagnosis ()
{
    return g_gate;
}

BindingStats GetBindingStats ()
{
    return g_binding;
}

void ResetBindingStats ()
{
    // ⚠️ THE FINGERPRINT AND THE SELECTION DELIBERATELY SURVIVE THIS.
    // What is reset is the COUNTERS and the memory of WHERE the pinned resources
    // were last seen -- phase B's generations are not phase A's, and starting at
    // "never seen" is what lets the first logical match re-acquire immediately
    // instead of waiting out a staleness window.
    const bool valid = g_fingerprint.valid;
    const uint64_t rebinds = g_binding.resizeRebinds;
    const uint64_t edits = g_binding.modelEditRebinds;
    const uint64_t reselects = g_binding.modelEditReselects;
    g_binding = BindingStats {};
    g_binding.modelEditRebinds = edits;
    g_binding.modelEditReselects = reselects;
    // A resize that has already been adopted is not undone by a count reset, and
    // Present latches on this to refuse a stale camera.
    g_binding.resizeRebinds = rebinds;
    g_binding.fingerprintValid = valid;
    g_diagnosis = FingerprintDiagnosis {};
    g_selectionLastSeenModel = 0;
}

void ShutdownRecognizer ()
{
    g_selection = Selection {};
    g_fingerprint = Fingerprint {};
    g_binding = BindingStats {};
    g_diagnosis = FingerprintDiagnosis {};
    g_selectionLastSeenModel = 0;
}

} // namespace census
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
