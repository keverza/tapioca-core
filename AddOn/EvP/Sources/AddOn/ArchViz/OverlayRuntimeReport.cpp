// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See OverlayRuntimeReport.hpp.

#include "ArchViz/OverlayRuntimeReport.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchVizLog.hpp"

#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {
namespace report {

namespace cen = dxgi::census;

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
                 "%s attach=%u ms failures=%u | wraps=%u hits=%u failed=%u distinct=%u "
                 "dropped on resize=%u%s%s",
                 health.diligentAttached ? "ATTACHED to Archicad's device" : "NOT ATTACHED", health.diligentAttachMs,
                 health.diligentAttachFailures, health.diligentWraps, health.diligentWrapHits,
                 health.diligentWrapFailures, health.diligentDistinctBackBuffers, health.diligentWrapDropsOnResize,
                 health.diligentError.empty () ? "" : " | ", health.diligentError.c_str ());
    const std::string current (line);
    if (current == g_lastBackend)
        return;
    g_lastBackend = current;
    Say ("DILIGENT", current);
}

void Reset ()
{
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
