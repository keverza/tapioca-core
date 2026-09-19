// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See OverlayRuntimeReport.hpp.

#include "ArchViz/OverlayRuntimeReport.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraFreshness.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchVizLog.hpp"

#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {
namespace report {

namespace cen = dxgi::census;

// Last decision narrated, so a decision that has not changed costs a compare.
uint32_t g_lastBindSerial = 0;
size_t g_matricesSaid = 0;
std::string g_lastComposite;
std::string g_lastEpochGate;

namespace {

// ⚠️ PREVIOUS TOTALS, SO THE REPORT CAN SUBTRACT. A cumulative
// counter answers "did this ever happen", and every question worth asking after
// the overlay starts is "is it happening NOW". See `NarrateLive`.
struct LiveMark {
    uint64_t present = 0;
    uint64_t compose = 0;
    uint64_t stale = 0;
    uint64_t noDepth = 0;
    uint64_t noGeometry = 0;
    uint64_t noEdges = 0;
    uint64_t noCamera = 0;
    uint64_t culled = 0;
    uint64_t mismatch = 0;
    uint64_t sceneNew = 0;
    uint64_t sceneRepeat = 0;
    uint64_t sceneLate = 0;
    uint64_t suppressed = 0;
};
LiveMark g_mark;
std::string g_lastLive;
uint32_t g_liveTicks = 0;
std::string g_lastChain;
std::string g_lastWatch;
std::string g_lastBackend;
std::string g_lastPulse;
std::string g_lastSync;
uint32_t g_syncTicks = 0;
std::string g_lastChains;
std::string g_lastScenePass;
std::string g_lastSignature;
std::string g_lastVariants;

} // namespace

// The overlay's own channel log, in the format the reader needs: one line per
// TRANSITION, never per tick. If it stops, the last line is the diagnosis.
void Say (const char* channel, const std::string& detail)
{
    // ⚠️ 512, BECAUSE 256 SILENTLY ATE THE ANSWER. The LIVE
    // line grew past the buffer and `_TRUNCATE` cut it exactly where
    // `suppressed+` and `redraw=` begin -- the two numbers the resize acceptance
    // criteria are read from. A report that truncates its own conclusion is worse
    // than one that never printed it, because it looks complete. See the header.
    char line[512] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE, "%-12s %s", channel, detail.c_str ());
    ArchVizLog (line);
}

void Gate ()
{
    const cen::EligibilityDiagnosis gate = cen::GetEligibilityDiagnosis ();
    if (!gate.haveClosest) {
        Say ("GATE", "no candidate groups were scored at all");
        return;
    }
    const cen::Eligibility want = cen::GetEligibility ();
    char line[320] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "closest g%u fails %u: samples %u/%u coverage %.0f%%/%.0f%% inside %.0f%%/%.0f%% "
                 "finite %.0f%%/%.0f%% area %.0f/%.0f px2 edge %.0f/%.0f px centre %.3f/%.2f",
                 gate.closestGroupId, gate.closestFailures, gate.closestSamples, want.minSamples,
                 gate.closestCoverage * 100.0f, want.minModelCoverage * 100.0f, gate.closestInsideClip * 100.0f,
                 want.minInsideClip * 100.0f, gate.closestFinite * 100.0f, want.minFiniteTriangles * 100.0f,
                 gate.closestAreaPixels, want.minMedianAreaPixels, gate.closestEdgePixels, want.minMedianMaxEdgePixels,
                 gate.closestCentreError, want.maxMedianCentreError);
    Say ("GATE", line);

    // Which term refused across ALL groups, and which refused ALONE -- the
    // second is the evidence, as it is for the fingerprint.
    char terms[256] = {};
    size_t used = 0;
    for (uint32_t i = 0; i < cen::kGateTermCount; ++i) {
        if (gate.missed[i] == 0)
            continue;
        char one[64] = {};
        _snprintf_s (one, sizeof (one), _TRUNCATE, "%s%s %u/%u", used == 0 ? "" : ", ", cen::GateTermName (i),
                     gate.soleMiss[i], gate.missed[i]);
        const size_t length = strlen (one);
        if (used + length + 1 >= sizeof (terms))
            break;
        memcpy (terms + used, one, length + 1);
        used += length;
    }
    if (used > 0)
        Say ("GATE", std::string ("refused (sole/total): ") + terms);
}

