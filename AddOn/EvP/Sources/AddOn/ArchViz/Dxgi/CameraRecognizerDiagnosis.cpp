// ⚠️ BOUND BY OVERLAY-INVARIANTS.md §7 -- a refusal names its reason. A fingerprint
// that matches nothing must say which term refused, or it costs a whole run to find out.
// ArchViz/Dxgi/CameraRecognizerDiagnosis -- `NoteSoleMiss` and the report's names, see
// CameraRecognizer.hpp. What a refused draw looked like, and what the states are
// called, is the report, not the decision, and lives apart from the matching it
// reports on.

#include "ArchViz/Dxgi/CameraRecognizer.hpp"

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace census {

void NoteSoleMiss (FingerprintDiagnosis& diagnosis, uint32_t term, const contextstate::ContextState& live,
                   DrawKind kind, uint32_t indexCount, uint32_t occurrence)
{
    if (term >= kFingerprintTermCount)
        return;
    ++diagnosis.soleMiss[term];
    if (diagnosis.sampled[term])
        return;
    diagnosis.sampled[term] = true;
    uint32_t* out = diagnosis.observed[term];
    switch (term) {
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

// The report's names for the recognizer's states, reasons and gate terms.

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
        case kGateProjectionDivides:
            return "projectionDivides";
        default:
            return "?";
    }
}

} // namespace census
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
