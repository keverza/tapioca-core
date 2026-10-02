// ArchViz/Dxgi/CameraShape: what a b1/b0 pair says about its camera -- pitch, field, and how far
// the frustum is off its axis. The camera is the census's own MATRIX line from the session spent
// toggling two-point perspective (2026-10-02 13:46:17, archviz.log): g3 occ2 idx60948 at model
// generation 1, rows as logged, on a 3177x1809 view. The two-point camera is built from it the
// way a two-point perspective is drawn -- the view axis level, the image moved instead -- since
// what Archicad draws one with is the open question the watch exists to answer.

#include "ArchViz/Dxgi/CameraShape.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace camerashape = geomsrv::archviz::dxgi::camerashape;
namespace cameralayout = geomsrv::archviz::dxgi::cameralayout;

namespace {

const float kView[16] = { -0.0799f, -0.7913f, 0.6062f, 0.0f, 0.9968f,   -0.0634f, 0.0486f,    0.0f,
                          -0.0f,    0.6081f,  0.7938f, 0.0f, 113.8975f, 19.1990f, -641.8741f, 1.0f };
const float kB0[16] = { -0.1041f, -1.8114f, -0.6309f, -0.6062f, 1.2992f, -0.1451f, -0.0505f, -0.0486f,
                        -0.0f,    1.3921f,  -0.8262f, -0.7938f, 0.0f,    0.0f,     -0.2041f, 0.0f };

constexpr double kDegrees = 57.29577951308232;

struct Pair {
    float view[16] = {};
    float b0[16] = {};
};

// The logged camera as a two-point perspective: the same eye, yaw and focal lengths, the view
// axis turned level, and the image moved by `shiftY` along it.
Pair TwoPointOf (const float view[16], const float b0[16], double shiftY)
{
    const camerashape::Shape logged = camerashape::Of (view, b0);
    double w[3], x[3], y[3], depth[3];
    cameralayout::detail::Column (b0, 3, w);
    cameralayout::detail::Column (b0, 0, x);
    cameralayout::detail::Column (b0, 1, y);
    cameralayout::detail::Column (b0, 2, depth);
    const double fx = std::sqrt (cameralayout::detail::Dot (x, x));
    const double fy = 1.0 / std::tan (logged.fovY / 2.0 / kDegrees);
    const double level = std::sqrt (w[0] * w[0] + w[1] * w[1]);
    const double forward[3] = { w[0] / level, w[1] / level, 0.0 };
    // Right is made from forward, not taken from the logged x column: four logged decimals leave
    // that 4e-5 from square, which moves the eye a view derives by 3 cm at this one's 650 m.
    const double right[3] = { forward[1], -forward[0], 0.0 };
    const double up[3] = { 0.0, 0.0, 1.0 };
    const double depthScale = cameralayout::detail::Dot (depth, w);

    Pair pair;
    // b0: x = fx * right, y = fy * up + shiftY * forward, depth along the axis, w = forward.
    for (int r = 0; r < 3; ++r) {
        pair.b0[r * 4 + 0] = float (fx * right[r]);
        pair.b0[r * 4 + 1] = float (fy * up[r] + shiftY * forward[r]);
        pair.b0[r * 4 + 2] = float (depthScale * forward[r]);
        pair.b0[r * 4 + 3] = float (forward[r]);
    }
    pair.b0[14] = b0[14];
    // b1: columns right, up, back; t = -eye * R.
    const double back[3] = { -forward[0], -forward[1], -forward[2] };
    for (int r = 0; r < 3; ++r) {
        pair.view[r * 4 + 0] = float (right[r]);
        pair.view[r * 4 + 1] = float (up[r]);
        pair.view[r * 4 + 2] = float (back[r]);
    }
    for (int c = 0; c < 3; ++c)
        pair.view[12 + c] = float (
            -(logged.eye[0] * pair.view[c] + logged.eye[1] * pair.view[4 + c] + logged.eye[2] * pair.view[8 + c]));
    pair.view[15] = 1.0f;
    return pair;
}

} // namespace