// ⚠️ WHAT IS HAPPENING NOW, WHICH NO CUMULATIVE COUNTER CAN
// SAY. A run reported `OVERLAY DRAWING (85 lines)` and the user saw nothing on
// screen -- both true: one pass had composed, and then the chain went silent
// because `blockedAt` only reports while `overlayDraws == 0`. Totals cannot
// distinguish "composing every frame" from "composed once, minutes ago", and
// that difference was the whole question.
//
// So: deltas since the last tick, and a line whenever the ANSWER changes --
// composing or not, a refusal that was zero becoming nonzero, the two viewports
// disagreeing -- plus one every five seconds so a healthy run still leaves a
// heartbeat to read. Never one per tick.
void Live (const Health& health)
{
    if (!health.running)
        return;

    const uint64_t present = health.presentInjections;
    const uint64_t compose = health.overlayDraws;
    char line[420] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%s present+%llu compose+%llu lines=%u curved=%u host=%u cam=%s vp=%ux%u target=%ux%u depth=%ux%u "
                 "rebind=%llu miss=0x%02x | stale+%llu nodepth+%llu nogeom+%llu noedge+%llu nocam+%llu culled+%llu "
                 "mismatch+%llu cam(new+%llu repeat+%llu LATE+%llu) "
                 "age(0=%llu 1=%llu 2=%llu 3+=%llu max=%u of %llu) suppressed+%llu redraw=%llu",
                 compose > g_mark.compose ? "composing" : "NOT COMPOSING",
                 (unsigned long long) (present - g_mark.present), (unsigned long long) (compose - g_mark.compose),
                 health.linesDrawn, health.silhouetteEdges, health.hostOpaqueTriangles, CameraStateName (health.camera),
                 health.acceptedViewportWidth, health.acceptedViewportHeight, health.targetWidth, health.targetHeight,
                 health.composeDepthWidth, health.composeDepthHeight, (unsigned long long) health.resizeRebinds,
                 health.lastMissMask, (unsigned long long) (health.skippedStaleCamera - g_mark.stale),
                 (unsigned long long) (health.hostNoDepthTarget - g_mark.noDepth),
                 (unsigned long long) (health.hostNoGeometry - g_mark.noGeometry),
                 (unsigned long long) (health.overlayNoEdges - g_mark.noEdges),
                 (unsigned long long) (health.overlayNoCamera - g_mark.noCamera),
                 (unsigned long long) (health.overlayCulled - g_mark.culled),
                 (unsigned long long) (health.composeSizeMismatches - g_mark.mismatch),
                 (unsigned long long) (health.sceneNew - g_mark.sceneNew),
                 (unsigned long long) (health.sceneRepeat - g_mark.sceneRepeat),
                 (unsigned long long) (health.sceneLate - g_mark.sceneLate), (unsigned long long) health.age0,
                 (unsigned long long) health.age1, (unsigned long long) health.age2,
                 (unsigned long long) health.age3plus, health.cameraAgeMax,
                 (unsigned long long) health.cameraAgeSamples,
                 (unsigned long long) (health.suppressedStaleViewport - g_mark.suppressed),
                 (unsigned long long) health.redrawRequests);

    g_mark.present = present;
    g_mark.compose = compose;
    g_mark.stale = health.skippedStaleCamera;
    g_mark.noDepth = health.hostNoDepthTarget;
    g_mark.noGeometry = health.hostNoGeometry;
    g_mark.noEdges = health.overlayNoEdges;
    g_mark.noCamera = health.overlayNoCamera;
    g_mark.culled = health.overlayCulled;
    g_mark.mismatch = health.composeSizeMismatches;
    g_mark.sceneNew = health.sceneNew;
    g_mark.sceneRepeat = health.sceneRepeat;
    g_mark.sceneLate = health.sceneLate;
    g_mark.suppressed = health.suppressedStaleViewport;

    // The leading word classifies the tick; comparing whole lines would narrate
    // every frame-count wobble, and comparing nothing would narrate four times a
    // second.
    const std::string current (line);
    const bool classChanged = g_lastLive.empty () || g_lastLive.compare (0, 13, current, 0, 13) != 0;
    const bool quiet =
        current.find ("stale+0 nodepth+0 nogeom+0 noedge+0 nocam+0 culled+0 mismatch+0") != std::string::npos &&
        current.find ("suppressed+0 ") != std::string::npos;
    ++g_liveTicks;
    if (classChanged || (!quiet && g_liveTicks >= 4) || g_liveTicks >= 20) {
        g_liveTicks = 0;
        g_lastLive = current;
        Say ("LIVE", current);
    }
}

