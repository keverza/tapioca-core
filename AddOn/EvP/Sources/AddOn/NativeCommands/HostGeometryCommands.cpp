// NativeCommands/HostGeometryCommands -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/HostGeometryCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandBase.hpp"

#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/InjectedOverlayRuntime.hpp"
#include "ArchViz/OverlayController.hpp"

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

        // ⚠️ THE RESULT IS DECIDED FIRST AND ADDED ONCE.
        // `GS::ObjectState::Add` does NOT overwrite an existing key: the previous
        // version wrote defaults and then "overrode" them inside the branch, so
        // every single response reported `ok=true, code=None` whatever had
        // happened -- including the one that prompted the rule that `None` must
        // never reach a user. A field written twice is a field nobody can trust.
        // ⚠️ AND IT IS IN THE INPUT SCHEMA, WHICH IS
        // WHERE A CALLER ACTUALLY MEETS IT. The schema is `additionalProperties:
        // false`, so a parameter the handler reads and the schema does not
        // declare is rejected by the bus BEFORE this function runs -- the command
        // reports `SchemaValidationFailed` and every assertion below it fails for
        // a reason that has nothing to do with the overlay. The check just below
        // is kept for an in-process caller that bypasses the bus; for a bus
        // caller the enum has already refused anything else.
        //
        // ⚠️ SET BEFORE `start`, BECAUSE THAT IS THE ONLY
        // MOMENT IT CAN TAKE EFFECT CLEANLY. The attach happens on the first
        // composition; switching mid-session would leave one backend attached
        // and the other drawing, which is exactly the "two renderers in one
        // viewport" state section 12 forbids.
        if (params.Contains ("backend")) {
            GS::UniString backend;
            params.Get ("backend", backend);
            const std::string which (backend.ToCStr (0, MaxUSize, CC_UTF8).Get ());
            if (which != "native" && which != "diligent")
                return NativeCommandResult::Failure (
                    EVP_FAIL ("unknown overlay backend '" + backend + "'; expected native or diligent",
                              "selecting the overlay drawing backend"));
            runtime::SetOverlayBackend (which == "diligent");
        }

        bool ok = true;
        runtime::StartError code = runtime::StartError::None;
        std::string message;
        bool retryable = false;
        if (wanted == "start") {
            const runtime::StartResult started = runtime::Start ();
            ok = started.ok;
            code = started.code;
            message = started.message;
            retryable = started.retryable;
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
        GS::ObjectState os;
        os.Add ("ok", ok);
        os.Add ("code", GS::UniString (runtime::StartErrorName (code), CC_UTF8));
        os.Add ("message", GS::UniString (message.c_str (), CC_UTF8));
        os.Add ("retryable", retryable);

        namespace control = av::overlaycontrol;
        const control::Status routing = control::GetStatus ();
        os.Add ("view", GS::UniString (control::ViewKindName (routing.view), CC_UTF8));
        os.Add ("portableRunning", routing.portableRunning);
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
        os.Add ("presentInjections", (GS::Int32) health.presentInjections);
        os.Add ("hostNoDepthTarget", (GS::Int32) health.hostNoDepthTarget);
        os.Add ("hostNoGeometry", (GS::Int32) health.hostNoGeometry);
        os.Add ("overlayNoEdges", (GS::Int32) health.overlayNoEdges);
        os.Add ("overlayNoCamera", (GS::Int32) health.overlayNoCamera);
        os.Add ("modelFramesSeen", (GS::Int32) health.modelFramesSeen);
        os.Add ("eligibleCandidates", (GS::Int32) health.eligibleCandidates);
        os.Add ("selectionValid", health.selectionValid);
        os.Add ("selectedGroup", (GS::Int32) health.selectedGroup);
        os.Add ("selectedOccurrence", (GS::Int32) health.selectedOccurrence);
        os.Add ("occurrenceLocked", health.occurrenceLocked);
        os.Add ("cameraSource", GS::UniString (health.cameraSource.c_str (), CC_UTF8));
        os.Add ("armState", GS::UniString (health.armState.c_str (), CC_UTF8));
        os.Add ("logicalMatches", (GS::Int32) health.logicalMatches);
        os.Add ("authoritativeSnapshots", (GS::Int32) health.authoritativeSnapshots);

        // ⚠️ THE NUMBERS A REGRESSION RUN ASSERTS ON, AND
        // NOTHING ELSE CAN SEE THEM. Every fault in this series was diagnosed
        // from the log after the fact; a command that can read these decides
        // pass or fail while the session is still open. The three revisions name
        // which stage a stale overlay stopped at, the watch counters separate
        // "not looking" from "looking and seeing nothing", the age buckets say
        // whether the camera is current, and `modelEditRebinds` is the one that
        // must NOT move during navigation.
        os.Add ("modelRevision", (GS::Int32) health.modelRevision);
        os.Add ("silhouetteEdges", (GS::Int32) health.silhouetteEdges);
        os.Add ("overlayBackend", GS::UniString (health.overlayBackend.c_str (), CC_UTF8));
        os.Add ("diligentAttached", health.diligentAttached);
        os.Add ("diligentAttachMs", (GS::Int32) health.diligentAttachMs);
        os.Add ("diligentAttachFailures", (GS::Int32) health.diligentAttachFailures);
        os.Add ("diligentWraps", (GS::Int32) health.diligentWraps);
        os.Add ("diligentWrapHits", (GS::Int32) health.diligentWrapHits);
        os.Add ("diligentWrapFailures", (GS::Int32) health.diligentWrapFailures);
        os.Add ("diligentDistinctBackBuffers", (GS::Int32) health.diligentDistinctBackBuffers);
        os.Add ("diligentWrapDropsOnResize", (GS::Int32) health.diligentWrapDropsOnResize);
        os.Add ("diligentError", GS::UniString (health.diligentError.c_str (), CC_UTF8));
        os.Add ("publishedRevision", (GS::Int32) health.publishedRevision);
        os.Add ("gpuRevision", (GS::Int32) health.gpuRevision);
        os.Add ("watchRunning", health.watchRunning);
        os.Add ("watchPolls", (GS::Int32) health.watchPolls);
        os.Add ("watchEdits", (GS::Int32) health.watchEdits);
        os.Add ("watchRefreshes", (GS::Int32) health.watchRefreshes);
        os.Add ("watchEnvironmentOnly", (GS::Int32) health.watchEnvironmentOnly);
        os.Add ("watchSkippedBusy", (GS::Int32) health.watchSkippedBusy);
        os.Add ("watchError", GS::UniString (health.watchError.c_str (), CC_UTF8));
        os.Add ("modelEditRebinds", (GS::Int32) health.modelEditRebinds);
        os.Add ("modelEditReselects", (GS::Int32) health.modelEditReselects);
        os.Add ("lastMissMask", (GS::Int32) health.lastMissMask);
        os.Add ("rebinds", (GS::Int32) health.rebinds);
        os.Add ("rebindsRefused", (GS::Int32) health.rebindsRefused);
        os.Add ("selectionMatches", (GS::Int32) health.selectionMatches);
        os.Add ("pinMissMask", (GS::Int32) health.pinMissMask);
        os.Add ("resizeRebinds", (GS::Int32) health.resizeRebinds);
        os.Add ("age0", (GS::Int32) health.age0);
        os.Add ("age1", (GS::Int32) health.age1);
        os.Add ("age2", (GS::Int32) health.age2);
        os.Add ("age3plus", (GS::Int32) health.age3plus);
        os.Add ("cameraAgeMax", (GS::Int32) health.cameraAgeMax);
        os.Add ("suppressedStaleViewport", (GS::Int32) health.suppressedStaleViewport);
        os.Add ("redrawRequests", (GS::Int32) health.redrawRequests);
        os.Add ("acceptedViewportWidth", (GS::Int32) health.acceptedViewportWidth);
        os.Add ("acceptedViewportHeight", (GS::Int32) health.acceptedViewportHeight);
        os.Add ("targetWidth", (GS::Int32) health.targetWidth);
        os.Add ("targetHeight", (GS::Int32) health.targetHeight);
        os.Add ("blockedAt", GS::UniString (health.blockedAt.c_str (), CC_UTF8));
        os.Add ("lastError", GS::UniString (runtime::StartErrorName (health.lastError), CC_UTF8));
        os.Add ("lastMessage", GS::UniString (health.lastMessage.c_str (), CC_UTF8));
        return os;
    }
};

