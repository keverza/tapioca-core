// ArchViz/PlanCameraMath — the floor plan's pixel-to-model mapping, turned into
// a top-down orthographic camera.
//
// WHAT THESE TESTS ARE FOR. The input comes from three `ACAPI_View_PointToCoord`
// samples in Archicad, so the only way to exercise the interesting cases is to
// build the mappings by hand here: a plan is never sheared or mirrored on
// purpose, and the reason those are REFUSED is precisely that they would render
// confidently and wrongly.
//
// ⚠️ THE PROPERTY, NOT THE ARITHMETIC. Each test builds a mapping from a scale,
// a rotation and an origin, then checks the fit recovers those -- rather than
// re-deriving the same formula and comparing it with itself.

#include "ArchViz/PlanCameraMath.hpp"

#include <gtest/gtest.h>

#include <cmath>

using geomsrv::archviz::FitPlanCamera;
using geomsrv::archviz::PlanCameraFit;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A window whose top-left pixel is `originX/originY` in model space, scaled by
// `metresPerPixel` and rotated CCW by `rotation`. Screen +Y points DOWN the
// screen, which for an unrotated plan is model -Y.
struct Mapping {
    double originX = 0.0;
    double originY = 0.0;
    double metresPerPixel = 0.01;
    double rotation = 0.0;

    void At (double pixelX, double pixelY, double out[2]) const
    {
        const double c = std::cos (rotation);
        const double s = std::sin (rotation);
        // right = (c, s), down = (s, -c): a quarter turn CLOCKWISE from right,
        // which is what makes the screen's Y axis point down the model.
        out[0] = originX + metresPerPixel * (pixelX * c + pixelY * s);
        out[1] = originY + metresPerPixel * (pixelX * s - pixelY * c);
    }
};

}   // namespace

TEST (PlanCameraMath, RecoversScaleCentreAndRotationOfAnUnrotatedPlan)
{
    // A deliberately off-origin plan at a realistic zoom: 8 mm of model per
    // pixel is roughly a 1:100 drawing on a 96 dpi screen.
    Mapping map;
    map.originX = 112.0;
    map.originY = 48.0;
    map.metresPerPixel = 0.008;

    constexpr uint32_t kWidth = 1600;
    constexpr uint32_t kHeight = 1200;

    double topLeft[2], topRight[2], bottomLeft[2];
    map.At (0.0, 0.0, topLeft);
    map.At (double (kWidth), 0.0, topRight);
    map.At (0.0, double (kHeight), bottomLeft);

    const PlanCameraFit fit = FitPlanCamera (topLeft, topRight, bottomLeft, kWidth, kHeight);
    ASSERT_TRUE (fit.valid) << fit.why;

    EXPECT_NEAR (fit.metresPerPixelX, map.metresPerPixel, 1e-9);
    EXPECT_NEAR (fit.metresPerPixelY, map.metresPerPixel, 1e-9);
    EXPECT_NEAR (fit.rotationRadians, 0.0, 1e-9);
    EXPECT_FALSE (fit.mirrored);
    EXPECT_NEAR (fit.scaleRatio, 1.0, 1e-9);

    // The camera's half-height must be half the SURFACE, or the overlay is
    // drawn at a different zoom from the plan under it.
    EXPECT_NEAR (fit.halfHeightMetres, map.metresPerPixel * kHeight * 0.5, 1e-9);

    // And its target must be the model point under the middle pixel.
    double centre[2];
    map.At (kWidth * 0.5, kHeight * 0.5, centre);
    EXPECT_NEAR (fit.centreX, centre[0], 1e-9);
    EXPECT_NEAR (fit.centreY, centre[1], 1e-9);
}

TEST (PlanCameraMath, RecoversTheRotationOfARotatedPlan)
{
    // ⚠️ A ROTATED PLAN IS THE CASE THAT SEPARATES A REAL FIT FROM A SCALE
    // FACTOR. Everything else about this file works if the rotation is quietly
    // dropped -- the overlay is simply turned, which on a nearly square building
    // reads as the model being wrong rather than the camera.
    for (const double rotation : {0.35, -1.1, 2.6}) {
        Mapping map;
        map.originX = -20.0;
        map.originY = 7.5;
        map.metresPerPixel = 0.02;
        map.rotation = rotation;

        double topLeft[2], topRight[2], bottomLeft[2];
        map.At (0.0, 0.0, topLeft);
        map.At (900.0, 0.0, topRight);
        map.At (0.0, 700.0, bottomLeft);

        const PlanCameraFit fit = FitPlanCamera (topLeft, topRight, bottomLeft, 900, 700);
        ASSERT_TRUE (fit.valid) << fit.why << " at rotation " << rotation;
        EXPECT_NEAR (fit.rotationRadians, rotation, 1e-9);
        EXPECT_NEAR (fit.metresPerPixelY, map.metresPerPixel, 1e-9);
        EXPECT_FALSE (fit.mirrored);
    }
}

TEST (PlanCameraMath, RotationStaysContinuousAcrossThePiBoundary)
{
    // atan2 wraps at +/-pi, and a camera whose roll jumps by a full turn between
    // two neighbouring plan rotations would be a picture that flips. The angle
    // itself may come back as either sign; what must hold is that it names the
    // same DIRECTION.
    Mapping map;
    map.metresPerPixel = 0.05;
    map.rotation = kPi - 1e-4;

    double topLeft[2], topRight[2], bottomLeft[2];
    map.At (0.0, 0.0, topLeft);
    map.At (640.0, 0.0, topRight);
    map.At (0.0, 480.0, bottomLeft);

    const PlanCameraFit fit = FitPlanCamera (topLeft, topRight, bottomLeft, 640, 480);
    ASSERT_TRUE (fit.valid) << fit.why;
    EXPECT_NEAR (std::cos (fit.rotationRadians), std::cos (map.rotation), 1e-9);
    EXPECT_NEAR (std::sin (fit.rotationRadians), std::sin (map.rotation), 1e-9);
}

