#ifndef EVP_ARCHVIZ_PLANCAMERAMATH_HPP
#define EVP_ARCHVIZ_PLANCAMERAMATH_HPP

// ArchViz/PlanCameraMath — turning a 2D drawing window's pixel-to-model mapping
// into a top-down orthographic camera.
//
// WHY THIS EXISTS AT ALL. The overlay's camera has always come from
// `ACAPI_View_Get3DProjectionSets`, which describes the 3D WINDOW. Put the floor
// plan in front and that call still answers -- with the 3D window's camera -- so
// the overlay draws a perspective view of the model over a plan drawing and
// nothing about the result says why. A 2D window has no camera to read: what it
// has is a mapping from model coordinates to pixels, and this file is the
// conversion from one to the other.
//
// ⚠️ THE MAPPING IS MEASURED, NOT ASSUMED, AND THAT IS THE WHOLE DESIGN.
// `ACAPI_View_GetZoom` hands back a model-space box, but how Archicad FITS that
// box into a window whose aspect ratio differs from the box's is not documented
// anywhere this repo has been able to grep -- and a fit rule guessed wrong is a
// scale error that looks exactly like a wrong zoom. Sampling
// `ACAPI_View_PointToCoord` at three window corners asks Archicad for the answer
// instead: three points determine the affine map completely, including the
// scale, the origin AND any view rotation, with nothing left to assume. The zoom
// box is still read alongside, as a cross-check that names a disagreement rather
// than resolving it silently.
//
// ⚠️ NO ACAPI AND NO DILIGENT IN HERE. The sampling is ACAPI (main thread only)
// and lives in PlanViewCamera.cpp; this half is arithmetic, so it is tested
// offline in tests/cpp/test_plancameramath.cpp against mappings built by hand --
// including the mirrored and skewed ones that must be REFUSED rather than
// rendered.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {

struct PlanCameraFit {
    // False means: do not point a camera at this. `why` says what was wrong, and
    // it is written for a log a human reads, not for a caller to switch on.
    bool valid = false;
    std::string why;

    // The model point under the CENTRE of the window.
    double centreX = 0.0;
    double centreY = 0.0;
    // Model metres per window pixel, vertically. The orthographic camera's
    // height comes from this one; `metresPerPixelX` is reported separately so a
    // non-uniform mapping is visible rather than averaged away.
    double metresPerPixelY = 0.0;
    double metresPerPixelX = 0.0;
    // Half the window's height in model metres -- the orthographic half-extent.
    double halfHeightMetres = 0.0;
    // The window's +X axis as an angle in model space, CCW from model +X. Zero
    // means an unrotated plan: model north (+Y) is up on screen.
    double rotationRadians = 0.0;

    // ---- the two ways the mapping can be something a camera cannot express --
    // How far the two window axes are from perpendicular: |dot| of their unit
    // vectors, so 0 is square. A shear cannot be expressed as a camera pose.
    double squareness = 0.0;
    // metresPerPixelX / metresPerPixelY. 1 is uniform; anything else is an
    // anisotropic scale, which a single orthographic frustum also cannot be.
    double scaleRatio = 1.0;
    // True when the window's +Y (down the screen) is on the WRONG side of its
    // +X -- a mirrored view. Refused: rendering it would need a negative
    // determinant, which is the exact bug class Camera.hpp's handedness note is
    // about.
    bool mirrored = false;
};

// The three samples are the model coordinates Archicad reports for three window
// pixels: (0,0), (widthPx,0) and (0,heightPx). Each is {x, y} in model metres.
//
// ⚠️ THE PIXELS MUST BE THE OVERLAY'S OWN, in the same space its surface is
// sized in. The whole point of the affine fit is that the caller never has to
// know Archicad's zoom or the display scaling -- but it does have to sample the
// SAME rectangle it is going to draw into, or it has measured a different
// window's mapping perfectly.
PlanCameraFit FitPlanCamera (const double topLeft[2], const double topRight[2],
                             const double bottomLeft[2], uint32_t widthPx, uint32_t heightPx);

