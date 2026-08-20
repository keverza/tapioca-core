// ArchViz/PlanViewCamera — see the header. ACAPI, MAIN THREAD ONLY.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/PlanViewCamera.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog
#include "ArchViz/PlanCameraMath.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"   // the target HWND, for its DPI

#include <windows.h>

#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

constexpr double kRadToDeg = 57.29577951308232;

// `API_Point` is a pair of SHORTS. A surface wider than this is not a window
// anyone has, but the conversion below would wrap silently rather than fail, and
// a wrapped sample produces a confident camera pointing at the wrong place.
constexpr uint32_t kMaxSamplePixel = 32000;

// The eye's height above the plan, and therefore the depth range. Only clipping
// depends on it -- see DiligentViewportSupport's orthographic branch, which owns
// the same constant for the same reason.
constexpr float kPlanTargetZ = 0.0f;

// The display scaling of the window the overlay is covering: 2.0 at 200%.
//
// ⚠️ THIS IS THE ANSWER TO PLAT-RE63, AND THE FIRST RUN IS WHAT ANSWERED IT.
// `API_Point` is documented as "pixels from the top/left corner of the window"
// and does not say WHICH pixels. It is LOGICAL, DPI-scaled ones, and the run
// that established it did so by its signature rather than by any documentation:
// on a 200% display the overlay came back "scaled down ~50% and moved to the top
// left". Both halves follow from sampling at PHYSICAL pixels:
//
//   * sampling at physical `W` reaches TWO window-widths out, so the fit's
//     metres-per-pixel is twice what it should be and the frustum covers twice
//     the model -- the building draws at half size;
//   * the point taken as the window's centre, physical (W/2, H/2), is really the
//     window's BOTTOM-RIGHT corner, so the model centres on a point that belongs
//     three-quarters of the way across -- content shifts toward the top left.
//
// It also explains the rest of that report: a pan tracked at twice the cursor's
// speed and a zoom that "scaled incorrectly and moved to a different position"
// are the same factor seen while navigating.
//
// ⚠️ THE SAMPLING IS CONVERTED, NOT THE RESULT. Everything downstream then works
// in the model metres the window spans, which is a physical fact and belongs to
// neither pixel space -- see ReadPlanViewCamera.
double WindowPixelScale (HWND window)
{
    if (window == nullptr || IsWindow (window) == FALSE)
        return 1.0;
    // Per-MONITOR, deliberately: Archicad can be dragged between a 200% laptop
    // panel and a 100% external display without the add-on being told, and a
    // process-wide DPI would then be wrong on one of them.
    const UINT dpi = GetDpiForWindow (window);
    if (dpi == 0)
        return 1.0;
    return double (dpi) / 96.0;
}

bool SampleModelCoord (uint32_t pixelX, uint32_t pixelY, double out[2])
{
    API_Point point = {};
    point.h = short (pixelX);
    point.v = short (pixelY);
    API_Coord coord = {};
    if (ACAPI_View_PointToCoord (&point, &coord) != NoError)
        return false;
    out[0] = coord.x;
    out[1] = coord.y;
    return true;
}

// The zoom box, for the log only. ⚠️ IT IS A CROSS-CHECK, NOT THE SOURCE. How
// Archicad fits this box into a window of a different aspect ratio is not
// documented, so a camera built from it would carry a guess; a camera built from
// three PointToCoord samples carries none. Printing both means a disagreement
// names itself instead of being resolved silently in one direction.
std::string DescribeZoomBox ()
{
    API_Box box = {};
    if (ACAPI_View_GetZoom (&box, nullptr) != NoError)
        return "ACAPI_View_GetZoom failed";
    return "x " + std::to_string (box.xMin) + ".." + std::to_string (box.xMax) + ", y " +
           std::to_string (box.yMin) + ".." + std::to_string (box.yMax);
}

// One log line per CHANGE of state, never one per sync tick. The camera sync
// runs at 60 Hz; logging each fit would write a megabyte a minute and bury
// everything else in archviz.log.
std::string g_lastLogged;

void LogOnce (const std::string& line)
{
    if (line == g_lastLogged)
        return;
    g_lastLogged = line;
    ArchVizLog (line);
}

}   // namespace

bool CurrentWindowIsFloorPlan ()
{
    API_WindowInfo info = {};
    if (ACAPI_Window_GetCurrentWindow (&info) != NoError)
        return false;
    return info.typeID == APIWind_FloorPlanID;
}