TEST (PlanCameraMath, RefusesAMirroredMapping)
{
    // Screen-down on the WRONG side of screen-right. This is determinant -1: it
    // renders perfectly and swaps north with south, which is the one rendering
    // fault that looks entirely fine (Camera.hpp's handedness note).
    const double topLeft[2] = {0.0, 0.0};
    const double topRight[2] = {10.0, 0.0};
    const double bottomLeft[2] = {0.0, 8.0};   // +Y DOWN the screen: mirrored

    const PlanCameraFit fit = FitPlanCamera (topLeft, topRight, bottomLeft, 1000, 800);
    EXPECT_FALSE (fit.valid);
    EXPECT_TRUE (fit.mirrored);
    EXPECT_FALSE (fit.why.empty ());
}

TEST (PlanCameraMath, RefusesAShearedMapping)
{
    // The two axes forty-odd degrees out of square. No camera pose produces
    // this, so a fit that "succeeded" would be a silently wrong overlay.
    const double topLeft[2] = {0.0, 0.0};
    const double topRight[2] = {10.0, 0.0};
    const double bottomLeft[2] = {8.0, -8.0};

    const PlanCameraFit fit = FitPlanCamera (topLeft, topRight, bottomLeft, 1000, 800);
    EXPECT_FALSE (fit.valid);
    EXPECT_GT (fit.squareness, 0.01);
}

TEST (PlanCameraMath, RefusesAnAnisotropicMapping)
{
    // Square axes, different scales: a plan cannot be stretched, and one
    // orthographic frustum cannot reproduce it if it were.
    const double topLeft[2] = {0.0, 0.0};
    const double topRight[2] = {10.0, 0.0};
    const double bottomLeft[2] = {0.0, -20.0};

    const PlanCameraFit fit = FitPlanCamera (topLeft, topRight, bottomLeft, 1000, 1000);
    EXPECT_FALSE (fit.valid);
    EXPECT_NEAR (fit.squareness, 0.0, 1e-9);
    EXPECT_GT (std::fabs (fit.scaleRatio - 1.0), 0.02);
}

TEST (PlanCameraMath, RefusesDegenerateSamplesRatherThanReturningNaN)
{
    // ⚠️ THIS IS THE ORDINARY FAILURE, NOT AN EXOTIC ONE: it is what a sample
    // against a window with no live database looks like. A NaN camera renders as
    // an EMPTY overlay, which is indistinguishable from the renderer having died.
    const double zero[2] = {0.0, 0.0};
    const PlanCameraFit fit = FitPlanCamera (zero, zero, zero, 1000, 800);
    EXPECT_FALSE (fit.valid);
    EXPECT_FALSE (std::isnan (fit.halfHeightMetres));
    EXPECT_FALSE (fit.why.empty ());

    const double topLeft[2] = {0.0, 0.0};
    const double topRight[2] = {10.0, 0.0};
    const double bottomLeft[2] = {0.0, -8.0};
    EXPECT_FALSE (FitPlanCamera (topLeft, topRight, bottomLeft, 1, 800).valid);
    EXPECT_FALSE (FitPlanCamera (topLeft, topRight, bottomLeft, 1000, 0).valid);
}

// ---------------------------------------------------------------------------
// The CONTINUITY guard (PLAT-RE82).
//
// ⚠️ THESE NUMBERS ARE FROM LIVE DATA, NOT INVENTED. 9,887 plan samples were
// logged on 2026-08-13; 0.87% of consecutive ticks moved the view centre by more
// than a whole half-height and the worst moved it 778, while the half-height
// itself jumped 7.18 m -> 5862.65 m in one 30 ms tick. The user's report of the
// same runs was "when panning fast the overlay first jumps to a random location
// away from the model and then starts to overlay geometry". The cases below are
// those samples, so a future change that lets them through fails here.

namespace {

using geomsrv::archviz::AcceptPlanCameraFit;
using geomsrv::archviz::PlanCameraContinuity;

// A fit that has already passed FitPlanCamera -- square, uniform, unmirrored.
// The guard's whole point is that WELL FORMED does not mean TRUE.
PlanCameraFit GoodFit (double centreX, double centreY, double halfHeight)
{
    PlanCameraFit fit;
    fit.valid = true;
    fit.centreX = centreX;
    fit.centreY = centreY;
    fit.halfHeightMetres = halfHeight;
    fit.metresPerPixelY = halfHeight * 2.0 / 1000.0;
    fit.metresPerPixelX = fit.metresPerPixelY;
    fit.scaleRatio = 1.0;
    return fit;
}

constexpr double kTick = 0.016;

}   // namespace

TEST (PlanCameraContinuityTest, FirstFitIsAlwaysAccepted)
{
    PlanCameraContinuity state;
    std::string why;
    EXPECT_TRUE (AcceptPlanCameraFit (state, GoodFit (10.0, 20.0, 7.18), 0.0, why));
    EXPECT_TRUE (state.have);
}

TEST (PlanCameraContinuityTest, InvalidFitNeverBecomesTheReference)
{
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));

    PlanCameraFit refused;           // valid == false, as FitPlanCamera left it
    refused.why = "sheared";
    refused.centreX = 9999.0;
    refused.halfHeightMetres = 5862.65;
    EXPECT_FALSE (AcceptPlanCameraFit (state, refused, kTick, why));
    // The reference must be untouched, or the next good fit is judged against
    // the numbers of a mapping that was already rejected.
    EXPECT_DOUBLE_EQ (state.centreX, 0.0);
    EXPECT_DOUBLE_EQ (state.halfHeightMetres, 7.18);
}