// ---------------------------------------------------------------------------
// DE-TEARING a moving sample (PLAT-RE82).
//
// ⚠️ THE THREE CORNERS ARE THREE SEPARATE ACAPI CALLS, so a view that scrolls
// between them is described by none of them. The resulting fit is still square,
// still uniform, still unmirrored -- and displaced ALONG THE PAN AXIS, sign
// following the pan direction. That is the user's report: "not exactly random,
// they start top left or bottom right based on pan direction".
//
// ⚠️ MEASURED AND REJECTED, NOT MODELLED AND CORRECTED, and the first attempt
// here got that wrong in a way worth recording.
//
// The obvious fix is to rewind each sample to a common instant using the drift
// between the two readings of the same pixel. It is a no-op for the case it was
// written for, and the offline test is what said so. For a UNIFORM drift `d` per
// call, the fit's centre works out to
//
//     origin + W*mpp/2 + 1.5*d
//
// which is exactly the true centre at the sampling window's midpoint. A uniform
// tear does not displace the centre at all -- it perturbs the AXES, as a shear
// and a small scale error. So a linear rewind corrects nothing that was wrong and
// the premise "the centre is displaced along the pan axis by the drift" is false.
//
// What CAN displace the centre is an UNEVEN tear: Archicad scrolling once,
// between two particular calls, rather than continuously. That displaces the
// centre by half a scroll step, with the sign following the pan -- which does
// match the reported "top left or bottom right based on pan direction" -- but the
// step size is not observable from here, so there is nothing to rewind BY.
//
// Hence: this measures, and the caller drops anything above `maxTearPixels`. A
// dropped tick holds the previous pose, which is the path an unreadable window
// already took. During a fast pan that means the overlay stops rather than
// jumping, and `hideonnav` blanks it outright -- both preferable to confidently
// drawing a view that never existed.
struct PlanSampleTear {
    // How far the repeated corner moved, in pixels of the view being sampled.
    double tearPixels = 0.0;
    // True once the tear is big enough to be motion rather than arithmetic noise.
    // The caller reports this as `viewMoving`: POSITIVE EVIDENCE that the user is
    // navigating, available on the SAME tick rather than the next one, which is
    // what lets `hideonnav` blank on the first moving frame.
    bool moving = false;
    // False when the tear is large enough that the fit cannot be trusted.
    bool usable = false;
};

// `topLeftAgain` is the fourth sample: pixel (0,0) read after the other three.
// Nothing is modified -- this only measures.
PlanSampleTear MeasurePlanSampleTear (const double topLeft[2], const double topRight[2],
                                      const double topLeftAgain[2], uint32_t widthPx,
                                      double maxTearPixels);

// ---------------------------------------------------------------------------
// The CONTINUITY guard (PLAT-RE82).
//
// ⚠️ A FIT CAN BE PERFECTLY WELL FORMED AND STILL BE GARBAGE, and that is what
// the 2026-08-13 runs caught. `FitPlanCamera` refuses the three mappings a camera
// cannot express -- shear, anisotropy, mirroring -- but a torn or bogus
// `PointToCoord` read produces a mapping that is square, uniform, unmirrored and
// completely wrong. Measured, from 9,887 live plan samples: 0.87% of ticks moved
// the view centre by more than a whole half-height, the worst by 778 half-heights
// (5.6 km, from a 7 m view), and the half-height itself jumped 7.18 m -> 5862 m
// inside one 30 ms tick. On screen that is the user's report: "when panning fast
// the overlay first jumps to a random location away from the model and then
// starts to overlay geometry".
//
// No plan view does that. A camera can only be trusted to the extent it is
// CONTINUOUS with the one before it, and the physical bound is generous: the
// limits below allow a pan of many screen-widths per second and a zoom far faster
// than a wheel produces, while rejecting every outlier in that sample.
//
// ⚠️ REJECTION IS NOT A FAILURE PATH. A rejected fit becomes an invalid
// CameraStart, `SyncCamera` drops it, and the overlay holds its previous pose for
// one tick -- the behaviour that already existed for an unreadable window.
//
// ⚠️ IT MUST BE ABLE TO GIVE UP, and that is what `rejectedInARow` is for. A
// legitimate discontinuity does exist -- Fit in Window, a storey change, a jump
// to a saved view -- and a guard with no escape would lock the overlay onto a
// stale camera forever. After kMaxConsecutiveRejects the next fit is accepted
// whatever it says: a discontinuity that PERSISTS is the new truth, while the
// garbage this catches is one tick wide.
struct PlanCameraContinuity {
    // The last ACCEPTED fit, and how long ago. `have` is false before the first.
    bool   have = false;
    double centreX = 0.0;
    double centreY = 0.0;
    double halfHeightMetres = 0.0;
    int    rejectedInARow = 0;
};

