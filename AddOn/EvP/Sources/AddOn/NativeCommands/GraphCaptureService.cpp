#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/GraphCaptureService.hpp"

#include "ArchViz/DiligentViewport.hpp"
#include "NodeGraph/RenderNodes.hpp"
#include "Python/MainThreadGate.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace geomsrv {
namespace {

namespace av = archviz;
namespace graph = evp::nodegraph;

// Where a graph's captures land.
//
// ⚠️ THE RENDERER DOES NOT CREATE DIRECTORIES ON A USER'S DISK - it is handed
// one that exists. Deciding where output goes is a product decision and belongs
// on this side of the boundary, beside the other places Tapioca writes.
std::string MakeCaptureDirectory (std::string& error)
{
    const char* local = std::getenv ("LOCALAPPDATA");
    if (local == nullptr || *local == '\0') {
        error = "%LOCALAPPDATA% is not set, so there is nowhere to write the captured frames";
        return {};
    }
    // One folder per run, stamped, so two runs of the same graph do not
    // overwrite each other's frames - the paths a previous run handed
    // downstream must keep resolving to the images that run produced.
    const auto now = std::chrono::system_clock::now ();
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch ()).count ();
    const std::filesystem::path folder =
        std::filesystem::path (local) / "Tapioca" / "output" / "graph-captures" / std::to_string (stamp);
    std::error_code failure;
    std::filesystem::create_directories (folder, failure);
    if (failure) {
        error = "could not create " + folder.string () + ": " + failure.message ();
        return {};
    }
    return folder.string ();
}

// MAIN THREAD. Applies a saved 3D view, so the extraction sees that view's
// visibility rather than whatever the 3D window happens to show.
bool GoToModelView (const std::string& guid, std::string& error)
{
    bool ok = false;
    GS::UniString gateError;
    const bool delivered = evp::MainThreadGate::Get ().Invoke (
        [&guid, &ok, &error] () {
            // ⚠️ AND IT LEAVES THE VIEW APPLIED, by explicit decision. That is
            // why the capture node is HostUiWrite: this moves what the user is
            // looking at, so it may only happen on a deliberate press.
            const GSErrCode err = ACAPI_View_GoToView (guid.c_str ());
            if (err != NoError) {
                error = "Archicad refused the model view (error " + std::to_string ((int) err) + ")";
                return;
            }
            ok = true;
        },
        evp::MainThreadGate::DefaultTimeoutMs, gateError);
    if (!delivered) {
        error = gateError.IsEmpty () ? std::string ("Archicad did not respond")
                                     : std::string (gateError.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        return false;
    }
    return ok;
}

class DiligentCaptureService final : public graph::ICaptureService {
  public:
    bool Capture (const graph::CaptureBatchRequest& request, const std::function<bool ()>& cancelled,
                  std::vector<std::string>& paths, std::string& error) override
    {
        if (!request.modelViewGuid.empty () && !GoToModelView (request.modelViewGuid, error))
            return false;

        graph::RenderSettings settings;
        // An unwired settings node is the defaults, not a failure: the capture
        // node is usable the moment it has cameras.
        if (!request.settings.empty () && !graph::DecodeRenderSettings (request.settings, settings)) {
            error = "the settings input is not a render-settings value";
            return false;
        }

        std::vector<av::CaptureFrame> frames;
        frames.reserve (request.cameras.size ());
        for (const graph::ViewCamera& camera : request.cameras) {
            av::CaptureFrame frame;
            // ⚠️ THE NARROWING HAPPENS HERE, ONCE, AT THE RENDERER'S DOOR.
            // Everything above is doubles because ACAPI and the graph are;
            // CameraStart is float because it is read on the render thread.
            frame.camera.valid = camera.valid;
            frame.camera.source = camera.source;
            for (int axis = 0; axis < 3; ++axis) {
                frame.camera.eye[axis] = static_cast<float> (camera.eye[axis]);
                frame.camera.target[axis] = static_cast<float> (camera.target[axis]);
            }
            frame.camera.viewConeDegreesHorizontal = static_cast<float> (camera.viewConeDegreesHorizontal);
            frame.sunEnabled = camera.hasSun;
            frame.sunAzimuthDegrees = static_cast<float> (camera.sunAzimuthDegrees);
            frame.sunAltitudeDegrees = static_cast<float> (camera.sunAltitudeDegrees);
            frames.push_back (frame);
        }

        const std::string directory = MakeCaptureDirectory (error);
        if (directory.empty ())
            return false;

        av::DiligentViewport::CaptureOverlays overlays;
        overlays.storySlices = settings.storySlices;
        overlays.storySliceFill = settings.storySliceFill;
        overlays.storySliceOccluded = settings.storySliceOccluded == "hidden"  ? av::SliceOccludedStyle::Hidden
                                      : settings.storySliceOccluded == "solid" ? av::SliceOccludedStyle::Solid
                                                                               : av::SliceOccludedStyle::Dashed;
        overlays.storySliceWidthPixels = static_cast<float> (settings.storySliceWidthPixels);
        overlays.storySliceRgba = settings.storySliceRgba;
        overlays.storySliceFillRgba = settings.storySliceFillRgba;

        graph::ReportCaptureProgress ("preparing");
        uint64_t id = 0;
        if (!av::DiligentViewport::Get ().StartCaptureBatch (
                static_cast<uint32_t> (settings.width), static_cast<uint32_t> (settings.height),
                static_cast<float> (settings.dpi), frames, settings.renderQuality == "realistic" ? 1 : 0, overlays,
                directory, id, error))
            return false;

        // ⚠️ POLLED, NOT WAITED ON, AND THE CANCELLATION IS CHECKED EVERY PASS.
        // This runs on a graph worker thread for as long as the batch takes -
        // minutes - so a Stop that was only noticed at the end would not be a
        // Stop. Cancelling asks the renderer to give up and returns the frames
        // already written rather than pretending none happened.
        for (;;) {
            if (cancelled ()) {
                av::DiligentViewport::Get ().CancelCapture (id);
                error = "the capture was cancelled";
                return false;
            }
            const av::DiligentCaptureStats stats = av::DiligentViewport::Get ().CaptureStats ();
            if (stats.id != id) {
                error = "the capture was replaced by another one";
                return false;
            }
            // ⚠️ THE STAGE THE RENDERER IS ACTUALLY IN, not one this
            // layer invents. "rendering 3 of 8" is the difference between a user
            // waiting and a user giving up, and the renderer is the only thing
            // that knows which frame it is on.
            graph::ReportCaptureProgress (stats.stage.empty () ? std::string ("capturing") : stats.stage);
            if (stats.status == "completed") {
                graph::ReportCaptureProgress (std::string {});
                paths = stats.paths;
                return true;
            }
            if (stats.status == "failed" || stats.status == "cancelled") {
                graph::ReportCaptureProgress (std::string {});
                error = "the capture " + stats.status + " during " + stats.stage +
                        (stats.failureMessage.empty () ? std::string {} : ": " + stats.failureMessage);
                return false;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
    }
};

} // namespace

evp::nodegraph::ICaptureService& GraphCaptureService ()
{
    static DiligentCaptureService service;
    return service;
}

} // namespace geomsrv
