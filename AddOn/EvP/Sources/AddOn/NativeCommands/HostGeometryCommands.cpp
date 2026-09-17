// NativeCommands/HostGeometryCommands -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/HostGeometryCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandBase.hpp"

#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"

namespace geomsrv {

namespace av = geomsrv::archviz;

namespace {

// ---------------------------------------------------------------------------
// Tapioca.RequestHostGeometry { full, start } -> { accepted, published..., ... }
//
// ⚠️ AN EXPLICIT VERB, BECAUSE "OPEN THE UI AND HOPE" WAS AN
// ASSUMPTION AND RUN FIFTY-FOUR FALSIFIED IT. The diagnostic opened the Diligent
// overlay to make an extraction happen and measured `batches 0` forty seconds
// later -- and worse, opening the overlay changes the camera sync mode, whose
// teardown calls `census::Shutdown`, which drops the fingerprint phase A had
// just selected. The run then reported "phase A's camera fingerprint did not
// survive" and looked like a rendering regression.
//
// ⚠️ SO HOST EXTRACTION MUST NOT SHARE A LIFECYCLE WITH OVERLAY
// SYNCHRONISATION, and this verb is that separation. It starts the extraction
// worker directly. It touches no camera-sync mode, opens no window, and tears
// down nothing -- so the recognizer state that phase A produces is untouchable
// by it, in either order.
// ---------------------------------------------------------------------------
class RequestHostGeometryCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "RequestHostGeometry";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace host = av::dxgi::hostocclusion;

        bool full = true;
        if (params.Contains ("full"))
            params.Get ("full", full);

        // ⚠️ STARTING IS OPT-IN, BECAUSE POLLING IT WAS STARTING IT. Run
        // fifty-six asked for an extraction and then read this same verb once a
        // second to watch it: every read that found the worker idle started
        // ANOTHER pass. 130 passes in 130 seconds, each one a full BeginBatch
        // that discarded the last one's geometry -- which is why the counters
        // read `batches 130` for an eight-element model. A status read must be
        // a status read.
        bool start = false;
        if (params.Contains ("start"))
            params.Get ("start", start);

        bool accepted = false;
        bool alreadyRunning = av::ExtractionWorker::Get ().IsRunning ();
        if (start && !alreadyRunning) {
            // ⚠️ ONE-SHOT, NOT LIVE. A live pass arms database
            // observers, which WRITES TO THE PROJECT -- see ExtractionThread.hpp
            // -- and a diagnostic may not do that to a user's model. The budget
            // is generous because a full building is not quick and the caller
            // polls rather than blocking.
            av::ExtractionWorker::Get ().Start (full, 12, 4, 600);
            accepted = true;
        }

        const host::Stats stats = host::GetStats ();
        GS::ObjectState os;
        os.Add ("accepted", accepted);
        os.Add ("alreadyRunning", alreadyRunning);
        os.Add ("running", av::ExtractionWorker::Get ().IsRunning ());
        // the extraction thread: what arrived
        os.Add ("extractionGeneration", (GS::Int32) stats.extractionGeneration);
        os.Add ("batchBegins", (GS::Int32) stats.batchBegins);
        os.Add ("batchEnds", (GS::Int32) stats.batchEnds);
        os.Add ("elementsReceived", (GS::Int32) stats.elementsReceived);
        os.Add ("opaqueVerticesAdded", (GS::Int32) stats.opaqueVerticesAdded);
        os.Add ("opaqueIndicesAdded", (GS::Int32) stats.opaqueIndicesAdded);
        os.Add ("opaqueTriangles", (GS::Int32) stats.opaqueTriangles);
        os.Add ("transparentTrianglesSkipped", (GS::Int32) stats.transparentTrianglesSkipped);
        os.Add ("pendingVertices", (GS::Int32) stats.pendingVertices);
        os.Add ("droppedOverCapacity", (GS::Int32) stats.droppedOverCapacity);
        // the extraction thread: what was handed over. ⚠️ THIS GROUP ALONE
        // ANSWERS "DID THE MODEL REACH THE OVERLAY".
        os.Add ("publishAttempted", (GS::Int32) stats.publishAttempted);
        os.Add ("publishSucceeded", (GS::Int32) stats.publishSucceeded);
        os.Add ("publishedGeneration", (GS::Int32) stats.publishedGeneration);
        os.Add ("publishedVertices", (GS::Int32) stats.publishedVertices);
        os.Add ("publishedIndices", (GS::Int32) stats.publishedIndices);
        os.Add ("publishedTriangles", (GS::Int32) stats.publishedTriangles);
        os.Add ("haveSnapshot", stats.haveSnapshot);
        os.Add ("publishFailureReason", GS::UniString (stats.publishFailureReason, CC_UTF8));
        // the render thread: what is on the GPU. ⚠️ THESE STAY ZERO UNTIL
        // INJECTION IS ACTIVE, so they can never gate extraction.
        os.Add ("uploaded", stats.uploaded);
        os.Add ("vertices", (GS::Int32) stats.vertices);
        os.Add ("triangles", (GS::Int32) stats.triangles);
        os.Add ("renders", (GS::Int32) stats.renders);
        os.Add ("lastError", GS::UniString (stats.lastError, CC_UTF8));
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.OverlayRuntime { action } -> { ok, code, ... }
//
// ⚠️ THE PRODUCT'S OVERLAY, NOT THE DIAGNOSTIC'S. `ViewerInjectTriangle`
// arms the injection with a deterministic test mesh, a depth sweep and probe
// primitives, because a regression command must show its working. This starts the
// same machinery the way a user's menu click does: no phase A, no orbit
// instruction, no ranking table, no hand-made selection.
//
// ⚠️ AND `hide` IS NOT `stop`. Hiding keeps the camera lock, the host
// snapshot and every compiled shader; stopping releases the hooks. A caller that
// used `stop` to mean `hide` would make re-showing slow enough to look broken.
// ---------------------------------------------------------------------------
class OverlayRuntimeCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "OverlayRuntime";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        namespace runtime = av::overlayruntime;