CameraStart ReadPlanViewCamera (uint32_t widthPx, uint32_t heightPx)
{
    CameraStart start;

    if (!CurrentWindowIsFloorPlan ()) {
        start.source = "the frontmost window is not the floor plan";
        return start;
    }
    if (widthPx < 2 || heightPx < 2 || widthPx > kMaxSamplePixel || heightPx > kMaxSamplePixel) {
        start.source = "the overlay surface is " + std::to_string (widthPx) + "x" +
                       std::to_string (heightPx) + ", which cannot be sampled as a window rect";
        LogOnce ("ArchViz plan camera: " + start.source);
        return start;
    }

    // ⚠️ THE SAME RECTANGLE, EXPRESSED IN ARCHICAD'S PIXELS. `widthPx`/`heightPx`
    // are the overlay surface's PHYSICAL size; `ACAPI_View_PointToCoord` speaks
    // LOGICAL pixels (see WindowPixelScale for how that was established and what
    // getting it wrong looks like). These are the same corners of the same
    // window, named in the other space.
    const double scale = WindowPixelScale (viewportoverlay::Stats ().target);
    const uint32_t sampleWidth = uint32_t (std::lround (double (widthPx) / scale));
    const uint32_t sampleHeight = uint32_t (std::lround (double (heightPx) / scale));
    if (sampleWidth < 2 || sampleHeight < 2) {
        start.source = "the window's display scaling (" + std::to_string (scale) +
                       ") leaves nothing to sample";
        LogOnce ("ArchViz plan camera: " + start.source);
        return start;
    }

    // Three corners of the OVERLAY'S OWN rectangle. Sampling at pixel
    // `sampleWidth` rather than `sampleWidth - 1` is deliberate: the mapping is
    // continuous and affine, and the far EDGE of the surface is what the
    // projection has to cover -- taking the last pixel's CENTRE instead would
    // scale the whole overlay by (W-1)/W, which is a fraction of a percent and
    // therefore exactly the kind of error nobody finds by looking.
    double topLeft[2] = {0.0, 0.0};
    double topRight[2] = {0.0, 0.0};
    double bottomLeft[2] = {0.0, 0.0};
    // ⚠️ THE FIRST CORNER IS READ AGAIN AT THE END, AND THAT FOURTH CALL IS THE
    // FIX FOR THE DIRECTIONAL JUMP (PLAT-RE82).
    //
    // The three samples below are three SEPARATE ACAPI calls. If Archicad scrolls
    // the view between the first and the last -- which is exactly what happens
    // during a pan -- the corners describe DIFFERENT VIEWS, and the affine fit
    // through them is a blend of the two. The blend is not noise: the error lies
    // along the pan axis and flips sign with the pan direction, which is what the
    // user reported as "jumps to top left or bottom right based on pan
    // direction". Nothing in FitPlanCamera can catch it, because a torn sample is
    // still square, still uniform and still unmirrored -- it is simply somewhere
    // else.
    //
    // Re-reading pixel (0,0) detects it directly instead of inferring it. On a
    // still view `PointToCoord` is deterministic and the two answers are bit
    // identical; any difference at all means the view moved WHILE we were asking.
    double topLeftAgain[2] = {0.0, 0.0};
    if (!SampleModelCoord (0, 0, topLeft) || !SampleModelCoord (sampleWidth, 0, topRight) ||
        !SampleModelCoord (0, sampleHeight, bottomLeft) ||
        !SampleModelCoord (0, 0, topLeftAgain)) {
        start.source = "ACAPI_View_PointToCoord failed on the floor plan window";
        LogOnce ("ArchViz plan camera: " + start.source);
        return start;
    }

    // ⚠️ FITTED IN THE SAMPLE SPACE, AND THE TWO ANSWERS IT FEEDS THE CAMERA ARE
    // SPACE-INDEPENDENT. `halfHeightMetres` is half the model extent the window
    // spans and `centreX/Y` is the model point under its middle -- both are
    // physical facts about the window, identical whichever pixel space measured
    // them. Only `metresPerPixel*`, which nothing but the log reads, is per
    // LOGICAL pixel here.
    // ⚠️ MEASURE THE TEAR BEFORE FITTING. See PlanCameraMath.hpp: the four
    // samples may straddle a scroll, and a fit through them describes no view
    // that ever existed. `kMaxTearPixels` is one pixel -- below that the blend
    // cannot displace anything visibly, above it the fit is not worth drawing.
    constexpr double kMaxTearPixels = 1.0;
    const PlanSampleTear tear =
        MeasurePlanSampleTear (topLeft, topRight, topLeftAgain, sampleWidth, kMaxTearPixels);
    // ⚠️ REPORTED EVEN WHEN THE SAMPLE IS DROPPED, and that is the point of the
    // flag. A tear is positive proof the user is navigating RIGHT NOW, one whole
    // tick before "the camera differs from the last one" can say so -- and
    // `hideonnav` exists to react on that first frame.
    start.viewMoving = tear.moving;
    if (!tear.usable) {
        start.source = "the floor plan scrolled " + std::to_string (tear.tearPixels) +
                       " px while its four corners were being sampled; the fit would blend two "
                       "views and land somewhere neither of them is";
        LogOnce ("ArchViz plan camera: torn sample dropped -- " + start.source);
        return start;
    }

    const PlanCameraFit fit =
        FitPlanCamera (topLeft, topRight, bottomLeft, sampleWidth, sampleHeight);
    if (!fit.valid) {
        start.source = "the floor plan's mapping cannot be a camera: " + fit.why;
        LogOnce ("ArchViz plan camera: " + start.source);
        return start;
    }

    // ⚠️ WELL FORMED IS NOT THE SAME AS TRUE (PLAT-RE82). The fit above rules out
    // the mappings a camera cannot express; it cannot tell a good read from a
    // bogus one that happens to be square and uniform. Live data says those
    // happen: 0.87% of plan ticks moved the view centre by more than a whole
    // half-height, the worst by 778 of them, with the half-height jumping
    // 7 m -> 5862 m inside one tick. That is the "jumps to a random location away
    // from the model" the user sees on a fast pan. The guard is continuity, and a
    // rejected tick simply holds the previous pose -- see PlanCameraMath.hpp.
    static PlanCameraContinuity g_continuity;
    static uint64_t g_lastAcceptedTickMs = 0;
    const uint64_t nowMs = GetTickCount64 ();
    const double elapsedSeconds =
        (g_lastAcceptedTickMs == 0) ? 0.0 : double (nowMs - g_lastAcceptedTickMs) / 1000.0;

    std::string rejection;
    if (!AcceptPlanCameraFit (g_continuity, fit, elapsedSeconds, rejection)) {
        start.source = rejection;
        // ⚠️ NOT LogOnce. Every rejection is a distinct event with its own
        // numbers, and the whole value of the line is being able to count them
        // and see how far off the bad reads were -- deduplicating identical text
        // would be fine, but these are never identical, so LogOnce would print
        // every one anyway while implying it had filtered.
        ArchVizLog ("ArchViz plan camera REJECTED: " + rejection);
        return start;
    }
    g_lastAcceptedTickMs = nowMs;

    start.valid = true;
    start.orthographic = true;
    start.target[0] = float (fit.centreX);
    start.target[1] = float (fit.centreY);
    // ⚠️ ZERO, AND IT IS NOT A GUESS AT THE STOREY. A parallel projection
    // straight down produces the same picture from any height, so the only thing
    // this Z decides is which part of the model survives clipping -- and the
    // frustum built from it spans five kilometres either way, which is every
    // real project. Aiming it at the current storey would look more correct and
    // change nothing.
    start.target[2] = kPlanTargetZ;
    start.orthoHalfHeightMetres = float (fit.halfHeightMetres);
    start.planRotationRadians = float (fit.rotationRadians);
    start.source = "floor plan (orthographic)";

    // ⚠️ THE NAV ROW IS *NOT* WRITTEN HERE. It was, briefly, and that was the
    // wrong seam: this function has several callers and only one of them is the
    // poll that drives the overlay. The row is written by CameraSyncTimerProc
    // instead, from the CameraStart this returns, so exactly one row exists per
    // tick of the poll under test -- see ArchVizPanelCamera.cpp.

    LogOnce ("ArchViz plan camera: surface " + std::to_string (widthPx) + "x" +
             std::to_string (heightPx) + " physical px, sampled as " +
             std::to_string (sampleWidth) + "x" + std::to_string (sampleHeight) +
             " logical at scaling " + std::to_string (scale) + " -> centre " +
             std::to_string (fit.centreX) + ", " + std::to_string (fit.centreY) + ", " +
             std::to_string (fit.metresPerPixelY) + " m per logical px, half-height " +
             std::to_string (fit.halfHeightMetres) + " m, rotation " +
             std::to_string (fit.rotationRadians * kRadToDeg) + " deg  |  zoom box " +
             DescribeZoomBox () +
             // ⚠️ THE SCALING IS PRINTED BECAUSE IT IS THE ONE ASSUMPTION LEFT,
             // and its failure signature is now known exactly: an overlay at 1/s
             // of the right size, shifted toward the top left, is this number
             // being applied when it should not be (or the reverse). See
             // WindowPixelScale.
             (scale != 1.0 ? "  |  display scaling IS being applied to the samples"
                           : "  |  no display scaling on this monitor"));
    return start;
}

}   // namespace archviz
}   // namespace geomsrv
