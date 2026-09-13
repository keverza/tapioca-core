// ArchViz/AutoOrbit -- see the header. Every rule about this file is in that
// header's comments; this is the mechanism.

#include "ArchViz/AutoOrbit.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog

#include "ACAPinc.h"

#include <atomic>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace autoorbit {

namespace {

// ⚠️ THE WHOLE STRUCT, NOT A CHOSEN SUBSET OF FIELDS. The first version of this
// feature restored through `Set3DProjection`'s sparse form -- azimuth, distance
// and the two heights -- and after run twelve the user reported the overlay
// drawing the model LARGER than Archicad did. Writing `distance` explicitly is
// enough to do that: Archicad keeps `pos`, `target` and `distance` and they are
// only consistent with each other until something writes one of them on its own,
// at which point the camera moves along its own view axis and the scale changes.
// A saved `API_3DProjectionInfo` written back verbatim cannot have that class of
// mistake, and it also carries the fields the sparse form cannot reach at all --
// `pos`, `target`, `rollAngle`, `isTwoPointPersp`.
API_3DProjectionInfo g_saved = {};
bool                 g_haveSaved = false;

std::atomic<bool>     g_running {false};
std::atomic<uint64_t> g_steps {0};
double                g_degreesPerStep = 0.4;

// ⚠️ HOW `azimuth` RELATES TO `pos`, MEASURED AT `Start` RATHER THAN ASSUMED.
// The DevKit does not say which direction Archicad measures the perspective
// azimuth from, and guessing it wrong turns the camera one way while the stored
// angle says the other. Both are read back at Start, so the offset between them
// is a fact this session can have for the price of one `atan2` -- and the first
// step logs what Archicad did with it, so a wrong sign shows up as a line in
// archviz.log rather than as a mystery in a frame-rate table.
double g_azimuthOffsetDegrees = 0.0;
bool   g_loggedFirstStep = false;

constexpr double kPi = 3.14159265358979323846;

double NormaliseDegrees (double degrees)
{
    while (degrees >= 360.0)
        degrees -= 360.0;
    while (degrees < 0.0)
        degrees += 360.0;
    return degrees;
}

// The compass-free bearing of the camera as seen from its target, in degrees.
double BearingOf (const API_PerspPars& p)
{
    return NormaliseDegrees (std::atan2 (p.pos.y - p.target.y, p.pos.x - p.target.x) *
                             180.0 / kPi);
}

}   // namespace

bool Start (double degreesPerStep, GS::UniString& error)
{
    // ⚠️ CLAMPED, NOT TRUSTED, and the low end matters more than the high. A
    // step of zero would leave the view still while the report said it was
    // orbiting -- a phase that measures an idle window very precisely and calls
    // it a frame rate. The high end only stops a caller from teleporting the
    // camera in a way that makes consecutive frames share no geometry.
    if (!(degreesPerStep > 0.0) || !std::isfinite (degreesPerStep)) {
        error = "degreesPerStep must be a positive, finite number of degrees";
        return false;
    }
    g_degreesPerStep = (degreesPerStep > 15.0) ? 15.0 : degreesPerStep;

    // ⚠️ A SECOND START ONLY CHANGES THE RATE. Saving again would save an
    // already-rotated projection, and `Stop` would then restore the view to
    // wherever the previous run had left it rather than to where the user had
    // it.
    if (g_running.load (std::memory_order_acquire))
        return true;

    API_3DProjectionInfo projection = {};
    const GSErrCode err = ACAPI_View_Get3DProjectionSets (&projection);
    if (err != NoError) {
        error = "ACAPI_View_Get3DProjectionSets failed (" +
                GS::UniString (std::to_string ((int) err).c_str ()) + ")";
        return false;
    }
    if (!projection.isPersp) {
        error = "the 3D window is axonometric; there is no camera to orbit and "
                "stage 3 has nothing to score against in one either";
        return false;
    }

    g_saved = projection;
    g_haveSaved = true;
    g_azimuthOffsetDegrees =
        NormaliseDegrees (projection.u.persp.azimuth - BearingOf (projection.u.persp));
    g_loggedFirstStep = false;
    g_steps.store (0, std::memory_order_relaxed);
    g_running.store (true, std::memory_order_release);
    ArchVizLog ("auto orbit: started at " + std::to_string (g_degreesPerStep) +
                " deg per camera tick; azimuth reads " +
                std::to_string (projection.u.persp.azimuth) + " at bearing " +
                std::to_string (BearingOf (projection.u.persp)) + " (offset " +
                std::to_string (g_azimuthOffsetDegrees) +
                "); the projection is saved and will be restored");
    return true;
}