        GS::UniString action ("state");
        if (params.Contains ("action"))
            params.Get ("action", action);
        const std::string wanted (action.ToCStr (0, MaxUSize, CC_UTF8).Get ());

        GS::ObjectState os;
        os.Add ("ok", true);
        os.Add ("code", GS::UniString (runtime::StartErrorName (runtime::StartError::None), CC_UTF8));
        if (wanted == "start") {
            const runtime::StartResult started = runtime::Start ();
            os.Add ("ok", started.ok);
            os.Add ("code", GS::UniString (runtime::StartErrorName (started.code), CC_UTF8));
            os.Add ("message", GS::UniString (started.message.c_str (), CC_UTF8));
            os.Add ("retryable", started.retryable);
        }
        else if (wanted == "stop") {
            runtime::Stop ();
        }
        else if (wanted == "hide") {
            runtime::SetVisible (false);
        }
        else if (wanted == "show") {
            runtime::SetVisible (true);
        }
        else if (wanted != "state") {
            return NativeCommandResult::Failure (
                EVP_FAIL ("unknown overlay runtime action '" + action + "'; expected start, stop, hide, show or state",
                          "driving the overlay runtime"));
        }

        // ⚠️ THE STATE IS REPORTED ON EVERY ACTION, not only on
        // `state`. A caller that starts the runtime needs to know what it started
        // into, and a second round trip through `MainThreadGate` to find out is a
        // second chance for the answer to have changed.
        runtime::Tick ();
        const runtime::Health health = runtime::GetHealth ();
        os.Add ("running", health.running);
        os.Add ("visible", health.visible);
        os.Add ("waitingForContext", health.waitingForContext);
        os.Add ("camera", GS::UniString (runtime::CameraStateName (health.camera), CC_UTF8));
        os.Add ("host", GS::UniString (runtime::HostStateName (health.host), CC_UTF8));
        os.Add ("autoSelections", (GS::Int32) health.autoSelections);
        os.Add ("reacquisitions", (GS::Int32) health.reacquisitions);
        os.Add ("hostOpaqueTriangles", (GS::Int32) health.hostOpaqueTriangles);
        os.Add ("overlayDraws", (GS::Int32) health.overlayDraws);
        os.Add ("lastError", GS::UniString (runtime::StartErrorName (health.lastError), CC_UTF8));
        os.Add ("lastMessage", GS::UniString (health.lastMessage.c_str (), CC_UTF8));
        return os;
    }
};

const NativeCommandRegistration kHostGeometryCommandRegistrations[] = {
    { "OverlayRuntime", &MakeRegisteredNativeCommand<OverlayRuntimeCommand>, false,
      R"json({"type":"object","properties":{"action":{"type":"string","enum":["start","stop","hide","show","state"]}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"message":{"type":"string"},"retryable":{"type":"boolean"},"running":{"type":"boolean"},"visible":{"type":"boolean"},"waitingForContext":{"type":"boolean"},"camera":{"type":"string"},"host":{"type":"string"},"autoSelections":{"type":"integer"},"reacquisitions":{"type":"integer"},"hostOpaqueTriangles":{"type":"integer"},"overlayDraws":{"type":"integer"},"lastError":{"type":"string"},"lastMessage":{"type":"string"}},"additionalProperties":false,"required":["ok","running","camera","host"]})json" },
    { "RequestHostGeometry", &MakeRegisteredNativeCommand<RequestHostGeometryCommand>, false,
      R"json({"type":"object","properties":{"full":{"type":"boolean"},"start":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"accepted":{"type":"boolean"},"alreadyRunning":{"type":"boolean"},"running":{"type":"boolean"},"extractionGeneration":{"type":"integer"},"batchBegins":{"type":"integer"},"batchEnds":{"type":"integer"},"elementsReceived":{"type":"integer"},"opaqueVerticesAdded":{"type":"integer"},"opaqueIndicesAdded":{"type":"integer"},"opaqueTriangles":{"type":"integer"},"transparentTrianglesSkipped":{"type":"integer"},"pendingVertices":{"type":"integer"},"droppedOverCapacity":{"type":"integer"},"publishAttempted":{"type":"integer"},"publishSucceeded":{"type":"integer"},"publishedGeneration":{"type":"integer"},"publishedVertices":{"type":"integer"},"publishedIndices":{"type":"integer"},"publishedTriangles":{"type":"integer"},"haveSnapshot":{"type":"boolean"},"publishFailureReason":{"type":"string"},"uploaded":{"type":"boolean"},"vertices":{"type":"integer"},"triangles":{"type":"integer"},"renders":{"type":"integer"},"lastError":{"type":"string"}},"additionalProperties":false,"required":["accepted","running","haveSnapshot"]})json" },
};

} // namespace

NativeCommandRegistrations GetHostGeometryCommandRegistrations ()
{
    return MakeRegistrationView (kHostGeometryCommandRegistrations);
}

} // namespace geomsrv
