// Tests for ArchViz/SunShadowMath — where the sun's shadow camera goes.
//
// ⚠️ THESE TEST THE PROPERTY, NOT THE ARITHMETIC. Re-deriving the matrix here
// and comparing would pass for any consistent pair of wrong implementations.
// What actually has to be true is that the model's own corners land inside the
// light's clip volume, in front of it, with depth increasing away from the sun
// -- so that is what is checked, by pushing the AABB's eight corners through
// the matrix the function returns.
//
// Every failure mode being guarded against renders a plausible picture rather
// than an error, which is why none of them would be caught by looking:
//   frustum too small          -> the shadow stops at a straight edge
//   frustum too large          -> the shadow is a low-resolution blur
//   depth range inverted       -> everything is shadowed except what should be
//   up vector parallel to sun  -> the matrix is NaN and the model goes black

#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunShadowMath.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

// The eight corners of an AABB, in the order (x, y, z) bits of 0..7.
void Corner (float out[3], const float lo[3], const float hi[3], int index)
{
    out[0] = (index & 1) ? hi[0] : lo[0];
    out[1] = (index & 2) ? hi[1] : lo[1];
    out[2] = (index & 4) ? hi[2] : lo[2];
}

// True when a world point lands inside the light's clip volume: x,y in [-1,1],
// z in [0,1] (D3D convention), w positive.
bool InsideLightClip (const SunShadow& fit, const float world[3], float slack = 1e-4f)
{
    const float point[4] = {world[0], world[1], world[2], 1.0f};
    float clip[4];
    TransformPoint (clip, point, fit.lightViewProj);
    if (!(clip[3] > 0.0f))
        return false;
    const float x = clip[0] / clip[3];
    const float y = clip[1] / clip[3];
    const float z = clip[2] / clip[3];
    return x >= -1.0f - slack && x <= 1.0f + slack &&
           y >= -1.0f - slack && y <= 1.0f + slack &&
           z >= 0.0f - slack && z <= 1.0f + slack;
}

float LightDepth (const SunShadow& fit, const float world[3])
{
    const float point[4] = {world[0], world[1], world[2], 1.0f};
    float clip[4];
    TransformPoint (clip, point, fit.lightViewProj);
    return clip[2] / clip[3];
}

}   // namespace

TEST (SunShadowMath, CoversEveryCornerOfTheModel)
{
    // A deliberately off-origin, non-cubic box: an Archicad project's origin is
    // wherever the user put it, and a fit that only works about the origin is a
    // fit that works on the debug cube and nowhere else.
    const float lo[3] = {112.0f, -48.0f, 3.5f};
    const float hi[3] = {138.0f, -12.0f, 21.0f};

    // A morning sun in the south-east, well above the horizon.
    const float sun[3] = {0.5f, -0.6f, 0.62f};

    const SunShadow fit = FitSunShadow (lo, hi, sun, 2048);
    ASSERT_TRUE (fit.valid);

    for (int i = 0; i < 8; ++i) {
        float corner[3];
        Corner (corner, lo, hi, i);
        EXPECT_TRUE (InsideLightClip (fit, corner)) << "corner " << i << " is outside the "
            "light's frustum -- its shadow would be clipped off at a straight edge";
    }
}

TEST (SunShadowMath, CoversTheModelFromEverySunAngle)
{
    const float lo[3] = {-6.0f, -4.0f, 0.0f};
    const float hi[3] = {6.0f, 4.0f, 9.0f};

    // Sweep the sky: every azimuth, from just above the horizon to the zenith.
    // The zenith is the case that needs the up-vector fallback, and without it
    // the matrix is NaN -- which InsideLightClip reports as "outside".
    for (int azimuth = 0; azimuth < 360; azimuth += 15) {
        for (int elevation = 5; elevation <= 90; elevation += 5) {
            const float a = float (azimuth) * 3.14159265f / 180.0f;
            const float e = float (elevation) * 3.14159265f / 180.0f;
            const float sun[3] = {std::cos (e) * std::cos (a),
                                  std::cos (e) * std::sin (a),
                                  std::sin (e)};
            const SunShadow fit = FitSunShadow (lo, hi, sun, 1024);
            ASSERT_TRUE (fit.valid) << "azimuth " << azimuth << " elevation " << elevation;
            for (int i = 0; i < 8; ++i) {
                float corner[3];
                Corner (corner, lo, hi, i);
                EXPECT_TRUE (InsideLightClip (fit, corner))
                    << "azimuth " << azimuth << " elevation " << elevation << " corner " << i;
            }
        }
    }
}

