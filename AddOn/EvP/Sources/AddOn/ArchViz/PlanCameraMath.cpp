#include "ArchViz/PlanCameraMath.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

// A mapping is refused past these. Both are generous: they exist to catch a
// mapping that is not a rotation+scale at all (a failed sample returning zeros,
// a window with rulers included on one axis only), not to police rounding.
constexpr double kMaxSquareness = 0.01;   // ~0.6 degrees out of square
constexpr double kMaxScaleSkew = 0.02;    // 2 percent between the two axes

double Length (double x, double y) { return std::sqrt (x * x + y * y); }

}   // namespace

PlanCameraFit FitPlanCamera (const double topLeft[2], const double topRight[2],
                             const double bottomLeft[2], uint32_t widthPx, uint32_t heightPx)
{
    PlanCameraFit fit;
    if (topLeft == nullptr || topRight == nullptr || bottomLeft == nullptr) {
        fit.why = "no samples";
        return fit;
    }
    if (widthPx < 2 || heightPx < 2) {
        fit.why = "the window is too small to measure a mapping from";
        return fit;
    }

    // The two window axes, in model metres per pixel. `down` is the screen's +Y,
    // which points DOWN -- so for an unrotated plan it is model -Y.
    const double rightX = (topRight[0] - topLeft[0]) / double (widthPx);
    const double rightY = (topRight[1] - topLeft[1]) / double (widthPx);
    const double downX = (bottomLeft[0] - topLeft[0]) / double (heightPx);
    const double downY = (bottomLeft[1] - topLeft[1]) / double (heightPx);

    fit.metresPerPixelX = Length (rightX, rightY);
    fit.metresPerPixelY = Length (downX, downY);
    // ⚠️ A ZERO AXIS IS THE ORDINARY FAILURE, not an exotic one: it is what a
    // sample against a window that has no database, or a call that quietly did
    // nothing, produces. Dividing by it below would hand back a NaN camera, and
    // a NaN view matrix renders as an empty overlay -- indistinguishable from
    // the renderer having died.
    if (!(fit.metresPerPixelX > 1e-12) || !(fit.metresPerPixelY > 1e-12)) {
        fit.why = "the window's pixel-to-model mapping is degenerate (a zero axis): the "
                  "samples did not come from a 2D drawing window with a live database";
        return fit;
    }

    const double ux = rightX / fit.metresPerPixelX;
    const double uy = rightY / fit.metresPerPixelX;
    const double dx = downX / fit.metresPerPixelY;
    const double dy = downY / fit.metresPerPixelY;

    fit.squareness = std::fabs (ux * dx + uy * dy);
    fit.scaleRatio = fit.metresPerPixelX / fit.metresPerPixelY;
    // For a non-mirrored view, screen-down is screen-right turned CLOCKWISE by a
    // quarter turn: (x, y) -> (y, -x). If the actual down axis points the other
    // way the mapping includes a reflection.
    fit.mirrored = (dx * uy - dy * ux) < 0.0;

    fit.rotationRadians = std::atan2 (uy, ux);
    fit.halfHeightMetres = fit.metresPerPixelY * double (heightPx) * 0.5;
    fit.centreX = topLeft[0] + rightX * double (widthPx) * 0.5 + downX * double (heightPx) * 0.5;
    fit.centreY = topLeft[1] + rightY * double (widthPx) * 0.5 + downY * double (heightPx) * 0.5;

    if (fit.squareness > kMaxSquareness) {
        fit.why = "the window's two axes are not perpendicular in model space, so the view is "
                  "sheared and no camera pose can reproduce it";
        return fit;
    }
    if (std::fabs (fit.scaleRatio - 1.0) > kMaxScaleSkew) {
        fit.why = "the window's horizontal and vertical scales differ, so the view is "
                  "anisotropic and one orthographic frustum cannot reproduce it";
        return fit;
    }
    if (fit.mirrored) {
        // ⚠️ REFUSED RATHER THAN NEGATED. A mirror is determinant -1 and this
        // repo has shipped one before (Camera.hpp): it renders perfectly and
        // swaps north with south, which is the one rendering fault that looks
        // entirely fine. If a real Archicad view turns out to produce this, the
        // answer is to find out WHY, not to add a sign here.
        fit.why = "the window's mapping is MIRRORED (its down axis is on the wrong side of "
                  "its right axis); refusing rather than rendering a reflected model";
        return fit;
    }

    fit.valid = true;
    return fit;
}

