// NativeCommands/HostGeometryCommands -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/HostGeometryCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandBase.hpp"

#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/ExtractionThread.hpp"

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

const NativeCommandRegistration kHostGeometryCommandRegistrations[] = {
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