void Chain (const Health& health)
{
    // ⚠️ WHILE REQUESTED BUT NOT DRAWING, THE CHAIN REPORTS ITSELF --
    // ONE LINE PER CHANGE, NEVER PER TICK. A state that has not moved is not
    // news; a state that has is the whole diagnosis.
    if (health.overlayDraws == 0) {
        char line[256] = {};
        _snprintf_s (line, sizeof (line), _TRUNCATE,
                     "frames=%llu draws=%llu/%llu groups=%u/+%llu attempts=%llu eligible=%u selection=%s "
                     "source=%s arm=%s matches=%llu snapshots=%llu present=%llu",
                     (unsigned long long) health.modelFramesSeen, (unsigned long long) health.drawsQualified,
                     (unsigned long long) health.drawsSeen, health.groupsUsed,
                     (unsigned long long) health.groupsOverflowed, (unsigned long long) health.selectionAttempts,
                     health.eligibleCandidates, health.selectionValid ? "valid" : "none", health.cameraSource.c_str (),
                     health.armState.c_str (), (unsigned long long) health.logicalMatches,
                     (unsigned long long) health.authoritativeSnapshots, (unsigned long long) health.presentInjections);
        if (g_lastChain != line) {
            g_lastChain = line;
            Say ("CHAIN", line);
            Say ("BLOCKED AT", health.blockedAt);
            // ⚠️ AND WHEN THE GATE IS WHAT REFUSED, WHICH TERM AND ON
            // WHAT NUMBERS. `eligible=0` names a stage and not a cause; these are
            // the measurements the gate was applied to, so the threshold can be
            // argued with instead of guessed at.
            if (!health.selectionValid && health.eligibleCandidates == 0)
                Gate ();
        }
    }
}

void Watch (const Health& health)
{
    // ⚠️ "ARMED" IS NOT "WORKING", AND A WHOLE RUN PROVED IT.
    // The log read `model watch: armed, polling every 750 ms` and then not one
    // `re-extracting` line, while the model went from 112 triangles to 96. Only
    // the extraction triggered by a VIEW SWITCH ever picked the edit up. These
    // numbers separate the three ways that happens: the watch never polled, it
    // polled and the generator reported nothing, or it saw an edit and the pass
    // could not start.
    char line[320] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%s every %u ms: polls=%u edits=%u refreshes=%u envOnly=%u busy=%u | "
                 "rev model=%u published=%u gpu=%u adopted=%llu reselect=%llu%s%s",
                 health.watchRunning ? "watching" : "NOT WATCHING", health.watchIntervalMs, health.watchPolls,
                 health.watchEdits, health.watchRefreshes, health.watchEnvironmentOnly, health.watchSkippedBusy,
                 health.modelRevision, health.publishedRevision, health.gpuRevision,
                 (unsigned long long) health.modelEditRebinds, (unsigned long long) health.modelEditReselects,
                 health.watchError.empty () ? "" : " | error: ", health.watchError.c_str ());
    const std::string current (line);
    if (current == g_lastWatch)
        return;
    g_lastWatch = current;
    Say ("WATCH", current);
}