// How much a plan view may legitimately change, per second of elapsed time.
// Deliberately loose -- this exists to catch kilometre-scale nonsense, not to
// smooth anything. Smoothing is PLAT-RE76's job and belongs nowhere near a
// validity test.
constexpr double kMaxHalfHeightsPerSecond = 100.0;   // panning speed
constexpr double kMaxZoomFactorPerSecond  = 60.0;    // zoom speed, as a ratio
constexpr int    kMaxConsecutiveRejects   = 4;

// True if `candidate` may be adopted. Updates `state` either way: on acceptance
// it becomes the new reference, on rejection it counts toward the escape.
// `elapsedSeconds` is the time since the last accepted fit; pass 0 or less and
// the guard assumes one tick so a bad clock cannot make it permissive.
//
// `why` is filled ONLY on rejection, for the log.
bool AcceptPlanCameraFit (PlanCameraContinuity& state, const PlanCameraFit& candidate,
                          double elapsedSeconds, std::string& why);

// ---------------------------------------------------------------------------
// PREDICTION (PLAT-RE76) -- the one mechanism on this path that can put a
// CONTINUOUS overlay genuinely in register.
//
// WHY FOLLOWING CANNOT, AND PREDICTING CAN. With two swap chains we only learn
// Archicad's camera after Archicad has already used it, so a follower is behind
// by construction and a faster poll only shrinks the gap. Predicting where the
// view will be at OUR next present puts us alongside it instead of behind it.
//
// ⚠️ IT WORKS HERE BECAUSE THE PLAN CAMERA HAS FOUR DEGREES OF FREEDOM AND
// ARCHICAD ANIMATES IT. Centre x/y, half-height and rotation, and nothing else.
// The 2026-08-13 logs show `PointToCoord` returning the intermediate frames of
// Archicad's own zoom animation -- a smooth, machine-generated ramp -- while
// `GetZoom` still reported the old settled box. Motion that smooth is close to
// exactly predictable. A perspective orbit is NOT (PLAT-RE80 is a separate
// question with a separate answer), so nothing here generalises to the 3D path.
//
// ⚠️ ZOOM IS EXTRAPOLATED GEOMETRICALLY, NOT LINEARLY. A zoom is a RATIO per
// unit time -- wheel notches multiply. Extrapolating half-height linearly
// overshoots badly zooming in and undershoots zooming out, and worse, a long
// enough linear step can predict a NEGATIVE half-height, which is a camera with
// an inside-out frustum.
struct PlanCameraPredictor {
    bool   have = false;
    double centreX = 0.0;
    double centreY = 0.0;
    double halfHeightMetres = 0.0;
    double rotationRadians = 0.0;

    // Rates, per second. Zoom is a LOG rate for the reason above.
    double centreVelocityX = 0.0;
    double centreVelocityY = 0.0;
    double logHalfHeightRate = 0.0;
    double rotationRate = 0.0;