PlanSampleTear MeasurePlanSampleTear (const double topLeft[2], const double topRight[2],
                                      const double topLeftAgain[2], uint32_t widthPx,
                                      double maxTearPixels)
{
    PlanSampleTear tear;
    if (topLeft == nullptr || topRight == nullptr || topLeftAgain == nullptr || widthPx < 2)
        return tear;

    const double drift = Length (topLeftAgain[0] - topLeft[0], topLeftAgain[1] - topLeft[1]);

    // The scale, straight off the raw samples. It is itself perturbed by the
    // tear, but only in proportion to it -- and this number exists solely to
    // express the tear in pixels, where being a few percent out changes nothing.
    const double metresPerPixel =
        Length (topRight[0] - topLeft[0], topRight[1] - topLeft[1]) / double (widthPx);
    if (!(metresPerPixel > 1e-12))
        return tear;

    tear.tearPixels = drift / metresPerPixel;
    // A quarter pixel. Below it the view is either still or moving too slowly to
    // displace anything a human could see, and the threshold keeps last-digit
    // differences from declaring motion on every frame.
    tear.moving = tear.tearPixels > 0.25;
    tear.usable = tear.tearPixels <= maxTearPixels;
    return tear;
}

namespace {

// Shortest signed way round from `from` to `to`. ⚠️ WITHOUT THIS, A ROTATION
// CROSSING PI EXTRAPOLATES THE LONG WAY -- a hundredth of a turn reads as
// three-hundred-odd degrees of angular velocity, and the overlay spins.
double AngleDelta (double from, double to)
{
    constexpr double kPi = 3.14159265358979323846;
    double delta = to - from;
    while (delta > kPi)
        delta -= 2.0 * kPi;
    while (delta < -kPi)
        delta += 2.0 * kPi;
    return delta;
}

// Clamp `step` to `limit` in magnitude, preserving sign.
double ClampStep (double step, double limit)
{
    const double bound = limit < 0.0 ? -limit : limit;
    if (step > bound)
        return bound;
    if (step < -bound)
        return -bound;
    return step;
}

// Move `consistency` toward 1 when this step agrees with the last, and drop it
// to 0 the moment they disagree.
//
// `agreement` is the dot product of the two steps (or the product, for a scalar
// channel) and `magnitude` is what it would be if they pointed exactly the same
// way. Their ratio is the cosine of the turn: 1 is dead straight, 0 is a right
// angle, negative is a reversal.
//
// ⚠️ ASYMMETRIC ON PURPOSE -- IT COLLAPSES IN ONE SAMPLE AND REBUILDS OVER FIVE.
// A reversal that keeps any trust at all still throws the overlay past the model
// on the frame the user is watching for exactly that, while being slow to trust
// a steady pan costs a few milliseconds of lag nobody can see. The two errors
// are not remotely equal, so the response to them is not either.
double UpdateConsistency (double consistency, double agreement, double magnitude)
{
    // Too small to have a direction: hold, do not punish. A pause mid-drag is
    // not a reversal, and treating it as one would restart the ramp every time
    // the user's hand hesitated.
    if (!(magnitude > 1e-12))
        return consistency;
    const double cosine = agreement / magnitude;
    if (cosine <= 0.0)
        return 0.0;   // a right angle or worse -- trust nothing
    const double target = cosine;
    return consistency + (target - consistency) * kConsistencyGain;
}

// The steps-ahead multiple this much consistency has earned.
double StepsAllowed (double consistency)
{
    const double clamped = consistency < 0.0 ? 0.0 : (consistency > 1.0 ? 1.0 : consistency);
    return kMinPredictedStepsAhead +
           (kMaxPredictedStepsAhead - kMinPredictedStepsAhead) * clamped;
}

}   // namespace