void Backend (const Health& health)
{
    if (health.overlayBackend != "diligent")
        return;
    char line[320] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%s attach=%u ms attempts=%u failures=%u | wraps=%u hits=%u failed=%u distinct=%u "
                 "dropped on resize=%u%s%s",
                 health.diligentAttached ? "ATTACHED to Archicad's device" : "NOT ATTACHED", health.diligentAttachMs,
                 health.diligentAttachAttempts, health.diligentAttachFailures, health.diligentWraps,
                 health.diligentWrapHits, health.diligentWrapFailures, health.diligentDistinctBackBuffers,
                 health.diligentWrapDropsOnResize, health.diligentError.empty () ? "" : " | ",
                 health.diligentError.c_str ());
    const std::string current (line);
    if (current == g_lastBackend)
        return;
    g_lastBackend = current;
    Say ("DILIGENT", current);
}

void Pulse (const Health& health)
{
    char line[400] = {};
    // `blankPresents`, not "raised": the request is COALESCED, so the two numbers
    // count different things and the first reading of this line took the
    // difference for fourteen lost requests. `suppressedStaleViewport` is
    // Presents that drew nothing; `redrawsTaken` is requests consumed.
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "ticks=%llu (timer %llu, caller %llu) asked every 250 ms, worst gap=%u ms, over 1 s=%llu | "
                 "redraw: blankPresents=%llu requestsTaken=%llu worst wait=%u ms | "
                 "repeat(held %llu, PASS MOVED %llu, window moved %llu)",
                 (unsigned long long) health.ticks, (unsigned long long) health.timerTicks,
                 (unsigned long long) (health.ticks - health.timerTicks), health.tickGapMaxMs,
                 (unsigned long long) health.tickGapsOverASecond, (unsigned long long) health.suppressedStaleViewport,
                 (unsigned long long) health.redrawsTaken, health.redrawWaitMaxMs,
                 (unsigned long long) health.repeatHeld, (unsigned long long) health.repeatPassMoved,
                 (unsigned long long) health.repeatWindowMoved);
    const std::string current (line);
    if (current == g_lastPulse)
        return;
    g_lastPulse = current;
    Say ("PULSE", current);
}

// ⚠️ THE ONE LINE TWO RUNS ARE COMPARED ON. Which draw
// family got pinned decided whether the overlay tracked or lagged, and the only
// record of it was `CAMERA Locked ...`, printed once, with no candidate count,
// no runner-up and no statement of why that one won. A decision nobody can
// argue with is a decision nobody can check.
//
// ⚠️ KEYED ON THE SERIAL, WHICH THE BINDER BUMPS ON EVERY
// DECISION INCLUDING A REFUSAL. A refusal is evidence: "the window is still
// open" and "the window closed and nothing qualified" are different faults and
// used to look identical from outside.
void CameraBind ()
{
    const cen::BindReport bind = cen::GetBindReport ();
    if (bind.serial == g_lastBindSerial)
        return;
    g_lastBindSerial = bind.serial;
    char line[300] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "candidate=g%u interp=%u occurrence=%u coverage=%.0f%% valid=%.0f%% centre=%.3f "
                 "candidates=%u runnerUp=%.0f%% calibrationFrame=%llu/%llu selected=%s reason=%s",
                 bind.groupId, bind.variant, bind.occurrenceIndex, bind.coverage * 100.0f, bind.insideClip * 100.0f,
                 bind.centreError, bind.candidates, bind.runnerUpCoverage * 100.0f,
                 (unsigned long long) bind.calibrationFrames, (unsigned long long) bind.calibrationTarget,
                 bind.selected ? "yes" : "no", cen::BindReasonName (bind.reason));
    Say ("CAMERA_BIND", line);
}

