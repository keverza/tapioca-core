// L2 offline tests for SunStudy/SunStudyRaster.
//
// The Python module private/Commands/SunStudy/sunraster.py is the ORACLE: this
// file is a transcription of it, and the parity block below pins the C++ against
// values produced by the Python. A divergence there is exactly the class of bug
// that yields a plausible wrong picture rather than a failure, which is why the
// numbers are written out rather than recomputed by a helper both sides share.

#include "SunStudy/SunStudyRaster.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace evp::sunstudy;

namespace {

constexpr double kEps = 1e-9;

// A 20 x 10 x 30 box away from the origin, so a bug that assumes a centred model
// shows up.
const Vec3 kBoxMin { 100.0, 200.0, 0.0 };
const Vec3 kBoxMax { 120.0, 210.0, 30.0 };

// Sun at 45 degrees in the XZ plane, pointing TO the sun.
Vec3 SunXZ45 ()
{
    const double s = std::sqrt (0.5);
    return { s, 0.0, s };
}

} // namespace

// ---------------------------------------------------------------------------
// frustum fitting
// ---------------------------------------------------------------------------

TEST (SunStudyRaster, FitProducesOrthonormalBasisAlignedToTheSun)
{
    const OrthoFrustum f = FitOrthoFrustum (kBoxMin, kBoxMax, SunXZ45 ());
    ASSERT_TRUE (f.valid);

    // The light travels FROM the sun, so lightAxis is the negated sun vector.
    const Vec3 sun = SunXZ45 ();
    EXPECT_NEAR (f.lightAxis[0], -sun[0], kEps);
    EXPECT_NEAR (f.lightAxis[1], -sun[1], kEps);
    EXPECT_NEAR (f.lightAxis[2], -sun[2], kEps);

    auto dot = [] (const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    EXPECT_NEAR (dot (f.rightAxis, f.rightAxis), 1.0, 1e-12);
    EXPECT_NEAR (dot (f.upAxis, f.upAxis), 1.0, 1e-12);
    EXPECT_NEAR (dot (f.lightAxis, f.lightAxis), 1.0, 1e-12);
    EXPECT_NEAR (dot (f.rightAxis, f.upAxis), 0.0, 1e-12);
    EXPECT_NEAR (dot (f.rightAxis, f.lightAxis), 0.0, 1e-12);
    EXPECT_NEAR (dot (f.upAxis, f.lightAxis), 0.0, 1e-12);
}

TEST (SunStudyRaster, EveryCornerLandsInsideTheFittedFrustum)
{
    const OrthoFrustum f = FitOrthoFrustum (kBoxMin, kBoxMax, SunXZ45 ());
    ASSERT_TRUE (f.valid);
    const Mat4 vp = LightViewProjection (f);

    const Vec3 corners[8] = {
        { kBoxMin[0], kBoxMin[1], kBoxMin[2] }, { kBoxMax[0], kBoxMin[1], kBoxMin[2] },
        { kBoxMin[0], kBoxMax[1], kBoxMin[2] }, { kBoxMin[0], kBoxMin[1], kBoxMax[2] },
        { kBoxMax[0], kBoxMax[1], kBoxMin[2] }, { kBoxMax[0], kBoxMin[1], kBoxMax[2] },
        { kBoxMin[0], kBoxMax[1], kBoxMax[2] }, { kBoxMax[0], kBoxMax[1], kBoxMax[2] },
    };
    for (const Vec3& corner : corners) {
        const Vec3 ndc = TransformPoint (vp, corner);
        EXPECT_GE (ndc[0], -1.0) << "corner left of the frustum";
        EXPECT_LE (ndc[0], 1.0) << "corner right of the frustum";
        EXPECT_GE (ndc[1], -1.0) << "corner below the frustum";
        EXPECT_LE (ndc[1], 1.0) << "corner above the frustum";
        EXPECT_GE (ndc[2], -1.0) << "corner behind the near plane";
        EXPECT_LE (ndc[2], 1.0) << "corner beyond the far plane";
    }
}

// ⚠️ THE REGRESSION THAT MADE THE RASTER ENGINE DISAGREE WITH THE RAY-CAST ONE.
// Clamping `near` to a positive floor clipped every sunward occluder out of the
// depth pass. For any sun above the horizon over a box sitting on z = 0, the
// fitted near plane must come out negative.
TEST (SunStudyRaster, NearPlaneIsNegativeAndMustNotBeClamped)
{
    // Centred on the origin, so the scene straddles the rotation origin along
    // the light axis and the two planes land either side of zero. This is the
    // shape the clamp destroyed.
    const OrthoFrustum f = FitOrthoFrustum (Vec3 { -10.0, -10.0, 0.0 }, Vec3 { 10.0, 10.0, 20.0 }, SunXZ45 ());
    ASSERT_TRUE (f.valid);
    EXPECT_LT (f.nearPlane, 0.0);
    EXPECT_GT (f.farPlane, 0.0);
    EXPECT_GT (f.worldDepth, 0.0);
    EXPECT_NEAR (f.worldDepth, f.farPlane - f.nearPlane, kEps);
}

// ⚠️ BOTH PLANES MAY BE NEGATIVE, AND THAT IS NOT A DEGENERATE CASE. A model at
// survey coordinates sits far from the rotation origin along the light axis, so
// its whole depth range can fall on one side of zero. Only the SPAN is
// meaningful. A reader who "fixes" the frustum by forcing near < 0 < far would
// break every real project while every origin-centred test kept passing.
TEST (SunStudyRaster, BothDepthPlanesMayBeNegativeAtSurveyCoordinates)
{
    const OrthoFrustum f = FitOrthoFrustum (kBoxMin, kBoxMax, SunXZ45 ());
    ASSERT_TRUE (f.valid);
    EXPECT_LT (f.nearPlane, 0.0);
    EXPECT_LT (f.farPlane, 0.0);
    EXPECT_LT (f.nearPlane, f.farPlane);
    EXPECT_GT (f.worldDepth, 0.0);

    // And the projection still maps the range onto [-1, 1] regardless.
    EXPECT_GT (DepthBiasNdc (0.01, f.nearPlane, f.farPlane), 0.0);
}

TEST (SunStudyRaster, OverheadSunDoesNotCollapseTheBasis)
{
    // cross(+Z, -Z) is the zero vector; the up candidate has to flip or every
    // number downstream becomes NaN. Noon at low latitude is this input.
    const OrthoFrustum f = FitOrthoFrustum (kBoxMin, kBoxMax, Vec3 { 0.0, 0.0, 1.0 });
    ASSERT_TRUE (f.valid);
    EXPECT_FALSE (std::isnan (f.rightAxis[0]));
    EXPECT_FALSE (std::isnan (f.upAxis[1]));
    EXPECT_GT (f.worldWidth, 0.0);
    EXPECT_GT (f.worldHeight, 0.0);
}

TEST (SunStudyRaster, DegenerateInputIsRefusedNotThrown)
{
    EXPECT_FALSE (FitOrthoFrustum (kBoxMin, kBoxMax, Vec3 { 0.0, 0.0, 0.0 }).valid);
    EXPECT_FALSE (FitOrthoFrustum (kBoxMax, kBoxMin, SunXZ45 ()).valid);
}

// ---------------------------------------------------------------------------
// RTC
// ---------------------------------------------------------------------------

TEST (SunStudyRaster, RtcOriginIsTheRoundedCentre)
{
    const Vec3 origin = RtcOrigin (kBoxMin, kBoxMax);
    EXPECT_NEAR (origin[0], 110.0, kEps);
    EXPECT_NEAR (origin[1], 205.0, kEps);
    EXPECT_NEAR (origin[2], 15.0, kEps);

    // Stable across runs: whole metres, so two dumps stay comparable.
    const Vec3 odd = RtcOrigin (Vec3 { 0.0, 0.0, 0.0 }, Vec3 { 1.3, 1.3, 1.3 });
    EXPECT_NEAR (odd[0], 1.0, kEps);
}

// ⚠️ THE ALL-OR-NOTHING RULE, AS AN ASSERTION. A shifted matrix on shifted
// points must equal an unshifted matrix on unshifted points; if it does not,
// every shadow lands in the wrong place at full confidence.
TEST (SunStudyRaster, RtcShiftedMatrixMatchesUnshiftedOnUnshiftedPoints)
{
    const OrthoFrustum f = FitOrthoFrustum (kBoxMin, kBoxMax, SunXZ45 ());
    ASSERT_TRUE (f.valid);

    const Vec3 origin = RtcOrigin (kBoxMin, kBoxMax);
    const Mat4 plain = LightViewProjection (f, nullptr);
    const Mat4 shifted = LightViewProjection (f, &origin);

    const Vec3 probes[3] = { { 100.0, 200.0, 0.0 }, { 110.5, 204.25, 12.5 }, { 120.0, 210.0, 30.0 } };
    for (const Vec3& p : probes) {
        const Vec3 a = TransformPoint (plain, p);
        const Vec3 rtc { p[0] - origin[0], p[1] - origin[1], p[2] - origin[2] };
        const Vec3 b = TransformPoint (shifted, rtc);
        EXPECT_NEAR (a[0], b[0], 1e-9);
        EXPECT_NEAR (a[1], b[1], 1e-9);
        EXPECT_NEAR (a[2], b[2], 1e-9);
    }
}

// ---------------------------------------------------------------------------
// bias
// ---------------------------------------------------------------------------

TEST (SunStudyRaster, BiasIsTwoTexelsWithAFloor)
{
    EXPECT_NEAR (DeriveBias (0.01), 0.02, kEps);

    // The floor wins on a tight frustum, so the bias can never vanish.
    EXPECT_NEAR (DeriveBias (0.0001), kMinBiasMetres, kEps);
    EXPECT_NEAR (DeriveBias (0.0), kMinBiasMetres, kEps);
}

TEST (SunStudyRaster, TexelSizeUsesTheLargerFrustumSide)
{
    OrthoFrustum f;
    f.worldWidth = 100.0;
    f.worldHeight = 20.0;
    f.valid = true;
    EXPECT_NEAR (TexelWorldSize (f, 1000), 0.1, kEps);
    EXPECT_NEAR (TexelWorldSize (f, 0), 0.0, kEps);
}

TEST (SunStudyRaster, DepthBiasIsAFractionOfTheDepthSpanAndIsClamped)
{
    EXPECT_NEAR (DepthBiasNdc (1.0, -50.0, 50.0), 0.01, kEps);

    // A negative near is ordinary; only the span matters.
    EXPECT_NEAR (DepthBiasNdc (2.0, -10.0, 10.0), 0.1, kEps);

    // A degenerate span must not make everything unconditionally lit.
    EXPECT_NEAR (DepthBiasNdc (1.0, 5.0, 5.0), 0.0, kEps);
    EXPECT_NEAR (DepthBiasNdc (1.0, 10.0, 0.0), 0.0, kEps);
    EXPECT_NEAR (DepthBiasNdc (100.0, 0.0, 1.0), 0.5, kEps);
}

TEST (SunStudyRaster, PickBiasTakesTheMaximumAndFloorsAnEmptySequence)
{
    OrthoFrustum tight;
    tight.worldWidth = tight.worldHeight = 10.0;
    tight.valid = true;
    OrthoFrustum wide;
    wide.worldWidth = wide.worldHeight = 1000.0;
    wide.valid = true;

    const double bias = PickBias ({ tight, wide }, 1000);
    EXPECT_NEAR (bias, DeriveBiasFromFrustum (wide, 1000), kEps);

    // Every timestep below the horizon is legitimate, and zero bias is acne on
    // every surface.
    EXPECT_NEAR (PickBias ({}, 1000), kMinBiasMetres, kEps);

    OrthoFrustum invalid;
    EXPECT_NEAR (PickBias ({ invalid }, 1000), kMinBiasMetres, kEps);
}

// ---------------------------------------------------------------------------
// accumulator layout
// ---------------------------------------------------------------------------

TEST (SunStudyRaster, AccumLayoutIsRoughlySquareAndCoversEverySample)
{
    for (const size_t n : { size_t { 1 }, size_t { 2 }, size_t { 99 }, size_t { 100 }, size_t { 84052 } }) {
        const AccumLayout layout = ComputeAccumLayout (n);
        ASSERT_TRUE (layout.valid) << "n = " << n;
        EXPECT_GE (static_cast<size_t> (layout.width) * layout.height, n) << "n = " << n;
        EXPECT_LE (layout.width, kMaxTextureSize);
        EXPECT_LE (layout.height, kMaxTextureSize);
    }

    const AccumLayout square = ComputeAccumLayout (100);
    EXPECT_EQ (square.width, 10u);
    EXPECT_EQ (square.height, 10u);
}

TEST (SunStudyRaster, AccumLayoutRefusesRatherThanTruncating)
{
    const AccumLayout tooBig = ComputeAccumLayout (static_cast<size_t> (kMaxTextureSize) * kMaxTextureSize + 1);
    EXPECT_FALSE (tooBig.valid);
}

TEST (SunStudyRaster, SampleIndexMapsToRowMajorTexels)
{
    uint32_t column = 0;
    uint32_t row = 0;
    SampleIndexToTexel (0, 10, column, row);
    EXPECT_EQ (column, 0u);
    EXPECT_EQ (row, 0u);

    SampleIndexToTexel (9, 10, column, row);
    EXPECT_EQ (column, 9u);
    EXPECT_EQ (row, 0u);

    SampleIndexToTexel (10, 10, column, row);
    EXPECT_EQ (column, 0u);
    EXPECT_EQ (row, 1u);

    SampleIndexToTexel (23, 10, column, row);
    EXPECT_EQ (column, 3u);
    EXPECT_EQ (row, 2u);
}

// ---------------------------------------------------------------------------
// grid ladder
// ---------------------------------------------------------------------------

TEST (SunStudyRaster, GridLadderAlwaysEndsAtTheRequestedGrid)
{
    // Small model: one rung, because a coarse pass buys only a flicker.
    const std::vector<double> small = GridLadder (100.0, 1.0);
    ASSERT_EQ (small.size (), 1u);
    EXPECT_NEAR (small.back (), 1.0, kEps);

    // Mid: two rungs.
    const std::vector<double> mid = GridLadder (10000.0, 1.0);
    ASSERT_EQ (mid.size (), 2u);
    EXPECT_NEAR (mid[0], 2.0, kEps);
    EXPECT_NEAR (mid[1], 1.0, kEps);

    // Large: three rungs, coarsest first.
    const std::vector<double> large = GridLadder (100000.0, 1.0);
    ASSERT_EQ (large.size (), 3u);
    EXPECT_NEAR (large[0], 4.0, kEps);
    EXPECT_NEAR (large[1], 2.0, kEps);
    EXPECT_NEAR (large[2], 1.0, kEps);
}

TEST (SunStudyRaster, GridLadderToleratesANonPositiveTarget)
{
    const std::vector<double> ladder = GridLadder (1000.0, 0.0);
    ASSERT_EQ (ladder.size (), 1u);
    EXPECT_NEAR (ladder[0], 0.0, kEps);
}

// ---------------------------------------------------------------------------
// parity with the Python oracle
// ---------------------------------------------------------------------------
//
// Values below were produced by private/Commands/SunStudy/sunraster.py on the
// same inputs. They are literals rather than a shared helper on purpose: a
// helper both sides call cannot detect the two drifting apart.

TEST (SunStudyRaster, ParityWithPythonSunRaster)
{
    const Vec3 lo { 0.0, 0.0, 0.0 };
    const Vec3 hi { 10.0, 10.0, 10.0 };
    const Vec3 sun { 0.0, 0.0, 1.0 }; // straight overhead: L = -Z, up flips to +Y

    const OrthoFrustum f = FitOrthoFrustum (lo, hi, sun, 1.0);
    ASSERT_TRUE (f.valid);

    // up_candidate flips to (0,1,0); R = cross((0,1,0), (0,0,-1)) = (-1, 0, 0);
    // U = cross((0,0,-1), (-1,0,0)) = (0, 1, 0).
    EXPECT_NEAR (f.rightAxis[0], -1.0, kEps);
    EXPECT_NEAR (f.rightAxis[1], 0.0, kEps);
    EXPECT_NEAR (f.rightAxis[2], 0.0, kEps);
    EXPECT_NEAR (f.upAxis[1], 1.0, kEps);
    EXPECT_NEAR (f.lightAxis[2], -1.0, kEps);

    // proj_r over the corners spans [-10, 0]; proj_u spans [0, 10];
    // proj_l = -z spans [-10, 0]. Each padded by the 1.0 margin.
    EXPECT_NEAR (f.left, -11.0, kEps);
    EXPECT_NEAR (f.right, 1.0, kEps);
    EXPECT_NEAR (f.bottom, -1.0, kEps);
    EXPECT_NEAR (f.top, 11.0, kEps);
    EXPECT_NEAR (f.nearPlane, -11.0, kEps);
    EXPECT_NEAR (f.farPlane, 1.0, kEps);
    EXPECT_NEAR (f.worldWidth, 12.0, kEps);
    EXPECT_NEAR (f.worldHeight, 12.0, kEps);
    EXPECT_NEAR (f.worldDepth, 12.0, kEps);

    EXPECT_NEAR (TexelWorldSize (f, 2048), 12.0 / 2048.0, kEps);
    // 2 x 0.005859 = 0.01171875, which clears the 0.005 floor, so the texel term
    // wins rather than the floor.
    EXPECT_NEAR (DeriveBiasFromFrustum (f, 2048), 12.0 / 1024.0, kEps);
    EXPECT_NEAR (DepthBiasNdc (0.005, f.nearPlane, f.farPlane), 0.005 / 12.0, kEps);

    // The centre of the box maps to the centre of the frustum in x and y.
    const Mat4 vp = LightViewProjection (f);
    const Vec3 centre = TransformPoint (vp, Vec3 { 5.0, 5.0, 5.0 });
    EXPECT_NEAR (centre[0], 0.0, 1e-12);
    EXPECT_NEAR (centre[1], 0.0, 1e-12);

    EXPECT_NEAR (RtcOrigin (lo, hi)[0], 5.0, kEps);
}

// ---------------------------------------------------------------------------
// the ground sample grid
// ---------------------------------------------------------------------------

// ⚠️ THE PADDING RULE IS WHAT MAKES THE STUDY MEAN ANYTHING. Sampling the
// model's own footprint puts every sample under a building, so the engine
// correctly reports about zero hours everywhere and the run says nothing.
TEST (SunStudyRaster, GroundPadReachesOutByTheModelsHeight)
{
    // A 20 x 20 footprint, 30 m tall: height wins.
    EXPECT_NEAR (AutoGroundPad (Vec3 { 0, 0, 0 }, Vec3 { 20, 20, 30 }), 30.0, kEps);

    // A 200 x 200 footprint, 2 m tall: a quarter of the footprint wins.
    EXPECT_NEAR (AutoGroundPad (Vec3 { 0, 0, 0 }, Vec3 { 200, 200, 2 }), 50.0, kEps);

    // A tiny model: the floor wins, so the grid is never degenerate.
    EXPECT_NEAR (AutoGroundPad (Vec3 { 0, 0, 0 }, Vec3 { 1, 1, 1 }), 2.0, kEps);
}

TEST (SunStudyRaster, GroundGridCoversThePaddedFootprint)
{
    const GroundGrid grid = MakeGroundSampleGrid (Vec3 { 0, 0, 0 }, Vec3 { 10, 10, 10 }, 1.0, 5.0 /* pad */, 0.1);
    ASSERT_TRUE (grid.valid);

    // 0 - 5 to 10 + 5 is 20 m across, at 1 m spacing: 21 positions per axis.
    EXPECT_EQ (grid.columns, 21u);
    EXPECT_EQ (grid.rows, 21u);
    EXPECT_EQ (grid.positions.size (), 21u * 21u * 3u);
    EXPECT_EQ (grid.normals.size (), grid.positions.size ());
    EXPECT_NEAR (grid.groundZ, 0.1, kEps);
    EXPECT_NEAR (grid.pad, 5.0, kEps);

    // Every sample sits on the ground plane, faces up, and lies inside the
    // padded footprint.
    for (size_t i = 0; i < grid.positions.size () / 3; ++i) {
        EXPECT_NEAR (grid.positions[i * 3 + 2], 0.1, kEps);
        EXPECT_NEAR (grid.normals[i * 3 + 0], 0.0, kEps);
        EXPECT_NEAR (grid.normals[i * 3 + 1], 0.0, kEps);
        EXPECT_NEAR (grid.normals[i * 3 + 2], 1.0, kEps);
        EXPECT_GE (grid.positions[i * 3 + 0], -5.0 - kEps);
        EXPECT_LE (grid.positions[i * 3 + 0], 15.0 + kEps);
        EXPECT_GE (grid.positions[i * 3 + 1], -5.0 - kEps);
        EXPECT_LE (grid.positions[i * 3 + 1], 15.0 + kEps);
    }
}

// The leftover after a whole number of steps is split between the two edges, so
// the grid is not biased toward the minimum corner.
TEST (SunStudyRaster, GroundGridIsCentredWhenTheSpacingDoesNotDivideEvenly)
{
    const GroundGrid grid = MakeGroundSampleGrid (Vec3 { 0, 0, 0 }, Vec3 { 10, 10, 1 }, 3.0, 0.0, 0.0);
    ASSERT_TRUE (grid.valid);
    ASSERT_EQ (grid.columns, 4u); // floor(10/3) + 1

    const double first = grid.positions[0];
    const double last = grid.positions[(grid.columns - 1) * 3];
    EXPECT_NEAR (first - 0.0, 10.0 - last, 1e-9) << "the two margins must match";
}

TEST (SunStudyRaster, GroundGridAutoPadsWhenNoPadIsGiven)
{
    const GroundGrid grid = MakeGroundSampleGrid (Vec3 { 0, 0, 0 }, Vec3 { 20, 20, 30 }, 5.0);
    ASSERT_TRUE (grid.valid);
    EXPECT_NEAR (grid.pad, 30.0, kEps) << "the model's own height";
}

// ⚠️ REFUSED, NOT TRUNCATED. A silently shortened grid analyses part of the site
// and looks like a complete answer.
TEST (SunStudyRaster, GroundGridRefusesRatherThanTruncating)
{
    EXPECT_FALSE (MakeGroundSampleGrid (Vec3 { 0, 0, 0 }, Vec3 { 10, 10, 10 }, 0.0).valid);
    EXPECT_FALSE (MakeGroundSampleGrid (Vec3 { 0, 0, 0 }, Vec3 { 10, 10, 10 }, -1.0).valid);
    EXPECT_FALSE (MakeGroundSampleGrid (Vec3 { 10, 10, 10 }, Vec3 { 0, 0, 0 }, 1.0).valid) << "inverted box";

    // A spacing fine enough to blow the ceiling is refused, not clipped.
    EXPECT_FALSE (MakeGroundSampleGrid (Vec3 { 0, 0, 0 }, Vec3 { 1000, 1000, 1 }, 0.001, 0.0, 0.0, 1000).valid);
}

TEST (SunStudyRaster, ADegenerateFootprintStillProducesASample)
{
    const GroundGrid grid = MakeGroundSampleGrid (Vec3 { 5, 5, 0 }, Vec3 { 5, 5, 0 }, 1.0, 0.0, 0.0);
    ASSERT_TRUE (grid.valid);
    EXPECT_EQ (grid.columns, 1u);
    EXPECT_EQ (grid.rows, 1u);
    EXPECT_EQ (grid.positions.size (), 3u);
}