void Stop ()
{
    const bool wasRunning = g_running.exchange (false, std::memory_order_acq_rel);
    if (!g_haveSaved)
        return;

    // ⚠️ RESTORED EVEN IF IT WAS NOT RUNNING, because "not running" is also what
    // a failed step leaves behind, and the view would still be turned.
    const GSErrCode err = ACAPI_View_Change3DProjectionSets (&g_saved);
    ACAPI_View_Redraw ();
    g_haveSaved = false;
    if (err != NoError) {
        // Worth a log line even though nothing can be done about it here: the
        // user's view is rotated and there is no undo step for a view setting.
        ArchVizLog ("auto orbit: WARNING -- could not restore the projection, "
                    "ACAPI_View_Change3DProjectionSets returned " +
                    std::to_string ((int) err) + "; the 3D view is left rotated");
        return;
    }
    if (wasRunning) {
        ArchVizLog ("auto orbit: stopped after " +
                    std::to_string (g_steps.load (std::memory_order_relaxed)) +
                    " steps; the projection is back where it was");
    }
}

void StepIfRunning ()
{
    if (!g_running.load (std::memory_order_acquire))
        return;

    // ⚠️ READ, ADD, WRITE, EVERY TICK -- NOT AN ACCUMULATOR. If anything else
    // moves the camera between two steps (the user, another command) an
    // accumulated angle would snap the view back to our own idea of where it
    // should be. Reading first means the orbit rides on top of whatever the view
    // is actually doing.
    API_3DProjectionInfo projection = {};
    if (ACAPI_View_Get3DProjectionSets (&projection) != NoError || !projection.isPersp) {
        // The window turned axonometric under us, or the read failed. Stopping
        // puts the saved projection back rather than leaving a half-turned view.
        ArchVizLog ("auto orbit: the 3D projection could not be read as a perspective; stopping");
        Stop ();
        return;
    }

    // ⚠️ THE CAMERA POSITION IS ROTATED, NOT JUST THE STORED ANGLE, AND THAT IS
    // THE DIFFERENCE BETWEEN THIS WORKING AND RUN TWELVE. The first version
    // advanced `azimuth` alone. The overlay's own camera read
    // (`ArchVizPanel::ReadArchicadCamera`) takes its eye from `pos`/`cameraZ`
    // and its target from `target`/`targetZ` and NEVER LOOKS AT `azimuth` -- so
    // an azimuth-only write moved nothing the overlay could see, the user
    // reported that the view had not orbited, and the overlay and the 3D window
    // ended up describing different cameras, which is what the overlay-scale
    // complaint was.
    //
    // Rotating `pos` about `target` in plan is unambiguous: it is the same fact
    // every reader uses. `azimuth` is then set from the rotated bearing through
    // the offset measured at Start, so the two stay consistent whichever one
    // Archicad treats as authoritative.
    API_PerspPars& persp = projection.u.persp;
    const double angle = g_degreesPerStep * kPi / 180.0;
    const double cosA = std::cos (angle);
    const double sinA = std::sin (angle);
    const double dx = persp.pos.x - persp.target.x;
    const double dy = persp.pos.y - persp.target.y;
    persp.pos.x = persp.target.x + dx * cosA - dy * sinA;
    persp.pos.y = persp.target.y + dx * sinA + dy * cosA;
    persp.azimuth = NormaliseDegrees (BearingOf (persp) + g_azimuthOffsetDegrees);

    if (ACAPI_View_Change3DProjectionSets (&projection) != NoError) {
        ArchVizLog ("auto orbit: the projection write failed; stopping");
        Stop ();
        return;
    }

    // ⚠️ THE FIRST STEP IS READ BACK AND LOGGED, ONCE. Archicad may keep what we
    // wrote, or recompute `pos` from `azimuth`, or clamp something -- and which
    // it does decides whether this whole mechanism turns the camera at all. One
    // line in archviz.log answers it for good; a run that quietly failed to
    // orbit is a frame-rate table that means nothing, and that has already
    // happened once.
    if (!g_loggedFirstStep) {
        g_loggedFirstStep = true;
        API_3DProjectionInfo readBack = {};
        if (ACAPI_View_Get3DProjectionSets (&readBack) == NoError && readBack.isPersp) {
            ArchVizLog ("auto orbit: first step asked for pos (" +
                        std::to_string (persp.pos.x) + ", " + std::to_string (persp.pos.y) +
                        ") azimuth " + std::to_string (persp.azimuth) + "; Archicad reports pos (" +
                        std::to_string (readBack.u.persp.pos.x) + ", " +
                        std::to_string (readBack.u.persp.pos.y) + ") azimuth " +
                        std::to_string (readBack.u.persp.azimuth));
        }
    }
    // ⚠️ THE REDRAW IS THE POINT. Changing the settings alone leaves the 3D
    // window showing what it last drew, so the phase would measure an idle
    // window while the overlay -- which follows the same settings -- swung away
    // from it. That divergence is exactly what the overlay-scale complaint after
    // run twelve looked like.
    ACAPI_View_Redraw ();
    g_steps.fetch_add (1, std::memory_order_relaxed);
}

bool IsRunning ()
{
    return g_running.load (std::memory_order_acquire);
}

uint64_t StepsTaken ()
{
    return g_steps.load (std::memory_order_relaxed);
}

}   // namespace autoorbit
}   // namespace archviz
}   // namespace geomsrv