// ⚠️ THE CAMERA AND THE IMAGE, SAID APART. Every other
// counter reports that composition happened; this one reports whether what was
// composed belonged to the frame it was composed into. See CameraFreshness.hpp.
void EpochGate ()
{
    const dxgi::injection::freshness::EpochGateReport gate = dxgi::injection::freshness::GetEpochGate ();
    const uint64_t total = gate.matched + gate.mismatched;
    if (total == 0)
        return;
    char line[240] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%s | matched=%llu mismatched=%llu (%.0f%%) suppressed=%llu | worst behind=%llu ahead=%llu "
                 "| camera epoch=%llu presented=%llu",
                 gate.enabled ? "ENFORCING" : "counting only", (unsigned long long) gate.matched,
                 (unsigned long long) gate.mismatched, 100.0 * double (gate.mismatched) / double (total),
                 (unsigned long long) gate.suppressed, (unsigned long long) gate.behindMax,
                 (unsigned long long) gate.aheadMax, (unsigned long long) gate.cameraEpoch,
                 (unsigned long long) gate.presentedEpoch);
    const std::string current (line);
    if (current == g_lastEpochGate)
        return;
    g_lastEpochGate = current;
    Say ("EPOCH", current);
}

// ⚠️ THE MATRICES THEMSELVES, ONCE EACH. Every other
// number in this file describes what a matrix DOES to an approximate anchor.
// None of them says what it IS, and "do the draws of one pass bind four
// transforms or one matrix written four times" is answered by sixteen floats.
//
// ⚠️ ONLY ROWS SHARING A GENERATION ARE COMPARABLE. Two
// matrices read in different model frames differ because the camera moved
// between them, which says nothing. `gen=` is printed so a reader can discard
// the pairs that cannot be compared.
void Matrices ()
{
    const size_t held = dxgi::injection::freshness::GroupMatrixCount ();
    if (held <= g_matricesSaid)
        return;
    dxgi::injection::freshness::GroupMatrices rows[24];
    const size_t count = dxgi::injection::freshness::GetGroupMatrices (rows, 24);
    for (size_t i = g_matricesSaid; i < count; ++i) {
        const dxgi::injection::freshness::GroupMatrices& row = rows[i];
        char line[420] = {};
        _snprintf_s (line, sizeof (line), _TRUNCATE,
                     "g%u occ%u gen=%llu VIEW %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                     "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                     row.groupId, row.occurrence, (unsigned long long) row.generation, row.view[0], row.view[1],
                     row.view[2], row.view[3], row.view[4], row.view[5], row.view[6], row.view[7], row.view[8],
                     row.view[9], row.view[10], row.view[11], row.view[12], row.view[13], row.view[14], row.view[15]);
        Say ("MATRIX", line);
        _snprintf_s (line, sizeof (line), _TRUNCATE,
                     "g%u occ%u gen=%llu PROJ %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                     "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                     row.groupId, row.occurrence, (unsigned long long) row.generation, row.projection[0],
                     row.projection[1], row.projection[2], row.projection[3], row.projection[4], row.projection[5],
                     row.projection[6], row.projection[7], row.projection[8], row.projection[9], row.projection[10],
                     row.projection[11], row.projection[12], row.projection[13], row.projection[14],
                     row.projection[15]);
        Say ("MATRIX", line);
    }
    g_matricesSaid = count;
}

// ⚠️ DOES THE COMPOSITE MOVE FASTER THAN THE MODEL?
// If it does, Archicad is re-projecting a cached scene image every frame with a
// camera the model draws never bind -- which is what a smooth preview against a
// stepping overlay looks like. RenderStateCapture.hpp carries the reasoning.
void Composite ()
{
    const dxgi::renderstate::CompositeDraw c = dxgi::renderstate::GetCompositeDraw ();
    if (c.draws == 0)
        return;
    char line[240] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "draws=%llu verts=%u windowChanges=%llu | b0 %s%llx b1 %s%llx b2 %s%llx", (unsigned long long) c.draws,
                 c.vertexCount, (unsigned long long) c.windowChanges, c.b0Bound ? "" : "unbound ",
                 (unsigned long long) (c.b0Window ? c.b0Window - 1 : 0), c.b1Bound ? "" : "unbound ",
                 (unsigned long long) (c.b1Window ? c.b1Window - 1 : 0), c.b2Bound ? "" : "unbound ",
                 (unsigned long long) (c.b2Window ? c.b2Window - 1 : 0));
    const std::string current (line);
    if (current == g_lastComposite)
        return;
    g_lastComposite = current;
    Say ("COMPOSITE", current);
}

