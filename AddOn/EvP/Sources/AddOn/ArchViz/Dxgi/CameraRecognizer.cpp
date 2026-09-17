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
// How many groups cleared the eligibility gate on the last attempt. A promotion
// that never happens is a different fault depending on whether this is zero.
uint32_t g_eligibleCandidates = 0;
EligibilityDiagnosis g_gate;
// Which single term the last evaluated draw missed, or -1. See `SoleMissWasViewport`.
int g_lastSoleMiss = -1;

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

bool MatchesSelection (const contextstate::ContextState& live, uint32_t occurrence)
{
    if (!g_selection.valid)
        return false;
    return g_selection.occurrenceIndex == occurrence && g_selection.renderTarget == live.renderTarget &&
           g_selection.depthStencil == live.depthStencil &&
           SameExtent (g_selection.viewportWidth, live.viewportWidth) &&
           SameExtent (g_selection.viewportHeight, live.viewportHeight) &&
           SameExtent (g_selection.viewportX, live.viewportX) && SameExtent (g_selection.viewportY, live.viewportY) &&
           g_selection.viewBuffer == live.vsConstantBuffers[1].buffer &&
           g_selection.projectionBuffer == live.vsConstantBuffers[2].buffer &&
           g_selection.viewNumConstants == live.vsConstantBuffers[1].numConstants &&
           g_selection.projectionNumConstants == live.vsConstantBuffers[2].numConstants;
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
    for (uint32_t i = 0; i < kFingerprintTermCount; ++i) {
        if (term[i])
            continue;
        ++g_diagnosis.missed[i];
        ++failures;
        lastFailure = i;
    }
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

// True when the draw just evaluated agreed on every fingerprint term except the
// viewport. Reads the diagnosis `MatchesFingerprint` has just written, so it must
// be called immediately after it and nowhere else.
static bool SoleMissWasViewport ()
{
    return g_lastSoleMiss == int (kTermViewport);
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
        return;
    }
    if (!MatchesFingerprint (live, kind, indexCount, occurrence)) {
        // ⚠️ A SOLE VIEWPORT MISS IS A RESIZE, AND IT IS THE ONE
        // MISMATCH THAT CANNOT HEAL ITSELF. Every other term can come back --
        // Archicad rebuilds views and the descriptors match again -- but the
        // viewport rectangle IS part of the identity, so once the window changes
        // size the fingerprint can never match anything again. The selection
        // then sits there, valid and unreachable, while Present keeps drawing
        // with the last snapshot: the overlay lands in the wrong place on the
        // screen, which is exactly what a resize was reported to do.
        //
        // ⚠️ AND "SOLE MISS" IS WHAT MAKES THIS SAFE. Thousands of
        // unrelated draws disagree on the viewport as well as on six other
        // terms; only a draw that agrees on ALL SEVEN others is this camera
        // family at a new size. That rule is already the fingerprint
        // diagnosis's, and this is the first use of it to decide something.
        if (SoleMissWasViewport ()) {
            ++g_binding.resizeRelearns;
            ClearSelection ();
        }
        return;
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
bool SelectCandidate (const Group* groups, size_t count, uint64_t modelFrames)
{

    // ⚠️ ELIGIBILITY FIRST, RANK SECOND, AND NEVER THE OTHER WAY. Ranking by
    // error alone put a group with ONE sample and no coverage ahead of the real
    // model camera, which carried 1497 draws across 259 model frames.
    const Group* best = nullptr;
    float bestCoverage = 0.0f;
    float bestInside = 0.0f;
    float bestMedian = 0.0f;
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
        const bool better = best == nullptr || coverage > bestCoverage + 0.01f ||
                            (coverage >= bestCoverage - 0.01f && insideClip > bestInside + 0.005f) ||
                            (coverage >= bestCoverage - 0.01f && insideClip >= bestInside - 0.005f &&
                             groups[i].medianCentreError < bestMedian);
        if (better) {
            best = &groups[i];
            bestCoverage = coverage;
            bestInside = insideClip;
            bestMedian = groups[i].medianCentreError;
        }
    }
    if (best == nullptr) {
        // ⚠️ FAIL CLOSED. A run that could not identify the camera injects
        // nothing, rather than injecting with whatever drew last -- which is the
        // behaviour that produced six inconclusive runs.
        g_selection = Selection {};
        // ⚠️ FAILING TO SELECT MUST ALSO FAIL THE SOURCE, or the
        // injection would keep pointing at a selection that no longer exists.
        injection::SetCameraSource (injection::CameraSource::Learner);
        return false;
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

void ClearSelection ()
{
    // ⚠️ THIS IS THE ONLY THING THAT DROPS THE FINGERPRINT, and it is
    // called once, at the start of phase A, when the run is about to learn a new
    // one. `census::ResetCounts` does not call it.
    g_selection = Selection {};
    g_fingerprint = Fingerprint {};
    g_binding.fingerprintValid = false;
    g_selectionLastSeenModel = 0;
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
    g_binding = BindingStats {};
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