    // The previous step, kept so the CURRENT one can be compared against it.
    // Consistency is the whole basis of the adaptive allowance below.
    double previousStepX = 0.0;
    double previousStepY = 0.0;
    double previousStepRotation = 0.0;
    double previousStepLogHalfHeight = 0.0;

    // 0 = the motion just changed its mind, 1 = it has been steady for a while.
    // One per channel, because a user zooms while panning and a reversal in one
    // says nothing about the other.
    double centreConsistency = 0.0;
    double rotationConsistency = 0.0;
    double zoomConsistency = 0.0;
};

// How far past the observation the prediction may run, as a multiple of the step
// just observed. ⚠️ THIS IS THE OVERSHOOT BOUND, and it is what makes an abrupt
// stop cost a fraction of a step instead of a visible bounce -- the failure mode
// `bounces past and returns` names in the matrix.
//
// ⚠️ IT IS A RANGE, NOT A CONSTANT, AND THE MEASUREMENTS FORCED THAT. A single
// 1.5 had to serve two opposite cases at once, and the 2026-08-13 sweep showed
// it doing neither well: raising the prediction horizon past 1.5x did nothing at
// all (lag plateaued at 14-16 ms across scales 1.5, 2.0 and 2.5, because the
// clamp -- not the horizon -- was the binding constraint), while the overshoot
// it was protecting against appeared anyway on fast direction changes.
//
// Steady motion is exactly predictable and can safely be extrapolated much
// further; a reversal cannot be extrapolated at all. So the allowance follows
// how consistent the motion has actually been, and the asymmetry is deliberate:
// it ramps up over several samples and collapses in ONE. Overshoot is far more
// visible than lag, so the cost of being slow to trust is small and the cost of
// being slow to distrust is the bounce.
constexpr double kMinPredictedStepsAhead = 0.5;   // just reversed: barely predict
constexpr double kMaxPredictedStepsAhead = 3.0;   // steady: the lag actually falls

// How fast trust is earned. One sample of agreement moves consistency this far
// toward 1, so it takes ~5 steady samples (~80 ms) to reach full extrapolation.
constexpr double kConsistencyGain = 0.35;

// ⚠️ THE SCALAR CHANNELS EARN TRUST FASTER, AND THE REASON IS STRUCTURAL RATHER
// THAN A TASTE FOR SHARPER ZOOM. `UpdateConsistency` blends toward the COSINE of
// the turn. For the centre that cosine is a real number over [0,1] -- a gentle
// curve is genuinely less predictable than a straight line, and averaging it over
// several samples is the whole point. For zoom and rotation the "cosine" is a
// product of two scalars divided by its own absolute value, so it can only ever
// be exactly +1 or negative. There is no partial agreement to average. The slow
// ramp spends five samples re-deriving a boolean.
//
// The cost showed up as the user's "zoom prediction is lagging behind slightly".
// A pan drag lasts seconds and reaches full extrapolation long before it ends; a
// wheel zoom is one short animation of roughly 150-300 ms, so at 0.35 it spends
// much of the gesture at a fraction of the allowance it has earned -- and the
// allowance, not the horizon, is the binding constraint (the 2026-08-13 sweep).
//
// The instant collapse is UNCHANGED: `UpdateConsistency` still returns 0 outright
// on a sign flip, so a reversal is distrusted on the sample it happens, exactly
// as before. Only the rebuild is faster, and only where the signal is a boolean.
//
// ⚠️ REASONED, NOT MEASURED. The argument above says the gain should be HIGHER
// here; it does not say 0.75. That number is a compromise -- two agreeing samples
// reach 94% -- which keeps some smoothing against a jittery rate estimate. It
// owes the same kind of sweep `kMinPredictedStepsAhead` got.
constexpr double kScalarConsistencyGain = 0.75;