void Sync ()
{
    CameraBind ();
    Composite ();
    Matrices ();
    EpochGate ();
    const dxgi::injection::freshness::Report cam = dxgi::injection::freshness::Snapshot ();
    // ⚠️ THE PIN, BECAUSE IT IS WHAT GATES THE BYTES.
    // `CopyCameraWindows` runs only for a draw that passed `MatchesSelection`,
    // so a pin that stops matching stops the refresh of the 256 bytes the shader
    // multiplies by -- and NOTHING downstream reports that, because every
    // composition counter stays perfectly healthy while the overlay holds the
    // last camera it was given. See `freshness::NoteAuthoritativeSnapshot`.
    const cen::BindingStats binding = cen::GetBindingStats ();
    const uint32_t pinMiss = binding.pinMissMask;
    // ⚠️ WHICH CAMERA IT SETTLED ON, EVERY SECOND, NOT ONLY
    // AT THE TRANSITION. `CAMERA Locked ...` is printed once, when the state word
    // changes -- so a selection UPGRADED inside the settling window, which
    // leaves the state word at `Locked`, would swap the pin under the overlay
    // and print nothing at all. The occurrence and its coverage are the two
    // numbers that decided the run: in the two logs this was written from, the
    // same binary locked occ3 at 97% when the overlay tracked and occ8 at 89%
    // when it lagged.
    const cen::Selection pin = cen::GetSelection ();
    char line[560] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "%s | BYTES: snapshots=%llu, %u ms since the last one, worst gap %u ms, pinMiss=0x%02x "
                 "| content: decodes=%llu changes=%llu, %u ms since capture "
                 "| adopted=%llu same=%llu FRESH_NOT_ADOPTED=%llu, run now=%llu worst=%llu "
                 "| recovery: run=%llu after %u ms, signature %s, pass %s, window %s "
                 "| pin: occ%u cov=%.0f%% inside=%.0f%% %s",
                 cam.msSinceSnapshot > 500 ? "BYTES STALLED" : (cam.camFreshNotAdopted > 0 ? "desync seen" : "in sync"),
                 (unsigned long long) cam.snapshots, cam.msSinceSnapshot, cam.msSinceSnapshotMax, pinMiss,
                 (unsigned long long) cam.contentDecodes, (unsigned long long) cam.contentChanges,
                 cam.msSinceLatestCaptureMax, (unsigned long long) cam.camAdopted, (unsigned long long) cam.camSame,
                 (unsigned long long) cam.camFreshNotAdopted, (unsigned long long) cam.freshRunCurrent,
                 (unsigned long long) cam.freshRunMax, (unsigned long long) cam.recoveryRunLength, cam.recoveryMs,
                 cam.recoverySignatureChanged ? "MOVED" : "held", cam.recoveryPassMoved ? "MOVED" : "held",
                 cam.recoveryWindowMoved ? "MOVED" : "held", pin.occurrenceIndex, pin.modelCoverage * 100.0f,
                 pin.insideClip * 100.0f, binding.calibrated ? "calibrated" : "PROVISIONAL");

    // ⚠️ KEYED ON THE STATE WORD AND RATE-LIMITED, THE
    // WAY `Live` IS. `ms since the last one` advances on every tick by
    // construction, so comparing whole lines would narrate four times a second
    // for the life of the session and comparing nothing would narrate never.
    const std::string current (line);
    const size_t bar = current.find (" |");
    const std::string state (current.substr (0, bar == std::string::npos ? current.size () : bar));
    const bool stateChanged = g_lastSync.empty () || state != g_lastSync;
    const bool quiet = state == "in sync";
    ++g_syncTicks;
    if (!stateChanged && !(!quiet && g_syncTicks >= 4) && g_syncTicks < 20)
        return;
    g_syncTicks = 0;
    g_lastSync = state;
    Say ("SYNC", current);
}

