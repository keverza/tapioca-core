#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/GateDiagnosticCommands.hpp"

#include "NativeCommands/CommandRegistration.hpp"

#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "Python/MainThreadGate.hpp"

#include <string>

namespace geomsrv {
namespace {

namespace av = archviz;

// ⚠️ GATE-FREE, AND THAT IS THE ENTIRE DESIGN CONSTRAINT. A diagnostic that
// needed the main thread could not observe a stalled main thread: it would queue
// behind the very thing it was sent to measure and report a timeout instead of
// an answer. Everything below reads atomics and a short-held mutex, so this
// answers over the loopback server while Archicad's UI is frozen solid.
constexpr const char kGateStateInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";

constexpr const char kGateStateResponse[] =
    R"json({"type":"object","properties":{"posted":{"type":"integer","minimum":0},"dispatched":{"type":"integer","minimum":0},"pending":{"type":"integer","minimum":0},"inlineRuns":{"type":"integer","minimum":0},"timeouts":{"type":"integer","minimum":0},"postFailures":{"type":"integer","minimum":0},"queueDepth":{"type":"integer","minimum":0},"msSinceLastDispatch":{"type":"integer"},"msSinceLastPost":{"type":"integer"},"longestWaitMs":{"type":"integer","minimum":0},"dispatching":{"type":"boolean"},"shuttingDown":{"type":"boolean"},"mainThreadKnown":{"type":"boolean"},"mainThreadCommand":{"type":"string"},"mainThreadCommandMs":{"type":"integer"},"extractionRunning":{"type":"boolean"},"extractionPhase":{"type":"string"},"extractionExtracted":{"type":"integer","minimum":0},"extractionTotal":{"type":"integer","minimum":0},"extractionSlices":{"type":"integer","minimum":0},"extractionLongestHoldMs":{"type":"integer"},"extractionLongestRoundTripMs":{"type":"integer"},"viewportRunning":{"type":"boolean"},"captureStatus":{"type":"string"},"captureStage":{"type":"string"},"capturePossible":{"type":"boolean"},"verdict":{"type":"string"}},"additionalProperties":false,"required":["posted","dispatched","pending","inlineRuns","timeouts","postFailures","queueDepth","msSinceLastDispatch","msSinceLastPost","longestWaitMs","dispatching","shuttingDown","mainThreadKnown","mainThreadCommand","mainThreadCommandMs","extractionRunning","extractionPhase","extractionExtracted","extractionTotal","extractionSlices","extractionLongestHoldMs","extractionLongestRoundTripMs","viewportRunning","captureStatus","captureStage","capturePossible","verdict"]})json";

// The one sentence a reader wants first.
//
// ⚠️ COMPOSED HERE RATHER THAN LEFT TO THE CALLER, because the numbers only
// mean something together: work waiting with nothing dispatching for seconds is
// a stalled event loop, and the same queue depth with dispatch ticking over is
// simply a busy gate. Stating the reading is what stops the next person
// re-deriving it under pressure.
std::string Verdict (const evp::MainThreadGate::Stats& gate)
{
    if (gate.shuttingDown)
        return "the add-on is shutting down; the gate is closed on purpose";
    if (!gate.mainThreadKnown)
        return "the main thread was never recorded - the gate cannot dispatch at all";
    if (gate.queueDepth == 0)
        return "nothing is waiting; the gate is idle";
    if (gate.msSinceLastDispatch >= 0 && gate.msSinceLastDispatch < 1000)
        return "work is queued and dispatching normally";
    if (!gate.mainThreadCommand.empty ())
        return "STALLED: Archicad's UI thread has been inside the native command '" + gate.mainThreadCommand +
               "' for " + std::to_string (gate.mainThreadCommandMs) + " ms, so the event loop cannot dispatch";
    return "STALLED: work is queued and nothing has dispatched for " + std::to_string (gate.msSinceLastDispatch) +
           " ms - Archicad's UI thread is busy or blocked, but not inside a Tapioca command";
}

class MainThreadGateStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "MainThreadGateState";
    }
    // The whole point. See the note above.
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const evp::MainThreadGate::Stats gate = evp::MainThreadGate::Get ().Snapshot ();

        GS::ObjectState os;
        os.Add ("posted", static_cast<GS::Int64> (gate.posted));
        os.Add ("dispatched", static_cast<GS::Int64> (gate.dispatched));
        // The number that matters most: posted minus dispatched. Climbing while
        // dispatched stands still IS the stall.
        os.Add ("pending", static_cast<GS::Int64> (gate.posted - gate.dispatched));
        os.Add ("inlineRuns", static_cast<GS::Int64> (gate.inlineRuns));
        os.Add ("timeouts", static_cast<GS::Int64> (gate.timeouts));
        os.Add ("postFailures", static_cast<GS::Int64> (gate.postFailures));
        os.Add ("queueDepth", static_cast<GS::Int64> (gate.queueDepth));
        os.Add ("msSinceLastDispatch", static_cast<GS::Int64> (gate.msSinceLastDispatch));
        os.Add ("msSinceLastPost", static_cast<GS::Int64> (gate.msSinceLastPost));
        os.Add ("longestWaitMs", static_cast<GS::Int64> (gate.longestWaitMs));
        os.Add ("dispatching", gate.msSinceLastDispatch >= 0 && gate.msSinceLastDispatch < 1000);
        os.Add ("shuttingDown", gate.shuttingDown);
        os.Add ("mainThreadKnown", gate.mainThreadKnown);
        os.Add ("mainThreadCommand", GS::UniString (gate.mainThreadCommand.c_str (), CC_UTF8));
        os.Add ("mainThreadCommandMs", static_cast<GS::Int64> (gate.mainThreadCommandMs));