TEST (PlanCameraContinuityTest, OrdinaryPanIsAccepted)
{
    // A brisk pan: a third of the view per tick is ~20 view-widths a second,
    // which is faster than a hand moves and must still pass.
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    for (int tick = 1; tick <= 20; ++tick) {
        const double x = double (tick) * 7.18 / 3.0;
        EXPECT_TRUE (AcceptPlanCameraFit (state, GoodFit (x, 0.0, 7.18), kTick, why))
            << "tick " << tick << ": " << why;
    }
}

TEST (PlanCameraContinuityTest, OrdinaryWheelZoomIsAccepted)
{
    // Wheel notches compound; 1.25x per tick for a second is a very fast spin.
    PlanCameraContinuity state;
    std::string why;
    double half = 7.18;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, half), 0.0, why));
    for (int tick = 0; tick < 20; ++tick) {
        half *= 1.25;
        EXPECT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, half), kTick, why))
            << "tick " << tick << ": " << why;
    }
}

TEST (PlanCameraContinuityTest, TheMeasured778HalfHeightTeleportIsRejected)
{
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    // 5585.58 m from a 7.18 m view, in one 30 ms tick -- the worst live sample.
    EXPECT_FALSE (AcceptPlanCameraFit (state, GoodFit (5585.58, 0.0, 7.18), 0.030, why));
    EXPECT_FALSE (why.empty ());
}

TEST (PlanCameraContinuityTest, TheMeasured816xZoomJumpIsRejected)
{
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    // 7.18 m -> 5862.65 m in one tick, the worst live zoom sample.
    EXPECT_FALSE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 5862.65), 0.030, why));
}

TEST (PlanCameraContinuityTest, ABogusZoomOutCannotJustifyItsOwnMovement)
{
    // ⚠️ THE REGRESSION THIS EXISTS FOR. If the budget were measured against the
    // CANDIDATE's half-height, a fit claiming a 5 km view would grant itself a
    // 5 km movement allowance -- so the most obviously wrong sample would be the
    // one that passes. The budget uses the SMALLER of the two.
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    EXPECT_FALSE (AcceptPlanCameraFit (state, GoodFit (3192.32, 0.0, 570.78), 0.046, why));
}

TEST (PlanCameraContinuityTest, APersistentJumpIsAdoptedSoTheGuardCannotLockUp)
{
    // Fit in Window, a storey change or a saved view really do teleport the
    // camera. A guard with no escape would hold a stale pose forever.
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));

    bool accepted = false;
    for (int tick = 0; tick < geomsrv::archviz::kMaxConsecutiveRejects + 1; ++tick)
        accepted = AcceptPlanCameraFit (state, GoodFit (4000.0, 4000.0, 900.0), kTick, why);

    EXPECT_TRUE (accepted);
    EXPECT_DOUBLE_EQ (state.centreX, 4000.0);
    EXPECT_DOUBLE_EQ (state.halfHeightMetres, 900.0);
    EXPECT_EQ (state.rejectedInARow, 0);
}

TEST (PlanCameraContinuityTest, AGoodFitAfterARejectResetsTheEscapeCounter)
{
    // Otherwise scattered single-tick glitches would accumulate across a whole
    // session and eventually wave a real one through.
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    for (int burst = 0; burst < 10; ++burst) {
        EXPECT_FALSE (AcceptPlanCameraFit (state, GoodFit (5000.0, 0.0, 7.18), kTick, why));
        EXPECT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.1 * burst, 0.0, 7.18), kTick, why));
        EXPECT_EQ (state.rejectedInARow, 0);
    }
}