void FramePath ()
{
    namespace rs = dxgi::renderstate;

    // ---- every swap chain, not only the one learned while stationary --------
    //
    // ⚠️ IF ORBIT PRESENTS ON ANOTHER CHAIN, THIS LINE
    // SAYS SO AND NOTHING ELSE IN THE TREE DOES. `nominated` is the one the
    // overlay composites into; a chain whose `presents` climbs while the
    // nominated one's does not is the explanation, immediately.
    dxgi::ChainInfo chains[8] = {};
    const size_t chainCount = dxgi::GetChainInventory (chains, 8);
    std::string chainLine;
    for (size_t i = 0; i < chainCount; ++i) {
        char one[160] = {};
        _snprintf_s (one, sizeof (one), _TRUNCATE, "%s#%llx hwnd=%llx %ux%u fmt=%u presents=%llu%s%s",
                     i == 0 ? "" : "  |  ", (unsigned long long) chains[i].swapChain,
                     (unsigned long long) chains[i].window, chains[i].width, chains[i].height, chains[i].format,
                     (unsigned long long) chains[i].presents, chains[i].ours ? " OURS" : "",
                     chains[i].nominated ? " <- NOMINATED" : "");
        chainLine += one;
    }
    if (chainLine.empty ())
        chainLine = "none seen yet";
    if (chainLine != g_lastChains) {
        g_lastChains = chainLine;
        Say ("CHAINS", chainLine);
    }

    // ---- the scene pass, which is the graph itself -------------------------
    //
    // ⚠️ READ IT BACKWARDS FROM PRESENT, WHICH IS WHAT
    // THESE FIELDS ARE FOR. `boundary` is the colour target being bound away --
    // the last moment the scene's own colour, depth and viewport are still the
    // bound state. `opsAfter` counts the copies and resolves between that and
    // Present: ZERO means the pass rendered straight into the presented back
    // buffer (what run twenty-one measured while stationary), NON-ZERO means
    // there is a resolve or composite in between and the overlay belongs before
    // it, not at Present.
    const rs::ScenePass pass = rs::LastCompletedScenePass ();
    const rs::LatchCensus latch = rs::GetLatchCensus ();
    char sceneLine[760] = {};
    _snprintf_s (sceneLine, sizeof (sceneLine), _TRUNCATE,
                 "pass#%llu frame=%llu rtv=%llx dsv=%llx colour=%llx | draws=%u camera=%s "
                 "| boundary=%s opsAfter=%u drawsAfter=%u returns=%u drawsAfterReturn=%u | consumed=%s epoch=%llu"
                 " | latch: byCopy=%llu byDeparture=%llu copiesInPass=%llu"
                 " | camera in pass: moved=%u lastAtDraw=%u first=%llx last=%llx"
                 " | b0: bound=%s moved=%u n=%u at=%llx",
                 (unsigned long long) pass.generation, (unsigned long long) pass.presentFrameId,
                 (unsigned long long) pass.colorTarget, (unsigned long long) pass.depthTarget,
                 (unsigned long long) pass.colorResource, pass.draws, pass.drawsHadCamera ? "yes" : "NO",
                 pass.boundaryHit ? "hit" : "NOT HIT", pass.opsAfterBoundary, pass.drawsAfterBoundary,
                 pass.targetReturns, pass.drawsAfterReturn, pass.sceneConsumed ? "yes" : "no",
                 (unsigned long long) pass.targetEpoch, (unsigned long long) latch.consumedByCopy,
                 (unsigned long long) latch.consumedByDeparture, (unsigned long long) latch.copiesSeenInPass,
                 pass.cameraWindowChanges, pass.cameraWindowLastChangeDraw, (unsigned long long) pass.cameraWindowFirst,
                 (unsigned long long) pass.cameraWindowLast, pass.b0Bound ? "yes" : "no", pass.b0WindowChanges,
                 pass.b0NumConstants, (unsigned long long) pass.b0WindowFirst);
    const std::string scene (sceneLine);
    if (scene != g_lastScenePass) {
        g_lastScenePass = scene;
        Say ("SCENEPASS", scene);
    }

    // ---- and what the frame as a whole did ---------------------------------
    const rs::FrameState frame = rs::LatestFrame ();
    const rs::SceneSignature signature = rs::GetSceneSignature ();
    char signatureLine[400] = {};
    _snprintf_s (signatureLine, sizeof (signatureLine), _TRUNCATE,
                 "learned=%s colour=%llx dsv=%llx %.0fx%.0f draws=%u stable=%u of %u watched, candidates=%u "
                 "| frame%llu: scene rtv=%llx dsv=%llx, last rtv=%llx dsv=%llx, binds=%u viewports=%u distinct=%u",
                 signature.learned ? "yes" : "NO", (unsigned long long) signature.colorResource,
                 (unsigned long long) signature.depthTarget, signature.viewportWidth, signature.viewportHeight,
                 signature.draws, signature.stableFrames, signature.framesWatched, signature.candidatesThisFrame,
                 (unsigned long long) frame.frameId, (unsigned long long) frame.sceneColorTarget,
                 (unsigned long long) frame.sceneDepthTarget, (unsigned long long) frame.lastColorTarget,
                 (unsigned long long) frame.lastDepthTarget, frame.targetBinds, frame.viewportSets,
                 frame.distinctCount);
    const std::string sig (signatureLine);
    if (sig != g_lastSignature) {
        g_lastSignature = sig;
        Say ("SIGNATURE", sig);
    }
}