        // The extraction's own view, in the same reading. "The gate stopped
        // dispatching" is reported BY the extraction, so the two have to be
        // legible side by side or the reader is correlating two logs by hand.
        const archviz::ExtractionWorker::Progress extraction = archviz::ExtractionWorker::Get ().Snapshot ();
        os.Add ("extractionRunning", extraction.running);
        os.Add ("extractionPhase", GS::UniString (extraction.phase.c_str (), CC_UTF8));
        os.Add ("extractionExtracted", static_cast<GS::Int64> (extraction.extracted));
        os.Add ("extractionTotal", static_cast<GS::Int64> (extraction.total));
        os.Add ("extractionSlices", static_cast<GS::Int64> (extraction.slices));
        os.Add ("extractionLongestHoldMs", static_cast<GS::Int64> (extraction.longestHoldMs));
        os.Add ("extractionLongestRoundTripMs", static_cast<GS::Int64> (extraction.longestRoundTripMs));

        // ⚠️ WHY A CAPTURE CANNOT START, in the same reading. "a
        // Diligent viewport or extraction pass is already running" is refused by
        // the same flag that refuses the 3D viewer and the overlay from the
        // menu, so a stuck pipeline presents as three unrelated-looking faults.
        // Reporting it here is what collapses them back into one.
        const av::DiligentCaptureStats capture = av::DiligentViewport::Get ().CaptureStats ();
        const bool viewportRunning = av::DiligentViewport::Get ().IsRunning ();
        os.Add ("viewportRunning", viewportRunning);
        os.Add ("captureStatus", GS::UniString (capture.status.c_str (), CC_UTF8));
        os.Add ("captureStage", GS::UniString (capture.stage.c_str (), CC_UTF8));
        os.Add ("capturePossible", !viewportRunning && !extraction.running);

        os.Add ("verdict", GS::UniString (Verdict (gate).c_str (), CC_UTF8));
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.ResetDiligentPipeline - stop whatever is holding the renderer.
//
// ⚠️ ONE STUCK FLAG PRESENTS AS THREE UNRELATED FAULTS, which is why
// this exists as a verb rather than as advice to restart Archicad.
// DiligentViewport::StartUnlocked refuses while `running_` is set, and the SAME
// refusal is what the 3D viewer, the 3D overlay and a headless capture all hit -
// so a capture that failed while the viewport was still winding down takes the
// menu items down with it and nothing says why.
//
// It stops rather than cancels: Stop() joins the render thread and the
// extraction worker, so when this returns the flags are provably clear rather
// than merely asked to clear. Safe to call when nothing is running - both stops
// are no-ops then, which is what makes it usable as a first move rather than a
// last resort.
constexpr const char kResetInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
constexpr const char kResetResponse[] =
    R"json({"type":"object","properties":{"viewportWasRunning":{"type":"boolean"},"extractionWasRunning":{"type":"boolean"},"capturePossible":{"type":"boolean"}},"additionalProperties":false,"required":["viewportWasRunning","extractionWasRunning","capturePossible"]})json";

class ResetDiligentPipelineCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ResetDiligentPipeline";
    }
    // Joins threads; it must not do that while holding Archicad's UI thread.
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        const bool viewportWasRunning = av::DiligentViewport::Get ().IsRunning ();
        const bool extractionWasRunning = archviz::ExtractionWorker::Get ().IsRunning ();

        // The extraction FIRST: the render thread's offscreen loop waits on its
        // progress, so stopping the producer lets the consumer reach its own
        // exit instead of being joined mid-wait.
        archviz::ExtractionWorker::Get ().Stop ();
        av::DiligentViewport::Get ().Stop ();

        GS::ObjectState os;
        os.Add ("viewportWasRunning", viewportWasRunning);
        os.Add ("extractionWasRunning", extractionWasRunning);
        os.Add ("capturePossible",
                !av::DiligentViewport::Get ().IsRunning () && !archviz::ExtractionWorker::Get ().IsRunning ());
        return os;
    }
};

const NativeCommandRegistration registrations[] = {
    { "MainThreadGateState", &MakeRegisteredNativeCommand<MainThreadGateStateCommand>, false, kGateStateInput,
      kGateStateResponse },
    { "ResetDiligentPipeline", &MakeRegisteredNativeCommand<ResetDiligentPipelineCommand>, false, kResetInput,
      kResetResponse },
};

} // namespace

NativeCommandRegistrations GetGateDiagnosticCommandRegistrations ()
{
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
