// ArchViz/DiligentViewportControl — the MAIN THREAD's side of the viewport.
//
// Everything a caller outside the render thread is allowed to touch lives here:
// the singleton, the lifecycle (Start/Stop/RequestResize), and the publishers
// that hand the frame loop a new camera, sun or anchor set. `DiligentViewport.cpp`
// keeps exactly one thing, which is what DiligentViewportSupport.hpp already said
// it should keep: own the device, swap chain and scene, run the frame loop, and
// tear it down in the order the flip model demands.
//
// It is a separate translation unit because the two halves answer to opposite
// rules and the file had grown past the cap with both in it. Splitting on the
// THREAD BOUNDARY rather than on size means the rule is checkable by reading a
// function's name: if it publishes to an atomic or takes `mutex_`, it belongs
// here; if it touches a Diligent object, it does not.
//
// ⚠️ MAIN THREAD (or any caller thread), NEVER the render thread. Every function
// here communicates through atomics and `mutex_` precisely because the render
// thread is running while they are called. None of them may touch a Diligent
// object, and none of them may call ACAPI either -- a `CameraStart` arrives
// already filled by whoever could legally read it.

#include "ArchViz/DiligentViewport.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/PlanAnchorRibbon.hpp" // BuildAnchorRibbonSet
#include "ArchViz/SceneCmdQueue.hpp"

#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv::archviz {

DiligentViewport& DiligentViewport::Get ()
{
    static DiligentViewport viewport;
    return viewport;
}

DiligentViewport::~DiligentViewport ()
{
    Stop ();
}

bool DiligentViewport::Start (const Surface& surface, const CameraStart& camera)
{
    std::lock_guard<std::mutex> lock (lifecycleMutex_);
    return StartUnlocked (surface, camera);
}

bool DiligentViewport::StartUnlocked (const Surface& surface, const CameraStart& camera)
{
    if (running_.load () || !surface.IsValid ())
        return false;

    if (worker_.joinable ())
        worker_.join ();

    stopRequested_.store (false);
    resizePending_.store (false);
    pendingWidth_.store (surface.width);
    pendingHeight_.store (surface.height);
    // Before the worker starts, so a SyncCamera arriving on the first tick of a
    // sync timer already sees this run's mode rather than the previous run's.
    mode_.store (surface.mode);
    {
        std::lock_guard<std::mutex> lock (mutex_);
        stats_ = {};
        stats_.running = true;
        stats_.width = surface.width;
        stats_.height = surface.height;
        stats_.cameraSource = camera.source;
        currentCameraAvailable_ = false;
    }
    running_.store (true);
    ArchVizLog ("---- Diligent viewport starting: " + std::to_string (surface.width) + "x" +
                std::to_string (surface.height) + " ----");
    // ⚠️ NO BUTTON MAY BE HELD FROM A PREVIOUS RUN. The wheel button is polled
    // globally, so a viewport opened while the user happens to be mid-drag in
    // Archicad would latch that drag on its first frame.
    InputRingBuffer::Get ().Reset ();
    worker_ = std::thread (&DiligentViewport::Run, this, surface, camera);
    return true;
}

bool DiligentViewport::StartCapture (uint32_t width, uint32_t height, const CameraStart& camera, int renderQuality,
                                     const CaptureOverlays& overlays, uint64_t& captureId, std::string& error)
{
    std::lock_guard<std::mutex> lifecycleLock (lifecycleMutex_);
    captureId = 0;
    if (!camera.valid || camera.orthographic) {
        error = "headless capture currently requires a valid perspective camera";
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite (camera.eye[axis]) || !std::isfinite (camera.target[axis]) ||
            std::abs (camera.eye[axis]) > 1e15f || std::abs (camera.target[axis]) > 1e15f) {
            error = "headless capture camera coordinates must be finite and within +/-1e15 metres";
            return false;
        }
    }
    const double dx = double (camera.eye[0]) - double (camera.target[0]);
    const double dy = double (camera.eye[1]) - double (camera.target[1]);
    const double dz = double (camera.eye[2]) - double (camera.target[2]);
    if (dx * dx + dy * dy + dz * dz <= 1e-8f || camera.viewConeDegreesHorizontal <= 1.0f ||
        camera.viewConeDegreesHorizontal >= 179.0f) {
        error = "headless capture requires distinct eye/target points and a horizontal field of view in (1, 179)";
        return false;
    }
    if (IsRunning () || ExtractionWorker::Get ().IsRunning ()) {
        error = "a Diligent viewport or extraction pass is already running";
        return false;
    }
    if (worker_.joinable ())
        worker_.join ();

    SceneCmdQueue::Get ().Clear ();
    {
        std::lock_guard<std::mutex> lock (mutex_);
        captureId = ++nextCaptureId_;
        captureStats_ = {};
        captureStats_.id = captureId;
        captureStats_.status = "running";
        captureStats_.stage = "extracting";
        captureStats_.width = width;
        captureStats_.height = height;
        captureStats_.url = "http://127.0.0.1:19191/screenshot/diligent?id=" + std::to_string (captureId);
    }
    activeCaptureId_.store (captureId);
    captureRenderQuality_.store (renderQuality);
    captureStorySlices_.store (overlays.storySlices);
    captureStorySliceFill_.store (overlays.storySliceFill);
    captureStorySliceOccluded_.store (int (overlays.storySliceOccluded));
    captureStorySliceWidthPixels_.store (overlays.storySliceWidthPixels);
    captureStorySliceRgba_.store (overlays.storySliceRgba);
    captureStorySliceFillRgba_.store (overlays.storySliceFillRgba);

    // ⚠️ BEFORE Start, NOT AFTER. The worker reads this once at the top of a
    // pass (see ExtractionThread.hpp), so setting it afterwards would arm the
    // NEXT extraction and leave this capture -- the one that asked for the
    // contours -- without them. A capture is one pass; there is no second
    // chance to catch.
    ExtractionWorker::Get ().SetStorySlicesWanted (overlays.storySlices);
    ExtractionWorker::Get ().Start (true);
    Surface surface;
    surface.mode = SurfaceMode::Offscreen;
    surface.width = width;
    surface.height = height;
    if (!StartUnlocked (surface, camera)) {
        ExtractionWorker::Get ().Stop ();
        SceneCmdQueue::Get ().Clear ();
        activeCaptureId_.store (0);
        std::lock_guard<std::mutex> lock (mutex_);
        captureStats_.status = "failed";
        captureStats_.stage = "starting";
        captureStats_.failureMessage = "the offscreen D3D11 viewport refused to start";
        error = captureStats_.failureMessage;
        return false;
    }
    return true;
}

bool DiligentViewport::CancelCapture (uint64_t captureId)
{
    uint64_t expected = captureId;
    if (captureId == 0 || !activeCaptureId_.compare_exchange_strong (expected, (std::numeric_limits<uint64_t>::max) ()))
        return false;
    ExtractionWorker::Get ().RequestStop ();
    stopRequested_.store (true);
    std::lock_guard<std::mutex> lock (mutex_);
    captureStats_.stage = "cancelling";
    return true;
}

DiligentCaptureStats DiligentViewport::CaptureStats () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return captureStats_;
}

bool DiligentViewport::CurrentCamera (CameraStart& camera) const
{
    std::lock_guard<std::mutex> lock (mutex_);
    if (!currentCameraAvailable_)
        return false;
    camera = currentCamera_;
    return true;
}

void DiligentViewport::RequestStop ()
{
    stopRequested_.store (true);
}

void DiligentViewport::Stop ()
{
    std::lock_guard<std::mutex> lifecycleLock (lifecycleMutex_);
    stopRequested_.store (true);
    if (worker_.joinable ())
        worker_.join ();
    running_.store (false);
    std::lock_guard<std::mutex> lock (mutex_);
    stats_.running = false;
}

void DiligentViewport::RequestResize (uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;
    // Keep the latest requested dimensions after the render thread consumes the
    // flag, so repeated DG layouts do not enqueue the same swap-chain resize.
    if (pendingWidth_.load () == width && pendingHeight_.load () == height)
        return;
    pendingWidth_.store (width);
    pendingHeight_.store (height);
    resizePending_.store (true);
}

void DiligentViewport::AdoptSurfaceSize (const DiligentViewportTarget& target, uint32_t& width,
                                         uint32_t& height)
{
    const uint32_t actualWidth = target.Width ();
    const uint32_t actualHeight = target.Height ();
    // A zero is a surface that has not been built yet, not a resize to nothing.
    if (actualWidth == 0 || actualHeight == 0)
        return;
    if (actualWidth == width && actualHeight == height)
        return;
    width = actualWidth;
    height = actualHeight;
    std::lock_guard<std::mutex> lock (mutex_);
    stats_.width = width;
    stats_.height = height;
}

void DiligentViewport::SyncCamera (const CameraStart& camera)
{
    // ⚠️ THE PANEL DOES NOT FOLLOW ARCHICAD (PLAT-RE124). The panel and the
    // overlay share this singleton, so an overlay-shaped camera poll would
    // otherwise drive the panel's camera as a side effect of being armed at all.
    // The panel's contract is one sync at open (Start's CameraStart) and
    // standalone navigation after it; an explicit re-sync goes through
    // AdoptCamera, which is a request rather than a follow. See the header.
    if (mode_.load () != SurfaceMode::Overlay)
        return;
    PublishCamera (camera, false);
}

void DiligentViewport::AdoptCamera (const CameraStart& camera)
{
    PublishCamera (camera, true);
}

void DiligentViewport::PublishCamera (const CameraStart& camera, bool discontinuity)
{
    // ⚠️ AN INVALID CAMERA IS DROPPED, NOT PUSHED. Archicad's 3D window in an
    // axonometric projection has no eye position at all, and
    // ACAPI_View_Get3DProjectionSets can simply fail; either arrives here as
    // `valid == false`. Adopting it would point the overlay's camera at the
    // origin from the origin -- an empty grey frame, which reads as the viewport
    // having died rather than as the sync having nothing to say.
    if (!camera.valid)
        return;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock (mutex_);
        pendingCamera_ = camera;
        pendingCameraDiscontinuity_ = discontinuity;
        // ⚠️ STAMPED INSIDE THE LOCK, WITH THE CAMERA IT BELONGS TO. The render
        // thread takes the pair out together; if the number were assigned
        // outside, two syncs in quick succession could pair the first camera
        // with the second's generation and a stale frame would look current.
        generation = ++pendingCameraGeneration_;
    }
    // Published AFTER the payload, so the render thread cannot see the flag set
    // and read the previous camera behind it.
    publishedCameraGeneration_.store (generation, std::memory_order_release);
    cameraSyncPending_.store (true);
}

void DiligentViewport::SetSunOverride (bool on, float azimuthDegrees, float altitudeDegrees)
{
    sunOverrideAzimuth_.store (azimuthDegrees);
    sunOverrideAltitude_.store (altitudeDegrees);
    sunOverrideOn_.store (on);
    // ⚠️ THE GENERATION LAST, AFTER THE PAYLOAD. The render thread adopts on
    // seeing a new number here; bumping it first would let a frame land between
    // the two stores and adopt the PREVIOUS angles under the new generation --
    // which then never gets adopted again, because the counter has already moved.
    sunOverrideSeq_.fetch_add (1);
}

void DiligentViewport::SetEnvironmentMap (const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        pendingEnvironmentPath_ = path;
    }
    environmentLoadPending_.store (true);
}

void DiligentViewport::SetEnvironmentSettings (bool enabled, float intensity, float rotationDegrees)
{
    environmentEnabled_.store (enabled);
    environmentIntensity_.store (intensity);
    environmentRotationDegrees_.store (rotationDegrees);
    environmentSettingsSeq_.fetch_add (1);
}

void DiligentViewport::SetPlanAnchors (const std::vector<std::vector<float>>& outlines,
                                       const std::vector<std::vector<float>>& arcs, bool enabled, float widthPixels,
                                       uint32_t rgba, float arcSign, float planZ)
{
    // Built HERE rather than in the frame loop -- see the header. It is pure
    // arithmetic (and tested as such, in tests/cpp), and the render thread
    // should not spend a present on it.
    std::vector<PlanAnchorVertex> ribbon;
    BuildAnchorRibbonSet (outlines, arcs, planZ, arcSign, /*arcChordMetres*/ 0.05f, ribbon);

    {
        std::lock_guard<std::mutex> lock (mutex_);
        pendingPlanAnchors_ = std::move (ribbon);
    }
    planAnchorWidthPixels_.store (widthPixels);
    planAnchorRgba_.store (rgba);
    planAnchorsOn_.store (enabled);
    // ⚠️ THE GENERATION LAST, AFTER THE PAYLOAD -- the same rule, and the same
    // reason, as SetSunOverride above.
    planAnchorSeq_.fetch_add (1);
}

DiligentViewportStats DiligentViewport::Stats () const
{
    std::lock_guard<std::mutex> lock (mutex_);
    return stats_;
}

} // namespace geomsrv::archviz
