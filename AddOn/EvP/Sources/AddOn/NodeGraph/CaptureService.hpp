#ifndef EVP_NODEGRAPH_CAPTURESERVICE_HPP
#define EVP_NODEGRAPH_CAPTURESERVICE_HPP

// The seam between the graph runtime and the RENDERER.
//
// ⚠️ IT EXISTS FOR THE SAME REASON IArchicadHost DOES, and the architecture gate
// is what makes that concrete rather than tasteful: NodeGraph is services-tier
// and ArchViz is features-tier, includes point down only, so the capture node
// cannot name DiligentViewport any more than it can name ACAPI. The node
// declares WHAT it wants rendered; exactly one translation unit on the other
// side of the boundary knows how.
//
// The shape is IArchicadHost's, deliberately: an interface here, one
// implementation installed at add-on startup, and nullptr treated as ordinary
// rather than exceptional - a graph evaluated with no renderer installed (the
// offline suite, a headless test) must report that plainly, not crash.

#include "NodeGraph/ArchicadHost.hpp"

#include <functional>
#include <string>
#include <vector>

namespace evp::nodegraph {

// One capture, of many cameras, from one model extraction.
//
// ⚠️ THE SETTINGS TRAVEL AS THE STRING THE RENDER-SETTINGS NODE PRODUCED. This
// interface does not re-describe width, height, quality and the contour options:
// RenderNodes.cpp already encodes them in StartDiligentCapture's own field
// names, and restating them here would be a second definition of the same
// contract - the exact fault ArchVizCaptureParams.cpp's boundary exception is
// written to avoid on the other side.
struct CaptureBatchRequest {
    // In capture order. Each carries the sun it was taken under; a camera with
    // `hasSun` false leaves the project's own lighting alone.
    std::vector<ViewCamera> cameras;

    // The render-settings JSON, or empty for the defaults.
    std::string settings;

    // A 3D view from the View Map to apply before extracting, or empty to use
    // whatever the 3D window is currently showing.
    //
    // ⚠️ APPLYING ONE CHANGES WHAT ARCHICAD SHOWS AND LEAVES IT CHANGED. That
    // was a deliberate call, not an oversight - see the capture node, which is
    // HostUiWrite precisely because of this.
    std::string modelViewGuid;
};

class ICaptureService {
  public:
    virtual ~ICaptureService () = default;

    // Renders every camera and returns the files written, in camera order.
    //
    // ⚠️ SYNCHRONOUS TO THE CALLER, ASYNCHRONOUS UNDERNEATH. The renderer's own
    // interface is start-and-poll; a node body wants an answer. The polling
    // lives on the far side of this call, and `cancelled` is how the evaluator's
    // cancellation reaches it - a batch runs for minutes, so a Stop that only
    // took effect afterwards would not be a Stop at all.
    virtual bool Capture (const CaptureBatchRequest& request, const std::function<bool ()>& cancelled,
                          std::vector<std::string>& paths, std::string& error) = 0;
};

// The installed renderer, or nullptr when the graph is running without one.
// Callers must treat nullptr as ordinary.
ICaptureService* ActiveCaptureService ();
void SetActiveCaptureService (ICaptureService* service);

// ---- what a running capture is doing, in one line -----------------------
//
// ⚠️ A SINK RATHER THAN AN EVENT, AND DELIBERATELY THE SMALLEST ONE THAT
// ANSWERS THE QUESTION. A capture runs for minutes and its only honest progress
// is a stage and a frame count, which the renderer already computes; the run
// event stream exists but carries no progress kind and no client reads it, so
// building progress on it would mean building its consumption too. One string,
// written by whoever is capturing and read by the run-state verb, is what makes
// "rendering 3 of 8" reach the node without a new channel.
//
// Any thread. Empty means nothing is capturing.
void ReportCaptureProgress (const std::string& progress);
std::string CaptureProgress ();

} // namespace evp::nodegraph

#endif