TEST (CameraShape, TheLoggedCameraIsCentredAndLooksDown)
{
    const camerashape::Shape shape = camerashape::Of (kView, kB0);
    EXPECT_TRUE (shape.view);
    EXPECT_TRUE (shape.rotationProjection);
    EXPECT_TRUE (shape.decodes);
    EXPECT_LT (shape.mismatch, 1e-4);
    // Down at the model, 52.5 degrees, by both windows.
    EXPECT_NEAR (shape.pitchB0, -52.5, 0.1);
    EXPECT_NEAR (shape.pitchB1, shape.pitchB0, 0.05);
    // A centred frustum: nothing of x or y along the axis.
    EXPECT_NEAR (shape.shiftX, 0.0, 1e-3);
    EXPECT_NEAR (shape.shiftY, 0.0, 1e-3);
    // The field's two tangents in the window's proportion.
    const double aspect = std::tan (shape.fovX / 2.0 / kDegrees) / std::tan (shape.fovY / 2.0 / kDegrees);
    EXPECT_NEAR (aspect, 3177.0 / 1809.0, 2e-3);
}

TEST (CameraShape, APointOnTheViewAxisLandsInTheMiddle)
{
    const camerashape::Shape shape = camerashape::Of (kView, kB0);
    double w[3];
    cameralayout::detail::Column (kB0, 3, w);
    const double target[3] = { shape.eye[0] + 40.0 * w[0], shape.eye[1] + 40.0 * w[1], shape.eye[2] + 40.0 * w[2] };
    double ndc[2] = {};
    ASSERT_TRUE (camerashape::ToNdc (kView, kB0, target, ndc));
    EXPECT_NEAR (ndc[0], 0.0, 1e-3);
    EXPECT_NEAR (ndc[1], 0.0, 1e-3);
    const double behind[3] = { shape.eye[0] - 40.0 * w[0], shape.eye[1] - 40.0 * w[1], shape.eye[2] - 40.0 * w[2] };
    EXPECT_FALSE (camerashape::ToNdc (kView, kB0, behind, ndc));
}

// A two-point perspective keeps the same target in the middle by moving the image: its axis is
// level, and the shift is the tangent of the pitch it no longer has, times the focal length.
TEST (CameraShape, ATwoPointCameraIsLevelAndOffAxis)
{
    const camerashape::Shape logged = camerashape::Of (kView, kB0);
    const double fy = 1.0 / std::tan (logged.fovY / 2.0 / kDegrees);
    const double shift = -fy * std::tan (logged.pitchB0 / kDegrees);
    const Pair two = TwoPointOf (kView, kB0, shift);

    const camerashape::Shape shape = camerashape::Of (two.view, two.b0);
    EXPECT_TRUE (shape.decodes) << "finding 1 already takes an off-axis frustum";
    EXPECT_NEAR (shape.pitchB0, 0.0, 1e-3);
    EXPECT_NEAR (shape.pitchB1, 0.0, 1e-3);
    EXPECT_NEAR (shape.shiftX, 0.0, 1e-4);
    EXPECT_NEAR (shape.shiftY, shift, 1e-4);
    EXPECT_NEAR (shape.eye[0], logged.eye[0], 1e-2);
    EXPECT_NEAR (shape.eye[2], logged.eye[2], 1e-2);

    double w[3];
    cameralayout::detail::Column (kB0, 3, w);
    const double target[3] = { logged.eye[0] + 40.0 * w[0], logged.eye[1] + 40.0 * w[1], logged.eye[2] + 40.0 * w[2] };
    double ndc[2] = {};
    ASSERT_TRUE (camerashape::ToNdc (two.view, two.b0, target, ndc));
    EXPECT_NEAR (ndc[0], 0.0, 1e-3);
    EXPECT_NEAR (ndc[1], 0.0, 1e-3) << "the target stays in the middle";
}

// What the watch would show if Archicad levelled b0 for the image but left b1 tilted: the pair
// is refused, and the two pitches say why.
TEST (CameraShape, ALevelProjectionUnderATiltedViewIsRefusedAndSaysWhy)
{
    const camerashape::Shape logged = camerashape::Of (kView, kB0);
    const double fy = 1.0 / std::tan (logged.fovY / 2.0 / kDegrees);
    const Pair two = TwoPointOf (kView, kB0, -fy * std::tan (logged.pitchB0 / kDegrees));

    const camerashape::Shape shape = camerashape::Of (kView, two.b0);
    EXPECT_TRUE (shape.view);
    EXPECT_TRUE (shape.rotationProjection);
    EXPECT_FALSE (shape.decodes);
    EXPECT_GT (shape.mismatch, 0.1);
    EXPECT_NEAR (shape.pitchB0, 0.0, 1e-3);
    EXPECT_NEAR (shape.pitchB1, -52.5, 0.1);
}
