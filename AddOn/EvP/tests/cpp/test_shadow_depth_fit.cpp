// ShadowDepthFit (#234): the sun's cascades split the depth the scene occupies, not the
// projection's 0.05..20,000 m. A wrong answer here does not look wrong: a range too wide gives
// back the centimetre texels that leaked light into every corner, and one too narrow drops the
// far part of the model out of every cascade, unshadowed.

#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/ShadowDepthFit.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

void View (float out[16], float ex, float ey, float ez, float tx, float ty, float tz)
{
    const float eye[3] = { ex, ey, ez }, at[3] = { tx, ty, tz }, up[3] = { 0.0f, 0.0f, 1.0f };
    LookAtRH (out, eye, at, up);
}

bool QuarterOctave (float v)
{
    const float steps = std::log2 (v) * 4.0f;
    return std::abs (steps - std::round (steps)) < 1e-4f;
}

} // namespace

// Inside a room: the near plane stays, the far plane comes in to just beyond the farthest corner.
TEST (ShadowDepthFit, RoomFromInsideKeepsNearAndPullsFarToTheFarthestCorner)
{
    float view[16];
    View (view, 0.0f, -0.9f, 1.0f, 0.0f, 1.0f, 1.0f); // looking +Y from just inside the front
    const float lo[3] = { -1.0f, -1.0f, 0.0f }, hi[3] = { 1.0f, 1.0f, 2.0f };
    float nearDepth = 0.05f, farDepth = 20000.0f;
    ASSERT_TRUE (FitShadowDepthRange (view, lo, hi, nearDepth, farDepth));
    EXPECT_FLOAT_EQ (nearDepth, 0.05f); // a corner is behind the eye: nothing to raise
    EXPECT_GE (farDepth, 1.9f);         // the back wall is 1.9 m away along the view
    EXPECT_LE (farDepth, 1.9f * 1.19f + 1e-4f);
    EXPECT_TRUE (QuarterOctave (farDepth));
}

// Outside the model: both ends come in, and still bracket every corner.
TEST (ShadowDepthFit, ModelAheadRaisesNearAndBracketsEveryCorner)
{
    float view[16];
    View (view, 0.0f, -50.0f, 10.0f, 0.0f, 0.0f, 5.0f);
    const float lo[3] = { -10.0f, -10.0f, 0.0f }, hi[3] = { 10.0f, 10.0f, 12.0f };
    float nearDepth = 0.05f, farDepth = 20000.0f;
    ASSERT_TRUE (FitShadowDepthRange (view, lo, hi, nearDepth, farDepth));
    for (int corner = 0; corner < 8; ++corner) {
        const float p[4] = { (corner & 1) ? hi[0] : lo[0], (corner & 2) ? hi[1] : lo[1], (corner & 4) ? hi[2] : lo[2],
                             1.0f };
        float v[4];
        TransformPoint (v, p, view);
        EXPECT_GE (-v[2], nearDepth) << corner;
        EXPECT_LE (-v[2], farDepth) << corner;
    }
    EXPECT_GT (nearDepth, 30.0f);
    EXPECT_LT (farDepth, 80.0f);
    EXPECT_TRUE (QuarterOctave (nearDepth));
    EXPECT_TRUE (QuarterOctave (farDepth));
}

// A small move does not change the range: the cascades' texel grid must not follow an orbit.
TEST (ShadowDepthFit, SmallCameraMoveKeepsTheQuantisedRange)
{
    const float lo[3] = { -1.0f, -1.0f, 0.0f }, hi[3] = { 1.0f, 1.0f, 2.0f };
    float view[16];
    View (view, 0.0f, -0.9f, 1.0f, 0.0f, 1.0f, 1.0f);
    float n0 = 0.05f, f0 = 20000.0f;
    ASSERT_TRUE (FitShadowDepthRange (view, lo, hi, n0, f0));
    View (view, 0.01f, -0.91f, 1.0f, 0.0f, 1.0f, 1.0f);
    float n1 = 0.05f, f1 = 20000.0f;
    ASSERT_TRUE (FitShadowDepthRange (view, lo, hi, n1, f1));
    EXPECT_EQ (n0, n1);
    EXPECT_EQ (f0, f1);
}

// Never widens: a projection already tighter than the model keeps its own range.
TEST (ShadowDepthFit, NeverWidensTheProjection)
{
    float view[16];
    View (view, 0.0f, -50.0f, 10.0f, 0.0f, 0.0f, 5.0f);
    const float lo[3] = { -10.0f, -10.0f, 0.0f }, hi[3] = { 10.0f, 10.0f, 12.0f };
    float nearDepth = 0.05f, farDepth = 20.0f; // ends in front of the model
    FitShadowDepthRange (view, lo, hi, nearDepth, farDepth);
    EXPECT_LE (farDepth, 20.0f);
    EXPECT_GE (nearDepth, 0.05f);
    EXPECT_LT (nearDepth, farDepth);
}

// Behind the camera, or degenerate input: the range is left exactly as it was.
TEST (ShadowDepthFit, BoxBehindTheCameraOrBadInputLeavesTheRange)
{
    float view[16];
    View (view, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f); // looking +Y
    const float lo[3] = { -1.0f, -10.0f, 0.0f }, hi[3] = { 1.0f, -5.0f, 2.0f };
    float nearDepth = 0.05f, farDepth = 20000.0f;
    EXPECT_FALSE (FitShadowDepthRange (view, lo, hi, nearDepth, farDepth));
    EXPECT_FLOAT_EQ (nearDepth, 0.05f);
    EXPECT_FLOAT_EQ (farDepth, 20000.0f);
    EXPECT_FALSE (FitShadowDepthRange (nullptr, lo, hi, nearDepth, farDepth));
    EXPECT_FALSE (FitShadowDepthRange (view, nullptr, hi, nearDepth, farDepth));
    float badNear = 0.0f;
    EXPECT_FALSE (FitShadowDepthRange (view, lo, hi, badNear, farDepth));
}