void Variants ()
{
    const cen::Selection chosen = cen::GetSelection ();
    if (!chosen.valid)
        return;

    static cen::Group groups[cen::kGroupCapacity];
    const size_t count = cen::CopyGroups (groups, cen::kGroupCapacity);
    const cen::Group* selected = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (groups[i].groupId == chosen.groupId) {
            selected = &groups[i];
            break;
        }
    }
    if (selected == nullptr)
        return;

    // ⚠️ THE WINNER IS MARKED, BECAUSE THE POINT IS THE
    // COMPARISON. A column of eight numbers says nothing; the same column with
    // "this is the one being drawn with" against one row is the measurement.
    char line[460] = {};
    size_t used = 0;
    for (size_t variant = 0; variant < cen::kVariantCount && used + 56 < sizeof (line); ++variant) {
        char one[64] = {};
        _snprintf_s (one, sizeof (one), _TRUNCATE, "%sv%u%s n=%u err=%.3f spread=%.0fpx", used == 0 ? "" : "  ",
                     unsigned (variant), variant == selected->winningVariant ? "*" : "",
                     selected->variantValid[variant], selected->variantMeanCentreError[variant],
                     selected->variantMeanSpreadPixels[variant]);
        const size_t length = strlen (one);
        if (used + length + 1 >= sizeof (line))
            break;
        memcpy (line + used, one, length + 1);
        used += length;
    }

    const std::string current (std::string ("g") + std::to_string (chosen.groupId) + " (* = drawn with)  " + line);
    if (current == g_lastVariants)
        return;
    g_lastVariants = current;
    Say ("VARIANTS", current);
}

void Reset ()
{
    g_matricesSaid = 0;
    g_lastBindSerial = 0;
    g_lastSync.clear ();
    g_syncTicks = 0;
    g_lastChains.clear ();
    g_lastScenePass.clear ();
    g_lastSignature.clear ();
    g_lastVariants.clear ();
    g_lastPulse.clear ();
    g_lastBackend.clear ();
    g_lastWatch.clear ();
    g_mark = LiveMark {};
    g_lastLive.clear ();
    g_liveTicks = 0;
    g_lastChain.clear ();
}

} // namespace report
} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv
