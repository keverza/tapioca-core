// What a headless capture does with a finished frame: encode it, put it
// somewhere, and move to the next camera.
//
// ⚠️ ITS OWN TRANSLATION UNIT, AND NEITHER OF THE OBVIOUS HOMES WOULD
// TAKE IT. DiligentViewport.cpp is a frozen-size exception whose recorded rule
// is that frame-body work is EXTRACTED rather than added - what is irreducible
// there is the ORDER of the passes, not the work inside them - and
// DiligentViewportSupport.cpp is already at the soft cap, so putting it there
// would move the problem rather than solve it. This is a distinct concern with
// a name: a capture's frame accounting, on the render thread.
//
// RENDER THREAD, like everything it is called from. It never touches ACAPI or
// DG, which is what lets it write files directly.

#include "ArchViz/DiligentViewport.hpp"

#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentViewportSupport.hpp"
#include "ArchViz/DiligentViewportTarget.hpp"
#include "Screenshot/ScreenshotStore.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace geomsrv::archviz {

namespace {

// ⚠️ THE FRAME IS WRITTEN AS IT IS ENCODED, NOT COLLECTED AND HANDED BACK
// AT THE END. Eight 4K PNGs is tens of megabytes held for no reason, and the
// reason a batch exists at all is that its frame count is not one. Plain
// std::ofstream: the render thread may not call ACAPI, and this needs nothing
// from it.
bool WriteCaptureFile (const std::string& path, const std::string& bytes, std::string& error)
{
    std::ofstream file (path, std::ios::binary);
    if (!file) {
        error = "could not open " + path + " for writing";
        return false;
    }
    file.write (bytes.data (), static_cast<std::streamsize> (bytes.size ()));
    if (!file) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

// "00.png", "01.png", ... Zero-padded so a directory listing and a sort agree
// with capture order past ten frames, which is where an unpadded name stops
// being readable.
std::string CaptureFileName (size_t index)
{
    const std::string digits = std::to_string (index);
    return (digits.size () < 2 ? "0" + digits : digits) + ".png";
}

} // namespace

// ---------------------------------------------------------------------------
// ONE CAPTURED FRAME: encode it, put it somewhere, and either move to the next
// camera or finish.
//
// ⚠️ A MEMBER DEFINED HERE RATHER THAN IN DiligentViewport.cpp, exactly as
// ApplyCaptureSettings is and for the recorded reason: that file is a
// frozen-size exception, and frame-body work must be extracted rather than
// added to it. What has to stay there is the ORDER of the passes; what a
// capture does with the finished pixels does not.
//
// RENDER THREAD.
DiligentViewport::CaptureStep DiligentViewport::CaptureOneFrame (DiligentViewportTarget& target,
                                                                 Diligent::IDeviceContext* context, Camera& camera,
                                                                 DiligentScene& scene, uint32_t width, uint32_t height,
                                                                 uint64_t runCaptureId, size_t& captureIndex,
                                                                 std::chrono::steady_clock::time_point& captureReadyAt)
{
    {
        std::lock_guard<std::mutex> lock (mutex_);
        captureStats_.stage =
            "encoding " + std::to_string (captureIndex + 1) + " of " + std::to_string (captureFrames_.size ());
    }

    std::string png;
    std::string captureError;
    if (!target.CapturePng (context, png, captureError))
        throw std::runtime_error (captureError);

    const bool lastFrame = captureIndex + 1 >= captureFrames_.size ();
    if (captureOutputDirectory_.empty ()) {
        // The single capture's contract, unchanged: one frame, into the
        // screenshot store, fetched over loopback. Claiming the id here is what
        // makes a cancel that lands mid-encode lose the race rather than publish
        // over a newer run.
        uint64_t captureId = runCaptureId;
        if (captureId == 0 || captureId == (std::numeric_limits<uint64_t>::max) () ||
            !activeCaptureId_.compare_exchange_strong (captureId, 0))
            return CaptureStep::Finished;
        ScreenshotStore::Get ().Publish ("diligent", png, captureId);
    }
    else {
        // ⚠️ THE ID IS CHECKED BUT NOT CLAIMED UNTIL THE LAST FRAME. A batch
        // stays cancellable between frames, and clearing activeCaptureId_ on
        // frame one would make every later frame look like a cancelled run to
        // itself.
        if (activeCaptureId_.load () != runCaptureId)
            return CaptureStep::Finished;
        const std::string path = captureOutputDirectory_ + "\\" + CaptureFileName (captureIndex);
        std::string writeError;
        if (!WriteCaptureFile (path, png, writeError))
            throw std::runtime_error (writeError);
        std::lock_guard<std::mutex> lock (mutex_);
        captureStats_.paths.push_back (path);
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);
        captureStats_.bytes += png.size ();
        captureStats_.framesDone = captureIndex + 1;
        if (lastFrame)
            captureStats_.stage = "teardown";
    }

    if (lastFrame) {
        if (!captureOutputDirectory_.empty ()) {
            uint64_t captureId = runCaptureId;
            activeCaptureId_.compare_exchange_strong (captureId, 0);
        }
        return CaptureStep::Finished;
    }

    // ---- on to the next camera ---------------------------------------------
    ++captureIndex;
    const CaptureFrame& next = captureFrames_[captureIndex];
    ApplyArchicadCamera (camera, next.camera, width, height);
    // The sun goes through the ordinary override path rather than a second one:
    // the frame loop already reads these atomics every frame and hands them to
    // the scene, so setting the scene directly here would be overwritten on the
    // next iteration.
    SetSunOverride (next.sunEnabled, next.sunAzimuthDegrees, next.sunAltitudeDegrees);
    // ⚠️ THE ACCUMULATED FRAMES ARE THE PREVIOUS CAMERA'S. Not resetting
    // here is what would smear viewpoint N-1 across viewpoint N, and the settle
    // in the frame loop is what gives the reset time to take effect.
    scene.ResetTemporalAntiAliasingHistory ();
    captureReadyAt = std::chrono::steady_clock::time_point {};
    return CaptureStep::NextCamera;
}

} // namespace geomsrv::archviz