TEST (PlanCameraContinuityTest, ABadClockCannotMakeTheGuardPermissive)
{
    // The budgets scale with elapsed time, so a huge or negative dt would wave
    // anything through. Both are clamped.
    PlanCameraContinuity state;
    std::string why;
    ASSERT_TRUE (AcceptPlanCameraFit (state, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    EXPECT_FALSE (AcceptPlanCameraFit (state, GoodFit (5585.58, 0.0, 7.18), 1.0e6, why));

    PlanCameraContinuity negative;
    ASSERT_TRUE (AcceptPlanCameraFit (negative, GoodFit (0.0, 0.0, 7.18), 0.0, why));
    EXPECT_FALSE (AcceptPlanCameraFit (negative, GoodFit (5585.58, 0.0, 7.18), -5.0, why));
}

// ---------------------------------------------------------------------------
// TEAR DETECTION (PLAT-RE82).
//
// The three corner samples are three separate ACAPI calls, so a view that
// scrolls between them is described by none of them. A fourth sample -- the
// first pixel, read again at the end -- measures how far it moved.
//
// ⚠️ THE FIRST ATTEMPT HERE WAS A CORRECTION, AND THESE TESTS KILLED IT. See
// `UniformTearDoesNotDisplaceTheCentre` below: for a uniform drift the fit's
// centre is ALREADY exact, so rewinding the samples to a common instant corrects
// nothing. The premise "the centre is displaced along the pan axis by the drift"
// is false, and the code now measures and rejects instead of modelling.

namespace {

using geomsrv::archviz::MeasurePlanSampleTear;
using geomsrv::archviz::PlanSampleTear;

// A window scrolling at a fixed rate per ACAPI call. The four calls happen at
// t = 0, 1, 2, 3 in call units; a sample of pixel p taken at call n reads the
// model point that pixel showed at that instant, which for a view scrolling by
// d per call is offset by n*d.
struct ScrollingWindow {
    double   originX = 0.0;
    double   originY = 0.0;
    double   metresPerPixel = 0.01;
    uint32_t widthPx = 2000;
    uint32_t heightPx = 1200;
    double   driftX = 0.0;
    double   driftY = 0.0;

    void Sample (double pixelX, double pixelY, int call, double out[2]) const
    {
        out[0] = originX + pixelX * metresPerPixel + driftX * double (call);
        out[1] = originY - pixelY * metresPerPixel + driftY * double (call);
    }

    // Where the centre really is at the sampling window's midpoint, call 1.5.
    double TrueCentreX () const
    {
        return originX + double (widthPx) * 0.5 * metresPerPixel + driftX * 1.5;
    }
    double TrueCentreY () const
    {
        return originY - double (heightPx) * 0.5 * metresPerPixel + driftY * 1.5;
    }
};

}   // namespace

TEST (PlanTearTest, UniformTearDoesNotDisplaceTheCentre)
{
    // ⚠️ THE FINDING THAT REDIRECTED THIS WHOLE FIX, kept as a test because it is
    // deeply counter-intuitive and the obvious "correction" follows from getting
    // it wrong. Work the algebra with drift d per call: the right axis picks up
    // d/W, the down axis picks up 2d/H, and the centre comes out at
    //     origin + W*mpp/2 + d/2 + d  =  origin + W*mpp/2 + 1.5d
    // which is exactly the true centre at the midpoint of the sampling window.
    // A uniform tear perturbs the AXES -- shear and a little scale -- and leaves
    // the centre alone. Anything that claims to fix a centre displacement caused
    // by uniform tearing is fixing something that is not happening.
    ScrollingWindow window;
    window.driftX = 2.0 * window.metresPerPixel;

    double tl[2], tr[2], bl[2];
    window.Sample (0, 0, 0, tl);
    window.Sample (window.widthPx, 0, 1, tr);
    window.Sample (0, window.heightPx, 2, bl);

    const PlanCameraFit fit = FitPlanCamera (tl, tr, bl, window.widthPx, window.heightPx);
    ASSERT_TRUE (fit.valid) << "a torn sample stays WELL FORMED -- that is the whole problem";
    EXPECT_NEAR (fit.centreX, window.TrueCentreX (), 1e-9);
    EXPECT_NEAR (fit.centreY, window.TrueCentreY (), 1e-9);
    // The damage is in the axes instead: out of square, by a hair.
    EXPECT_GT (fit.squareness, 0.0);
}

TEST (PlanTearTest, AStillViewIsNotReportedAsMoving)
{
    ScrollingWindow window;   // no drift
    double tl[2], tr[2], tl2[2];
    window.Sample (0, 0, 0, tl);
    window.Sample (window.widthPx, 0, 1, tr);
    window.Sample (0, 0, 3, tl2);

    const PlanSampleTear tear = MeasurePlanSampleTear (tl, tr, tl2, window.widthPx, 1.0);
    EXPECT_FALSE (tear.moving);
    EXPECT_TRUE (tear.usable);
    EXPECT_NEAR (tear.tearPixels, 0.0, 1e-9);
}

TEST (PlanTearTest, TheTearIsMeasuredInPixelsOfTheViewBeingSampled)
{
    // Three calls of 1.5 px each between the first read and the fourth.
    ScrollingWindow window;
    window.driftX = 1.5 * window.metresPerPixel;

    double tl[2], tr[2], tl2[2];
    window.Sample (0, 0, 0, tl);
    window.Sample (window.widthPx, 0, 1, tr);
    window.Sample (0, 0, 3, tl2);

    const PlanSampleTear tear = MeasurePlanSampleTear (tl, tr, tl2, window.widthPx, 1.0);
    EXPECT_TRUE (tear.moving);
    EXPECT_FALSE (tear.usable);          // 4.5 px, well past the one-pixel budget
    EXPECT_NEAR (tear.tearPixels, 4.5, 0.05);
}

TEST (PlanTearTest, TheMeasurementIsScaleInvariant)
{
    // ⚠️ IT MUST BE, or the threshold means something different at every zoom --
    // permissive zoomed out and paranoid zoomed in, which is backwards: a pixel
    // of error looks the same to the user at any zoom. Two views a thousand
    // times apart in scale, drifting the same number of PIXELS, must measure the
    // same tear.
    for (int power = -1; power <= 2; ++power) {
        ScrollingWindow window;
        window.metresPerPixel = 0.01 * std::pow (10.0, double (power));
        window.driftX = 0.5 * window.metresPerPixel;

        double tl[2], tr[2], tl2[2];
        window.Sample (0, 0, 0, tl);
        window.Sample (window.widthPx, 0, 1, tr);
        window.Sample (0, 0, 3, tl2);

        const PlanSampleTear tear = MeasurePlanSampleTear (tl, tr, tl2, window.widthPx, 1.0);
        EXPECT_NEAR (tear.tearPixels, 1.5, 0.05) << "at metresPerPixel " << window.metresPerPixel;
    }
}

TEST (PlanTearTest, MotionIsDetectedRegardlessOfDirection)
{
    // `hideonnav` blanks on this flag, and a detector that only saw one pan
    // direction would blank on half of them -- which reads as a random failure.
    const double signs[2] = {1.0, -1.0};
    for (int index = 0; index < 2; ++index) {
        ScrollingWindow window;
        window.driftX = signs[index] * 0.4 * window.metresPerPixel;
        window.driftY = signs[index] * -0.3 * window.metresPerPixel;

        double tl[2], tr[2], tl2[2];
        window.Sample (0, 0, 0, tl);
        window.Sample (window.widthPx, 0, 1, tr);
        window.Sample (0, 0, 3, tl2);

        const PlanSampleTear tear = MeasurePlanSampleTear (tl, tr, tl2, window.widthPx, 1.0);
        EXPECT_TRUE (tear.moving) << "sign " << signs[index];
    }
}

TEST (PlanTearTest, MovingIsReportedEvenWhenTheSampleIsUnusable)
{
    // The frames `hideonnav` most needs the flag on are exactly the ones too torn
    // to draw, so the two answers must be independent.
    ScrollingWindow window;
    window.driftX = 100.0 * window.metresPerPixel;

    double tl[2], tr[2], tl2[2];
    window.Sample (0, 0, 0, tl);
    window.Sample (window.widthPx, 0, 1, tr);
    window.Sample (0, 0, 3, tl2);

    const PlanSampleTear tear = MeasurePlanSampleTear (tl, tr, tl2, window.widthPx, 1.0);
    EXPECT_TRUE (tear.moving);
    EXPECT_FALSE (tear.usable);
}

TEST (PlanTearTest, ADegenerateWindowIsNotReportedAsMovingOrUsable)
{
    // A zero right-axis is what a sample against a window with no live database
    // produces. Dividing by it would yield an infinite tear; reporting `moving`
    // off the back of it would blank the overlay for a reason that is not motion.
    double tl[2] = {5.0, 5.0};
    double tr[2] = {5.0, 5.0};
    double tl2[2] = {5.0, 5.0};
    const PlanSampleTear tear = MeasurePlanSampleTear (tl, tr, tl2, 2000, 1.0);
    EXPECT_FALSE (tear.moving);
    EXPECT_FALSE (tear.usable);
}

// ---------------------------------------------------------------------------
// PREDICTION (PLAT-RE76).
//
// ⚠️ WHAT THESE HAVE TO PROVE IS NOT "the arithmetic runs". Prediction is the
// only mechanism on this path that can put a CONTINUOUS overlay in register, and
// the way it fails is by OVERSHOOTING -- which on screen is a bounce past the
// model and back, worse than the lag it replaces. So the interesting tests are
// the discontinuities: a dead stop and a direction reversal.

namespace {

using geomsrv::archviz::PlanCameraPredictor;
using geomsrv::archviz::PredictPlanCamera;

constexpr double kTickSeconds = 0.016;

// The .cpp's own wrap helper is file-local, so the test carries its own copy
// rather than widening the interface for a test's convenience.
double ShortestAngleDelta (double from, double to)
{
    constexpr double kPi = 3.14159265358979323846;
    double delta = to - from;
    while (delta > kPi)
        delta -= 2.0 * kPi;
    while (delta < -kPi)
        delta += 2.0 * kPi;
    return delta;
}

PlanCameraFit Pose (double centreX, double centreY, double halfHeight, double rotation = 0.0)
{
    PlanCameraFit fit;
    fit.valid = true;
    fit.centreX = centreX;
    fit.centreY = centreY;
    fit.halfHeightMetres = halfHeight;
    fit.rotationRadians = rotation;
    fit.metresPerPixelY = halfHeight * 2.0 / 1000.0;
    fit.metresPerPixelX = fit.metresPerPixelY;
    return fit;
}

}   // namespace

TEST (PlanPredictTest, TheFirstObservationIsPassedThroughUnchanged)
{
    // A velocity invented from one sample is a guess, and a guess here moves the
    // picture on the very first frame the overlay is up.
    PlanCameraPredictor state;
    const PlanCameraFit observed = Pose (10.0, 20.0, 7.0);
    const PlanCameraFit out = PredictPlanCamera (state, observed, 0.0, kTickSeconds);
    EXPECT_DOUBLE_EQ (out.centreX, observed.centreX);
    EXPECT_DOUBLE_EQ (out.centreY, observed.centreY);
    EXPECT_DOUBLE_EQ (out.halfHeightMetres, observed.halfHeightMetres);
}

TEST (PlanPredictTest, AnInvalidObservationIsNotPredictedFrom)
{
    PlanCameraPredictor state;
    PlanCameraFit refused;              // valid == false
    refused.centreX = 999.0;
    const PlanCameraFit out = PredictPlanCamera (state, refused, kTickSeconds, kTickSeconds);
    EXPECT_FALSE (out.valid);
    EXPECT_FALSE (state.have) << "a refused fit must not seed the predictor";
}

TEST (PlanPredictTest, ASteadyPanIsPredictedOntoTheNextObservation)
{
    // ⚠️ THE POINT OF THE WHOLE FEATURE, as a test. At constant velocity the
    // prediction for one tick ahead should land on where the view ACTUALLY is one
    // tick later -- that is what closes the structural one-frame gap.
    PlanCameraPredictor state;
    const double perTick = 0.5;         // metres per tick
    double x = 0.0;

    PredictPlanCamera (state, Pose (x, 0.0, 7.0), 0.0, kTickSeconds);
    PlanCameraFit predicted;
    for (int tick = 1; tick <= 10; ++tick) {
        x += perTick;
        predicted = PredictPlanCamera (state, Pose (x, 0.0, 7.0), kTickSeconds, kTickSeconds);
    }
    // The next real observation would be at x + perTick.
    EXPECT_NEAR (predicted.centreX, x + perTick, perTick * 0.15);
}

TEST (PlanPredictTest, ADeadStopDoesNotBounce)
{
    // ⚠️ THE FAILURE MODE THIS FEATURE IS JUDGED ON. When the view stops, the
    // observed step collapses to zero and so must the allowance -- otherwise the
    // blended velocity keeps pushing the overlay past the model for several ticks,
    // which is `bounces past and returns` in the matrix's vocabulary.
    PlanCameraPredictor state;
    const double perTick = 1.0;
    double x = 0.0;
    PredictPlanCamera (state, Pose (x, 0.0, 7.0), 0.0, kTickSeconds);
    for (int tick = 0; tick < 10; ++tick) {
        x += perTick;
        PredictPlanCamera (state, Pose (x, 0.0, 7.0), kTickSeconds, kTickSeconds);
    }

    // Now it stops dead and stays stopped.
    const double stoppedAt = x;
    for (int tick = 0; tick < 5; ++tick) {
        const PlanCameraFit out =
            PredictPlanCamera (state, Pose (stoppedAt, 0.0, 7.0), kTickSeconds, kTickSeconds);
        EXPECT_DOUBLE_EQ (out.centreX, stoppedAt)
            << "tick " << tick << " after the stop: the allowance is a multiple of the "
               "observed step, and the observed step is zero";
    }
}

TEST (PlanPredictTest, ADirectionReversalDoesNotSwingTheWrongWay)
{
    // Blended velocity means the estimate lags a reversal, so the prediction can
    // briefly point the old way. The clamp must keep that within a fraction of
    // one step rather than a visible swing.
    PlanCameraPredictor state;
    const double perTick = 1.0;
    double x = 0.0;
    PredictPlanCamera (state, Pose (x, 0.0, 7.0), 0.0, kTickSeconds);
    for (int tick = 0; tick < 10; ++tick) {
        x += perTick;
        PredictPlanCamera (state, Pose (x, 0.0, 7.0), kTickSeconds, kTickSeconds);
    }

    x -= perTick;   // reverse
    const PlanCameraFit out =
        PredictPlanCamera (state, Pose (x, 0.0, 7.0), kTickSeconds, kTickSeconds);
    EXPECT_LE (std::fabs (out.centreX - x), perTick * geomsrv::archviz::kMaxPredictedStepsAhead)
        << "the prediction may lag a reversal, but never by more than the clamp";
}

TEST (PlanPredictTest, ZoomIsExtrapolatedGeometricallyAndStaysPositive)
{
    // ⚠️ A LINEAR EXTRAPOLATION OF HALF-HEIGHT CAN PREDICT A NEGATIVE ONE, which
    // is a camera with an inside-out frustum. Zoom multiplies; so does this.
    PlanCameraPredictor state;
    double half = 100.0;
    const double ratioPerTick = 0.5;    // a very hard zoom in
    PredictPlanCamera (state, Pose (0.0, 0.0, half), 0.0, kTickSeconds);
    for (int tick = 0; tick < 12; ++tick) {
        half *= ratioPerTick;
        const PlanCameraFit out =
            PredictPlanCamera (state, Pose (0.0, 0.0, half), kTickSeconds, kTickSeconds);
        EXPECT_GT (out.halfHeightMetres, 0.0) << "tick " << tick;
        // Predicting one tick ahead of a geometric ramp means roughly one more
        // multiplication, never a subtraction past zero.
        EXPECT_LT (out.halfHeightMetres, half * 1.05);
        EXPECT_GT (out.halfHeightMetres, half * ratioPerTick * 0.9);
    }
}

TEST (PlanPredictTest, RotationTakesTheShortWayRoundPi)
{
    // ⚠️ WITHOUT THE WRAP, a rotation crossing pi extrapolates the LONG way: a
    // hundredth of a turn reads as three-hundred-odd degrees per second of
    // angular velocity, and the overlay spins.
    constexpr double kPi = 3.14159265358979323846;
    PlanCameraPredictor state;
    const double step = 0.01;
    double angle = kPi - 2.0 * step;

    PredictPlanCamera (state, Pose (0.0, 0.0, 7.0, angle), 0.0, kTickSeconds);
    for (int tick = 0; tick < 6; ++tick) {
        angle += step;
        if (angle > kPi)
            angle -= 2.0 * kPi;         // as an angle normaliser would report it
        const PlanCameraFit out =
            PredictPlanCamera (state, Pose (0.0, 0.0, 7.0, angle), kTickSeconds, kTickSeconds);
        const double drift = std::fabs (ShortestAngleDelta (angle, out.rotationRadians));
        EXPECT_LT (drift, step * 3.0) << "tick " << tick << ": prediction ran away at the wrap";
    }
}

TEST (PlanPredictTest, AZeroHorizonReturnsTheObservationExactly)
{
    // The mode switch can set the horizon to zero to compare prediction against
    // plain following without rebuilding, and that comparison is only fair if the
    // zero case is bit-identical to not predicting at all.
    PlanCameraPredictor state;
    PredictPlanCamera (state, Pose (0.0, 0.0, 7.0), 0.0, 0.0);
    const PlanCameraFit observed = Pose (5.0, 0.0, 7.0);
    const PlanCameraFit out = PredictPlanCamera (state, observed, kTickSeconds, 0.0);
    EXPECT_DOUBLE_EQ (out.centreX, observed.centreX);
    EXPECT_DOUBLE_EQ (out.halfHeightMetres, observed.halfHeightMetres);
}

// ---------------------------------------------------------------------------
// PLAT-RE98: the allowance follows how steady the motion has been.
//
// One fixed 1.5-step clamp had to serve a steady pan and a fast reversal at
// once, and the 2026-08-13 sweep showed it failing both: raising the prediction
// horizon past 1.5x changed nothing (lag plateaued at 14-16 ms across scales
// 1.5, 2.0 and 2.5 because the CLAMP, not the horizon, was binding), while the
// overshoot it existed to prevent showed up anyway on fast direction changes.

namespace {

// Feed `count` identical steps of `dx` per `dt`, returning the last prediction.
geomsrv::archviz::PlanCameraFit RunSteadyPan (geomsrv::archviz::PlanCameraPredictor& state,
                                              int count, double dx, double dt, double horizon)
{
    geomsrv::archviz::PlanCameraFit observed;
    observed.valid = true;
    observed.halfHeightMetres = 10.0;
    geomsrv::archviz::PlanCameraFit predicted = observed;
    for (int i = 0; i < count; ++i) {
        observed.centreX = dx * double (i);
        predicted = geomsrv::archviz::PredictPlanCamera (state, observed, dt, horizon);
    }
    return predicted;
}

}   // namespace

TEST (PlanPredictAdaptiveTest, SteadyMotionEarnsMoreThanTheOldFixedCap)
{
    // THE POINT OF THE CHANGE. Under the old fixed 1.5 the prediction could
    // never exceed 1.5 steps however far the horizon reached, which is exactly
    // why the measured lag stopped falling. After a run of consistent steps a
    // steady pan must be allowed further than that.
    geomsrv::archviz::PlanCameraPredictor state;
    const double dx = 0.10;
    const double dt = 0.016;
    // A horizon of 4 steps asks for far more than any clamp will grant; what is
    // being measured is how much the clamp now grants.
    const auto predicted = RunSteadyPan (state, 12, dx, dt, dt * 4.0);
    const double lastObserved = dx * 11.0;
    const double stepsAhead = (predicted.centreX - lastObserved) / dx;
    EXPECT_GT (stepsAhead, 1.6) << "steady motion is still capped near the old 1.5";
    EXPECT_LE (stepsAhead, geomsrv::archviz::kMaxPredictedStepsAhead + 1e-9);
}

TEST (PlanPredictAdaptiveTest, AReversalCollapsesTheAllowanceInOneSample)
{
    // The other half, and the one that must not regress: the user reported real
    // overshoot at horizon scale 2.5. However much trust a steady pan has built
    // up, the first step in a new direction must spend almost none of it.
    geomsrv::archviz::PlanCameraPredictor state;
    const double dx = 0.10;
    const double dt = 0.016;
    RunSteadyPan (state, 12, dx, dt, dt * 4.0);

    geomsrv::archviz::PlanCameraFit observed;
    observed.valid = true;
    observed.halfHeightMetres = 10.0;
    // Now go the other way by one step.
    const double reversed = dx * 11.0 - dx;
    observed.centreX = reversed;
    const auto predicted =
        geomsrv::archviz::PredictPlanCamera (state, observed, dt, dt * 4.0);

    const double stepsAhead = std::fabs (predicted.centreX - reversed) / dx;
    EXPECT_LE (stepsAhead, geomsrv::archviz::kMinPredictedStepsAhead + 1e-9)
        << "a reversal still spends the trust a steady pan earned";
}

TEST (PlanPredictAdaptiveTest, APauseDoesNotDestroyEarnedTrust)
{
    // ⚠️ A HESITATION IS NOT A REVERSAL. A zero-length step has no direction, so
    // punishing it would restart the ramp every time the user's hand paused
    // mid-drag -- and a drag is full of those.
    geomsrv::archviz::PlanCameraPredictor state;
    const double dx = 0.10;
    const double dt = 0.016;
    RunSteadyPan (state, 12, dx, dt, dt * 4.0);
    const double earned = state.centreConsistency;

    geomsrv::archviz::PlanCameraFit observed;
    observed.valid = true;
    observed.halfHeightMetres = 10.0;
    observed.centreX = dx * 11.0;   // no movement at all
    geomsrv::archviz::PredictPlanCamera (state, observed, dt, dt * 4.0);

    EXPECT_NEAR (state.centreConsistency, earned, 1e-9);
}

TEST (PlanPredictAdaptiveTest, AZoomPredictionIsBoundedHoweverFastTheWheelSpins)
{
    // ⚠️ THE 12,183-PIXEL FRAME. Zoom extrapolates in log space, so a big step
    // compounds; the sweep produced single-frame errors that put the overlay
    // off the screen entirely. No one frame may change the zoom by more than
    // half, whatever the measured rate claims.
    geomsrv::archviz::PlanCameraPredictor state;
    geomsrv::archviz::PlanCameraFit observed;
    observed.valid = true;
    observed.centreX = 0.0;
    observed.halfHeightMetres = 10.0;

    const double dt = 0.016;
    // A violent, CONSISTENT zoom -- consistent so the allowance is at its most
    // generous, which is the worst case for this bound.
    double half = 10.0;
    geomsrv::archviz::PlanCameraFit predicted = observed;
    for (int i = 0; i < 12; ++i) {
        half *= 0.55;   // nearly halving every sample
        observed.halfHeightMetres = half;
        predicted = geomsrv::archviz::PredictPlanCamera (state, observed, dt, dt * 4.0);
    }
    const double ratio = predicted.halfHeightMetres / half;
    EXPECT_GT (ratio, 1.0 / 1.51);
    EXPECT_LT (ratio, 1.51) << "one frame moved the zoom by more than half";
    EXPECT_GT (predicted.halfHeightMetres, 0.0);
}

TEST (PlanPredictAdaptiveTest, APerpendicularTurnIsTreatedAsADirectionChange)
{
    // A 90-degree turn is not a reversal, but it is not continuation either --
    // the old per-axis sign test would have called it "consistent in Y" and
    // predicted straight through the corner.
    geomsrv::archviz::PlanCameraPredictor state;
    const double step = 0.10;
    const double dt = 0.016;
    RunSteadyPan (state, 12, step, dt, dt * 4.0);

    geomsrv::archviz::PlanCameraFit observed;
    observed.valid = true;
    observed.halfHeightMetres = 10.0;
    observed.centreX = step * 11.0;
    observed.centreY = step;          // turn hard left
    geomsrv::archviz::PredictPlanCamera (state, observed, dt, dt * 4.0);

    EXPECT_NEAR (state.centreConsistency, 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Blit-time reprojection (PLAT-RE114).
//
// ⚠️ THESE TESTS ARE THE WHOLE VERIFICATION OF THE WARP, and they are written as
// a ROUND TRIP rather than as expected constants. A hand-computed constant only
// proves the implementation matches whatever the test author believed about the
// sign conventions -- which is exactly the thing that has gone wrong repeatedly
// in this work (the arc sign, the axonometric handedness, `viewCone` being
// horizontal). Projecting a model point through both cameras and asking whether
// the warp maps one image position to the other cannot be satisfied by a warp
// that is self-consistently backwards.
namespace {

// The renderer's own convention, from Camera::GetViewMatrix: top-down with
// `up = (-sin r, cos r, 0)`, so view space is R(-r) * (m - c), divided by the
// half extents to reach NDC. Returned in units of the camera's HALF-HEIGHT, x
// first -- the same space the warp works in.
void ProjectPlan (double mx, double my, double cx, double cy, double half, double rotation,
                  double& outX, double& outY)
{
    const double dx = mx - cx;
    const double dy = my - cy;
    outX = (dx * std::cos (rotation) + dy * std::sin (rotation)) / half;
    outY = (-dx * std::sin (rotation) + dy * std::cos (rotation)) / half;
}

// Apply the warp exactly as the pixel shader does.
void ApplyWarp (const geomsrv::archviz::PlanReprojection& warp, double x1, double y1,
                double& outX, double& outY)
{
    outX = warp.offsetX + warp.scale * (warp.cosDelta * x1 - warp.sinDelta * y1);
    outY = warp.offsetY + warp.scale * (warp.sinDelta * x1 + warp.cosDelta * y1);
}

// One round trip: a model point, projected through both cameras, must be
// carried from the CURRENT image position to the RENDERED one by the warp.
void ExpectWarpCarriesPoint (double mx, double my,
                             double cx0, double cy0, double h0, double r0,
                             double cx1, double cy1, double h1, double r1)
{
    const geomsrv::archviz::PlanReprojection warp =
        geomsrv::archviz::ComputePlanReprojection (cx0, cy0, h0, r0, cx1, cy1, h1, r1);
    ASSERT_TRUE (warp.valid);

    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    ProjectPlan (mx, my, cx0, cy0, h0, r0, x0, y0);
    ProjectPlan (mx, my, cx1, cy1, h1, r1, x1, y1);

    double warpedX = 0.0, warpedY = 0.0;
    ApplyWarp (warp, x1, y1, warpedX, warpedY);
    // ⚠️ THE TOLERANCE IS FLOAT32'S, NOT A FUDGE FACTOR. The warp is STORED as
    // five floats because that is what reaches the shader, so the round trip
    // carries ~1e-7 of relative error however exact the algebra is -- measured
    // 6e-9 on a pan of 1.5 m. At the edge of a 16:9 frame that is a millionth of
    // a half-height, or well under a thousandth of a pixel. A tighter bound here
    // would only ever fail for a reason that cannot be seen.
    EXPECT_NEAR (warpedX, x0, 2e-6);
    EXPECT_NEAR (warpedY, y0, 2e-6);
}

}   // namespace

TEST (PlanReprojectionTest, IdenticalCamerasWarpToIdentity)
{
    const geomsrv::archviz::PlanReprojection warp =
        geomsrv::archviz::ComputePlanReprojection (12.0, -3.0, 8.0, 0.4, 12.0, -3.0, 8.0, 0.4);
    ASSERT_TRUE (warp.valid);
    EXPECT_NEAR (warp.scale, 1.0, 1e-9);
    EXPECT_NEAR (warp.cosDelta, 1.0, 1e-9);
    EXPECT_NEAR (warp.sinDelta, 0.0, 1e-9);
    EXPECT_NEAR (warp.offsetX, 0.0, 1e-9);
    EXPECT_NEAR (warp.offsetY, 0.0, 1e-9);
}

TEST (PlanReprojectionTest, APurePanIsCarriedExactly)
{
    ExpectWarpCarriesPoint (5.0, 7.0, /*rendered*/ 0.0, 0.0, 10.0, 0.0,
                                      /*current*/  1.5, -2.25, 10.0, 0.0);
}

TEST (PlanReprojectionTest, APureZoomIsCarriedExactly)
{
    ExpectWarpCarriesPoint (-4.0, 9.0, 2.0, 2.0, 10.0, 0.0,
                                       2.0, 2.0, 6.5, 0.0);
}

TEST (PlanReprojectionTest, APureRotationIsCarriedExactly)
{
    ExpectWarpCarriesPoint (3.0, -8.0, 1.0, 1.0, 12.0, 0.0,
                                       1.0, 1.0, 12.0, 0.7);
}

TEST (PlanReprojectionTest, PanZoomAndRotationTogetherAreCarriedExactly)
{
    // ⚠️ THE COMBINED CASE IS NOT IMPLIED BY THE THREE SEPARATE ONES. The offset
    // is expressed in the RENDERED camera's rotated frame and scaled by ITS
    // half-height; applying the rotation to the wrong one of the two cameras
    // passes every single-axis test above and fails only when both move.
    ExpectWarpCarriesPoint (-11.0, 4.0, 2.0, -1.0, 14.0, -0.35,
                                        5.5, 3.25, 9.0, 0.42);
}

TEST (PlanReprojectionTest, ManyPointsUnderOneWarp)
{
    // The warp is one affine map for the WHOLE image, so it has to carry every
    // point, not just the centre -- a scale error hides completely at the middle
    // of the frame and shows only at the edges.
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            ExpectWarpCarriesPoint (double (i) * 6.0, double (j) * 6.0,
                                    1.0, -2.0, 11.0, 0.21,
                                    -3.0, 4.0, 7.5, -0.13);
        }
    }
}

TEST (PlanReprojectionTest, AZeroHalfHeightIsRefusedRatherThanDividedBy)
{
    EXPECT_FALSE (geomsrv::archviz::ComputePlanReprojection (0, 0, 0.0, 0.0,
                                                             0, 0, 10.0, 0.0).valid);
    EXPECT_FALSE (geomsrv::archviz::ComputePlanReprojection (0, 0, 10.0, 0.0,
                                                             0, 0, 0.0, 0.0).valid);
}