const NativeCommandRegistration kHostGeometryCommandRegistrations[] = {
    { "OverlayRuntime",
      &MakeRegisteredNativeCommand<OverlayRuntimeCommand>, false, R"json({"type":"object","properties":{"action":{"type":"string","enum":["start","stop","hide","show","state"]},"backend":{"type":"string","enum":["native","diligent"]}},"additionalProperties":false})json", R"json({"type":"object","properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"message":{"type":"string"},"retryable":{"type":"boolean"},"running":{"type":"boolean"},"visible":{"type":"boolean"},"waitingForContext":{"type":"boolean"},"camera":{"type":"string"},"host":{"type":"string"},"autoSelections":{"type":"integer"},"reacquisitions":{"type":"integer"},"hostOpaqueTriangles":{"type":"integer"},"overlayDraws":{"type":"integer"},"presentInjections":{"type":"integer"},"hostNoDepthTarget":{"type":"integer"},"hostNoGeometry":{"type":"integer"},"overlayNoEdges":{"type":"integer"},"overlayNoCamera":{"type":"integer"},"modelFramesSeen":{"type":"integer"},"eligibleCandidates":{"type":"integer"},"selectionValid":{"type":"boolean"},"selectedGroup":{"type":"integer"},"selectedOccurrence":{"type":"integer"},"occurrenceLocked":{"type":"boolean"},"cameraSource":{"type":"string"},"armState":{"type":"string"},"logicalMatches":{"type":"integer"},"authoritativeSnapshots":{"type":"integer"},"blockedAt":{"type":"string"},"lastError":{"type":"string"},"lastMessage":{"type":"string"},"view":{"type":"string"},"portableRunning":{"type":"boolean"},"modelRevision":{"type":"integer"},"silhouetteEdges":{"type":"integer"},"overlayBackend":{"type":"string"},"diligentAttached":{"type":"boolean"},"diligentAttachMs":{"type":"integer"},"diligentAttachFailures":{"type":"integer"},"diligentWraps":{"type":"integer"},"diligentWrapHits":{"type":"integer"},"diligentWrapFailures":{"type":"integer"},"diligentDistinctBackBuffers":{"type":"integer"},"diligentWrapDropsOnResize":{"type":"integer"},"diligentError":{"type":"string"},"publishedRevision":{"type":"integer"},"gpuRevision":{"type":"integer"},"watchRunning":{"type":"boolean"},"watchPolls":{"type":"integer"},"watchEdits":{"type":"integer"},"watchRefreshes":{"type":"integer"},"watchEnvironmentOnly":{"type":"integer"},"watchSkippedBusy":{"type":"integer"},"watchError":{"type":"string"},"modelEditRebinds":{"type":"integer"},"modelEditReselects":{"type":"integer"},"lastMissMask":{"type":"integer"},"rebinds":{"type":"integer"},"rebindsRefused":{"type":"integer"},"selectionMatches":{"type":"integer"},"pinMissMask":{"type":"integer"},"resizeRebinds":{"type":"integer"},"age0":{"type":"integer"},"age1":{"type":"integer"},"age2":{"type":"integer"},"age3plus":{"type":"integer"},"cameraAgeMax":{"type":"integer"},"suppressedStaleViewport":{"type":"integer"},"redrawRequests":{"type":"integer"},"acceptedViewportWidth":{"type":"integer"},"acceptedViewportHeight":{"type":"integer"},"targetWidth":{"type":"integer"},"targetHeight":{"type":"integer"}},"additionalProperties":false,"required":["ok","running","camera","host"]})json" },
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