// ⚠️ A ZOOM PREDICTION NEEDS AN ABSOLUTE CEILING AS WELL AS A RELATIVE ONE. Zoom
// is extrapolated in log space, so a large step compounds: the sweep produced
// single-frame errors of 12,183 px on `fast wheel zoom` at scale 2.0, which is
// the overlay leaving the screen entirely. No single frame should ever change
// the zoom by more than half, whatever the rate says.
constexpr double kMaxPredictedLogZoom = 0.405;   // log(1.5)

// ⚠️ VELOCITY IS BLENDED, NOT REPLACED. A single tick's estimate is noisy --
// the poll interval jitters between one and two system ticks -- and feeding that
// straight into a prediction makes the overlay shimmer while the view moves
// steadily. Blending also makes a STOP decay over a few ticks rather than
// requiring a special case for it.
constexpr double kVelocityBlend = 0.5;

// Fold `observed` into `state` and return where the view is predicted to be
// `horizonSeconds` from now. `elapsedSeconds` is the time since the previous
// observation; pass 0 or less and no velocity is learned from this one.
//
// The returned fit carries the same validity and diagnostics as `observed`; only
// the four pose numbers are advanced.
PlanCameraFit PredictPlanCamera (PlanCameraPredictor& state, const PlanCameraFit& observed,
                                 double elapsedSeconds, double horizonSeconds);

// ---- blit-time reprojection (PLAT-RE114) -----------------------------------
//
// WHY THIS EXISTS, AND WHY IT IS NOT MORE PREDICTION. Compositing into
// Archicad's own frame (PLAT-RE79) fixed WHEN our pixels land -- same frame as
// Archicad's drawing, no compositor race -- and did nothing about WHAT they
// contain. The image was rendered from a camera sampled one poll earlier and
// finished a frame before that, so a perfectly aligned frame carries slightly
// stale content. Measured 10-18 ms, and unmoved by every timing change tried.
//
// A PLAN OVERLAY DOES NOT NEED RE-RENDERING TO BE CORRECTED. Its content is a
// parallel projection of a plane, so a change of camera is EXACTLY a 2D affine
// transform of the finished image -- no parallax, no disocclusion, nothing the
// pixels cannot express. So the image is warped through the difference at the
// moment it is blitted, using the newest camera we have. This is async timewarp,
// for the same reason head-mounted displays use it: the render is always late,
// so correct it at the last possible instant instead of trying to be early.
//
// ⚠️ IT IS EXACT FOR THE PLAN AND ONLY APPROXIMATE IN 3D, where rotation
// reprojects cleanly and translation does not -- a flat image has no parallax to
// give. This function is the plan case; 3D is a separate question.
//
// ⚠️ THE CONVENTION IS THE RENDERER'S, TAKEN FROM Camera::GetViewMatrix RATHER
// THAN ASSUMED. Top-down, `up = (-sin r, cos r, 0)`, which makes view space
// `R(-r) * (m - c)`; the projection divides by the half extents, and D3D's NDC
// has +Y up while a texture's V runs down. Getting any one of those backwards
// produces a warp that looks plausible and drifts the wrong way.
struct PlanReprojection {
    // uv0 = warp(uv1). Applied in the pixel shader to the OUTPUT pixel's uv to
    // find where to sample the rendered frame. `scale`/`cos`/`sin` act on
    // centred, aspect-corrected coordinates; `offsetX/Y` are in units of the
    // RENDERED frame's half-height.
    float scale = 1.0f;
    float cosDelta = 1.0f;
    float sinDelta = 0.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    // False when either camera is unusable; the caller then blits unwarped
    // rather than warping through a zero half-height.
    bool valid = false;
};

// `rendered` is the pose the frame in hand was drawn with; `current` is the
// newest pose known. Both are (centreX, centreY, halfHeightMetres, rotation).
PlanReprojection ComputePlanReprojection (double renderedCentreX, double renderedCentreY,
                                          double renderedHalfHeight, double renderedRotation,
                                          double currentCentreX, double currentCentreY,
                                          double currentHalfHeight, double currentRotation);

}   // namespace archviz
}   // namespace geomsrv

#endif