TEST (SunShadowMath, DepthIncreasesAwayFromTheSun)
{
    // THE test that separates a working shadow map from one that shadows
    // everything except what should be in shadow. Two points on the same ray
    // from the sun: the one nearer the sun must have the SMALLER stored depth,
    // because the depth test keeps the nearest and that is the occluder.
    const float lo[3] = {-5.0f, -5.0f, 0.0f};
    const float hi[3] = {5.0f, 5.0f, 10.0f};
    const float sun[3] = {0.0f, 0.0f, 1.0f};   // straight overhead

    const SunShadow fit = FitSunShadow (lo, hi, sun, 1024);
    ASSERT_TRUE (fit.valid);

    const float high[3] = {0.0f, 0.0f, 9.0f};   // the roof: nearer the sun
    const float low[3] = {0.0f, 0.0f, 1.0f};    // the ground: further away
    EXPECT_LT (LightDepth (fit, high), LightDepth (fit, low));
}

TEST (SunShadowMath, TexelSizeScalesWithTheModelAndTheResolution)
{
    const float lo[3] = {0.0f, 0.0f, 0.0f};
    const float hi[3] = {10.0f, 10.0f, 10.0f};
    const float sun[3] = {0.3f, -0.4f, 0.87f};

    const SunShadow small = FitSunShadow (lo, hi, sun, 1024);
    const SunShadow large = FitSunShadow (lo, hi, sun, 2048);
    ASSERT_TRUE (small.valid);
    ASSERT_TRUE (large.valid);
    // Twice the resolution over the same model is half the world size per texel.
    EXPECT_NEAR (small.texelWorldSize, large.texelWorldSize * 2.0f, 1e-5f);

    const float bigHi[3] = {20.0f, 20.0f, 20.0f};
    const SunShadow bigger = FitSunShadow (lo, bigHi, sun, 1024);
    ASSERT_TRUE (bigger.valid);
    // Twice the model at the same resolution is twice the world size per texel.
    EXPECT_NEAR (bigger.texelWorldSize, small.texelWorldSize * 2.0f, 1e-4f);
}

TEST (SunShadowMath, TexelSizeDoesNotChangeAsTheSunMoves)
{
    // The reason the fit uses a bounding SPHERE rather than the AABB's projected
    // extent. With the extent, a shadow's quality would depend on the time of
    // day and its edges would crawl while the sun animates.
    const float lo[3] = {-8.0f, -3.0f, 0.0f};
    const float hi[3] = {8.0f, 3.0f, 12.0f};

    const float morning[3] = {0.8f, -0.3f, 0.52f};
    const float noon[3] = {0.05f, 0.05f, 0.997f};
    const SunShadow a = FitSunShadow (lo, hi, morning, 2048);
    const SunShadow b = FitSunShadow (lo, hi, noon, 2048);
    ASSERT_TRUE (a.valid);
    ASSERT_TRUE (b.valid);
    EXPECT_FLOAT_EQ (a.texelWorldSize, b.texelWorldSize);
    EXPECT_FLOAT_EQ (a.depthRange, b.depthRange);
}

TEST (SunShadowMath, RefusesADegenerateSunOrAnEmptyScene)
{
    const float lo[3] = {0.0f, 0.0f, 0.0f};
    const float hi[3] = {10.0f, 10.0f, 10.0f};
    const float sun[3] = {0.3f, -0.4f, 0.87f};

    // A sun of zero length -- which is exactly what a sun below the horizon can
    // arrive as. Rendering with the NaN matrix that a naive normalize would give
    // shadows the whole model to the ambient floor, and it reads as night.
    const float noSun[3] = {0.0f, 0.0f, 0.0f};
    EXPECT_FALSE (FitSunShadow (lo, hi, noSun, 2048).valid);

    // A degenerate AABB: no geometry has arrived yet. Not an error -- the
    // ordinary state for the first second of a live extraction.
    EXPECT_FALSE (FitSunShadow (lo, lo, sun, 2048).valid);

    // Inverted bounds, and a zero resolution.
    EXPECT_FALSE (FitSunShadow (hi, lo, sun, 2048).valid);
    EXPECT_FALSE (FitSunShadow (lo, hi, sun, 0).valid);
}

TEST (SunShadowMath, TheMatrixIsFiniteEverywhere)
{
    // NaN is the one output that renders as a plausible picture (an unshadowed
    // scene, or a black one) on every path that consumes it.
    const float lo[3] = {-1.0f, -1.0f, -1.0f};
    const float hi[3] = {1.0f, 1.0f, 1.0f};
    const float straightUp[3] = {0.0f, 0.0f, 1.0f};
    const float straightDown[3] = {0.0f, 0.0f, -1.0f};

    for (const float* sun : {straightUp, straightDown}) {
        const SunShadow fit = FitSunShadow (lo, hi, sun, 512);
        ASSERT_TRUE (fit.valid);
        for (int i = 0; i < 16; ++i)
            EXPECT_TRUE (std::isfinite (fit.lightViewProj[i])) << "element " << i;
    }
}