PlanCameraFit PredictPlanCamera (PlanCameraPredictor& state, const PlanCameraFit& observed,
                                 double elapsedSeconds, double horizonSeconds)
{
    if (!observed.valid || observed.halfHeightMetres <= 0.0)
        return observed;

    if (!state.have || !(elapsedSeconds > 0.0) || state.halfHeightMetres <= 0.0) {
        // First observation, or no usable interval to measure a rate over. Adopt
        // it and predict nothing -- a velocity invented from one sample is a
        // guess, and a guess here moves the picture.
        state.have = true;
        state.centreX = observed.centreX;
        state.centreY = observed.centreY;
        state.halfHeightMetres = observed.halfHeightMetres;
        state.rotationRadians = observed.rotationRadians;
        state.centreVelocityX = 0.0;
        state.centreVelocityY = 0.0;
        state.logHalfHeightRate = 0.0;
        state.rotationRate = 0.0;
        state.previousStepX = 0.0;
        state.previousStepY = 0.0;
        state.previousStepRotation = 0.0;
        state.previousStepLogHalfHeight = 0.0;
        state.centreConsistency = 0.0;
        state.rotationConsistency = 0.0;
        state.zoomConsistency = 0.0;
        return observed;
    }

    // ---- what just happened, as rates ------------------------------------
    const double stepX = observed.centreX - state.centreX;
    const double stepY = observed.centreY - state.centreY;
    const double stepRotation = AngleDelta (state.rotationRadians, observed.rotationRadians);
    // Geometric, as the header explains: a zoom multiplies.
    const double stepLogHalfHeight =
        std::log (observed.halfHeightMetres / state.halfHeightMetres);

    const double blend = kVelocityBlend;
    state.centreVelocityX = state.centreVelocityX * (1.0 - blend) + (stepX / elapsedSeconds) * blend;
    state.centreVelocityY = state.centreVelocityY * (1.0 - blend) + (stepY / elapsedSeconds) * blend;
    state.rotationRate =
        state.rotationRate * (1.0 - blend) + (stepRotation / elapsedSeconds) * blend;
    state.logHalfHeightRate =
        state.logHalfHeightRate * (1.0 - blend) + (stepLogHalfHeight / elapsedSeconds) * blend;

    // ---- has this motion been steady? -------------------------------------
    // Compared against the PREVIOUS step, before it is overwritten. The centre
    // uses a dot product because a pan has a direction in the plane and a
    // sign-per-axis test would call a 90-degree turn "consistent in Y".
    state.centreConsistency = UpdateConsistency (
        state.centreConsistency,
        state.previousStepX * stepX + state.previousStepY * stepY,
        std::hypot (state.previousStepX, state.previousStepY) * std::hypot (stepX, stepY));
    state.rotationConsistency = UpdateConsistency (
        state.rotationConsistency, state.previousStepRotation * stepRotation,
        std::fabs (state.previousStepRotation * stepRotation));
    state.zoomConsistency = UpdateConsistency (
        state.zoomConsistency, state.previousStepLogHalfHeight * stepLogHalfHeight,
        std::fabs (state.previousStepLogHalfHeight * stepLogHalfHeight));

    state.previousStepX = stepX;
    state.previousStepY = stepY;
    state.previousStepRotation = stepRotation;
    state.previousStepLogHalfHeight = stepLogHalfHeight;

    state.centreX = observed.centreX;
    state.centreY = observed.centreY;
    state.halfHeightMetres = observed.halfHeightMetres;
    state.rotationRadians = observed.rotationRadians;

    if (!(horizonSeconds > 0.0))
        return observed;

    // ---- where it will be, bounded by what it just did --------------------
    // ⚠️ THE CLAMP IS AGAINST THE STEP JUST OBSERVED, NOT A FIXED DISTANCE. That
    // is what makes an abrupt stop cheap: the moment the view stops, the observed
    // step collapses to zero and so does the allowance, so the blended velocity
    // has nothing to spend. Without it a decaying velocity would keep pushing the
    // overlay past the model for several ticks -- the matrix's
    // `bounces past and returns`.
    //
    // ⚠️ THE MULTIPLE NOW FOLLOWS THE MOTION. See the header: one fixed 1.5 was
    // simultaneously too tight for a steady pan (it capped the lag at 14-16 ms
    // no matter how far ahead the horizon asked for) and too loose for a fast
    // reversal (it still bounced).
    const double centreSteps = StepsAllowed (state.centreConsistency);
    const double rotationSteps = StepsAllowed (state.rotationConsistency);
    const double zoomSteps = StepsAllowed (state.zoomConsistency);

    const double allowanceX = std::fabs (stepX) * centreSteps;
    const double allowanceY = std::fabs (stepY) * centreSteps;
    const double allowanceRotation = std::fabs (stepRotation) * rotationSteps;
    const double allowanceLog =
        std::min (std::fabs (stepLogHalfHeight) * zoomSteps, kMaxPredictedLogZoom);

    PlanCameraFit predicted = observed;
    predicted.centreX += ClampStep (state.centreVelocityX * horizonSeconds, allowanceX);
    predicted.centreY += ClampStep (state.centreVelocityY * horizonSeconds, allowanceY);
    predicted.rotationRadians +=
        ClampStep (state.rotationRate * horizonSeconds, allowanceRotation);
    predicted.halfHeightMetres =
        observed.halfHeightMetres *
        std::exp (ClampStep (state.logHalfHeightRate * horizonSeconds, allowanceLog));
    return predicted;
}

bool AcceptPlanCameraFit (PlanCameraContinuity& state, const PlanCameraFit& candidate,
                          double elapsedSeconds, std::string& why)
{
    // An invalid fit never becomes the reference -- adopting a refused mapping's
    // numbers would let the next good one be measured against nonsense.
    if (!candidate.valid) {
        why = candidate.why;
        return false;
    }

    // ⚠️ A BAD CLOCK MUST NOT MAKE THE GUARD PERMISSIVE. The budgets below scale
    // with elapsed time, so a large or negative dt would wave anything through.
    // One tick is the floor; the sync poll cannot run faster than that anyway.
    constexpr double kOneTickSeconds = 0.016;
    constexpr double kMaxBudgetSeconds = 1.0;
    double dt = elapsedSeconds;
    if (!(dt > kOneTickSeconds))
        dt = kOneTickSeconds;
    if (dt > kMaxBudgetSeconds)
        dt = kMaxBudgetSeconds;

    if (!state.have || candidate.halfHeightMetres <= 0.0 || state.halfHeightMetres <= 0.0) {
        state.have = true;
        state.centreX = candidate.centreX;
        state.centreY = candidate.centreY;
        state.halfHeightMetres = candidate.halfHeightMetres;
        state.rejectedInARow = 0;
        return true;
    }

    // ⚠️ MEASURED AGAINST THE SMALLER OF THE TWO HALF-HEIGHTS. Using the
    // candidate's would make a bogus zoom-OUT self-justifying: a fit claiming a
    // 5 km view gives itself a 5 km movement budget, so the very sample that is
    // most obviously wrong is the one that passes.
    const double reference = state.halfHeightMetres < candidate.halfHeightMetres
                                 ? state.halfHeightMetres
                                 : candidate.halfHeightMetres;
    const double moved = Length (candidate.centreX - state.centreX,
                                 candidate.centreY - state.centreY);
    const double movedInHalfHeights = moved / reference;
    const double movedBudget = kMaxHalfHeightsPerSecond * dt;

    const double zoomRatio = candidate.halfHeightMetres > state.halfHeightMetres
                                 ? candidate.halfHeightMetres / state.halfHeightMetres
                                 : state.halfHeightMetres / candidate.halfHeightMetres;
    const double zoomBudget = 1.0 + (kMaxZoomFactorPerSecond - 1.0) * dt;

    const bool movedTooFar = movedInHalfHeights > movedBudget;
    const bool zoomedTooFast = zoomRatio > zoomBudget;

    if (movedTooFar || zoomedTooFast) {
        ++state.rejectedInARow;
        if (state.rejectedInARow <= kMaxConsecutiveRejects) {
            why = "discontinuous plan camera: centre moved " +
                  std::to_string (movedInHalfHeights) + " half-heights (budget " +
                  std::to_string (movedBudget) + ") and zoom changed x" +
                  std::to_string (zoomRatio) + " (budget x" + std::to_string (zoomBudget) +
                  ") in " + std::to_string (dt) + " s; holding the previous camera";
            return false;
        }
        // The escape. A discontinuity this persistent is a real view change --
        // Fit in Window, a storey switch, a saved view -- not a bad read.
        why.clear ();
    }

    state.centreX = candidate.centreX;
    state.centreY = candidate.centreY;
    state.halfHeightMetres = candidate.halfHeightMetres;
    state.rejectedInARow = 0;
    return true;
}

PlanReprojection ComputePlanReprojection (double renderedCentreX, double renderedCentreY,
                                          double renderedHalfHeight, double renderedRotation,
                                          double currentCentreX, double currentCentreY,
                                          double currentHalfHeight, double currentRotation)
{
    PlanReprojection warp;
    if (!(renderedHalfHeight > 0.0) || !(currentHalfHeight > 0.0))
        return warp;   // an unusable camera warps to nothing; blit unwarped

    // A model point seen at view offset e1 (in half-heights) under the CURRENT
    // camera sits at model position
    //     m = c1 + R(r1) * e1 * h1
    // and the RENDERED camera put that same point at
    //     e0 = R(-r0) * (m - c0) / h0
    //        = R(-r0) * (c1 - c0) / h0  +  (h1 / h0) * R(r1 - r0) * e1
    // which is one scale, one rotation and one offset -- the whole warp.
    const double delta = currentRotation - renderedRotation;
    warp.scale = float (currentHalfHeight / renderedHalfHeight);
    warp.cosDelta = float (std::cos (delta));
    warp.sinDelta = float (std::sin (delta));

    const double dx = currentCentreX - renderedCentreX;
    const double dy = currentCentreY - renderedCentreY;
    // R(-r0), the same rotation Camera::GetViewMatrix applies to reach view space.
    const double cos0 = std::cos (renderedRotation);
    const double sin0 = std::sin (renderedRotation);
    warp.offsetX = float ((dx * cos0 + dy * sin0) / renderedHalfHeight);
    warp.offsetY = float ((-dx * sin0 + dy * cos0) / renderedHalfHeight);
    warp.valid = true;
    return warp;
}

}   // namespace archviz
}   // namespace geomsrv
