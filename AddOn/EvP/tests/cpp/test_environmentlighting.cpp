// Tests for ArchViz/EnvironmentLighting — the HDR sky reduced to 9 SH coefficients.
//
// ⚠️ EVERY FAILURE MODE HERE RENDERS A PLAUSIBLE PICTURE. That is the whole
// reason this file exists rather than a live look: a spherical-harmonic
// irradiance that is uniformly 3.14 times too bright is a building that looks
// over-exposed, which gets blamed on the exposure and TUNED OUT somewhere else
// in the shader -- after which the error is permanent and invisible. The same
// goes for a missing solid-angle weight (the sky's zenith swamps the horizon and
// the model is lit flat from above) and for a mirrored direction convention (the
// sun's reflection lands on the wrong side of every window, on a photograph of a
// road where nobody can tell).
//
// So the tests are chosen to have ANALYTICALLY KNOWN answers, not to restate the
// implementation:
//
//   a CONSTANT sky must reconstruct to ITSELF, exactly, in every direction.
//     This is the single most valuable assertion in the file -- it pins down the
//     pi bookkeeping, the basis normalisation and the solid-angle weight all at
//     once, and NOTHING ELSE catches a stray factor of pi.
//
//   a sky bright in ONE hemisphere must light a normal pointing into it more
//     than one pointing away, and the two must average back to the constant case.
//
//   resampling must PRESERVE the solid-angle-weighted mean, because that mean is
//     the total energy the sky delivers.

#include "ArchViz/EnvironmentLighting.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace geomsrv::archviz;

namespace {

// A sky whose every texel is the same radiance.
EquirectImage ConstantSky (uint32_t w, uint32_t h, float r, float g, float b)
{
    EquirectImage image;
    image.width = w;
    image.height = h;
    image.rgb.resize (size_t (w) * h * 3);
    for (size_t i = 0; i < size_t (w) * h; ++i) {
        image.rgb[i * 3 + 0] = r;
        image.rgb[i * 3 + 1] = g;
        image.rgb[i * 3 + 2] = b;
    }
    return image;
}

// A sky lit only where the texel's direction has z > 0 (the upper hemisphere).
EquirectImage UpperHemisphereSky (uint32_t w, uint32_t h, float value)
{
    EquirectImage image = ConstantSky (w, h, 0.0f, 0.0f, 0.0f);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float dir[3];
            DirectionAt (x, y, w, h, dir);
            if (dir[2] > 0.0f) {
                const size_t texel = (size_t (y) * w + x) * 3;
                image.rgb[texel + 0] = value;
                image.rgb[texel + 1] = value;
                image.rgb[texel + 2] = value;
            }
        }
    }
    return image;
}

}   // namespace

// ---------------------------------------------------------------------------
// THE PI TEST. A uniform environment of radiance c delivers, to a Lambertian
// surface of albedo 1, exactly radiance c back -- in every direction. Any stray
// factor of pi, any un-normalised basis constant, any missing solid angle shows
// up here and essentially nowhere else.
// ---------------------------------------------------------------------------
TEST (EnvironmentLighting, ConstantSkyRoundTripsToItself)
{
    const EquirectImage sky = ConstantSky (64, 32, 0.5f, 0.25f, 0.75f);
    const ShIrradiance sh = ProjectIrradiance (sky);

    // Directions covering both poles, the equator and several oblique angles --
    // a constant sky has no preferred direction, so ALL of them must agree.
    const float directions[][3] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
        {1, 1, 1}, {-1, 0.5f, -0.25f}, {0.3f, -0.8f, 0.5f},
    };

    for (const auto& n : directions) {
        float out[3] = {};
        EvaluateDiffuse (sh, n, out);
        EXPECT_NEAR (out[0], 0.5f, 2e-3f);
        EXPECT_NEAR (out[1], 0.25f, 2e-3f);
        EXPECT_NEAR (out[2], 0.75f, 2e-3f);
    }
}

// The result must not depend on how finely the sky was sampled. A projection
// missing its solid-angle weight passes the constant test at ONE resolution and
// drifts with the row count, so this is the second half of that assertion.
//
// ⚠️ THE TOLERANCE TIGHTENS WITH RESOLUTION ON PURPOSE -- A FLAT ONE WOULD BE
// THE WRONG ASSERTION, and getting this wrong the first time is what the
// tightening records. The row solid angles are integrated in CLOSED FORM, so
// they contribute no error at all; what remains is that the SH basis is sampled
// at each texel's centre rather than averaged over the texel, which leaves a
// residual that vanishes as O(1/height^2). Measured: 0.59% at 8 rows, and
// roughly a sixteenth of that per doubling.
//
// A single loose tolerance would pass an implementation that does not converge
// at all, and a single tight one would fail on a 16x8 sky that no real path
// produces. What actually has to be true is that the error SHRINKS -- and that
// at the size the loader resamples to it is far below anything visible.
TEST (EnvironmentLighting, ConstantSkyConvergesWithResolution)
{
    struct Case {
        uint32_t width, height;
        float tolerance;
    };
    const Case cases[] = {
        {16, 8, 1e-2f},       // absurdly coarse; no real sky is this small
        {64, 32, 1e-3f},
        {256, 128, 1e-4f},    // finer than the loader's fixed target
    };
    const float n[3] = {0.2f, -0.4f, 0.9f};

    float previousError = 1.0f;
    for (const Case& c : cases) {
        const ShIrradiance sh = ProjectIrradiance (ConstantSky (c.width, c.height, 1.0f, 1.0f, 1.0f));
        float out[3] = {};
        EvaluateDiffuse (sh, n, out);
        const float error = std::fabs (out[0] - 1.0f);
        EXPECT_LT (error, c.tolerance) << "at " << c.width << "x" << c.height;
        EXPECT_LT (error, previousError) << "error must shrink as the sky gets finer";
        previousError = error;
    }
}

// ---------------------------------------------------------------------------
// DIRECTIONALITY -- the entire point of replacing the two-colour ambient.
// ---------------------------------------------------------------------------
TEST (EnvironmentLighting, UpFacingNormalGetsMoreLightFromAnUpperHemisphereSky)
{
    const ShIrradiance sh = ProjectIrradiance (UpperHemisphereSky (128, 64, 1.0f));

    const float up[3] = {0, 0, 1};
    const float down[3] = {0, 0, -1};
    float lit[3] = {}, unlit[3] = {};
    EvaluateDiffuse (sh, up, lit);
    EvaluateDiffuse (sh, down, unlit);

    EXPECT_GT (lit[0], unlit[0]);
    // A surface facing straight up sees the whole lit hemisphere; one facing
    // down sees none of it. The order-2 reconstruction does not reach zero
    // (it rings), but it must be well under a quarter of the lit value or the
    // sky is not actually directional.
    EXPECT_LT (unlit[0], lit[0] * 0.25f);
}

// ⚠️ THE HORIZON MUST LAND BETWEEN THE TWO, and roughly halfway. A projection
// that dropped the l=1 band entirely would still pass the test above (l=0 alone
// carries some difference through the l=2 term) but would put the horizon in the
// wrong place, which is what makes a wall's vertical gradient read correctly.
TEST (EnvironmentLighting, HorizonSitsBetweenZenithAndNadir)
{
    const ShIrradiance sh = ProjectIrradiance (UpperHemisphereSky (128, 64, 1.0f));

    const float up[3] = {0, 0, 1};
    const float side[3] = {1, 0, 0};
    const float down[3] = {0, 0, -1};
    float lit[3] = {}, horizon[3] = {}, unlit[3] = {};
    EvaluateDiffuse (sh, up, lit);
    EvaluateDiffuse (sh, side, horizon);
    EvaluateDiffuse (sh, down, unlit);

    EXPECT_GT (horizon[0], unlit[0]);
    EXPECT_LT (horizon[0], lit[0]);
    EXPECT_NEAR (horizon[0], (lit[0] + unlit[0]) * 0.5f, lit[0] * 0.2f);
}

// The reconstruction is allowed to ring, but never to hand the shader a negative
// colour -- which would SUBTRACT light further along the pixel shader.
TEST (EnvironmentLighting, DiffuseIsNeverNegative)
{
    // A single very bright texel is the worst case for ringing: a small, intense
    // sun is exactly what order-2 SH cannot represent.
    EquirectImage sky = ConstantSky (64, 32, 0.0f, 0.0f, 0.0f);
    const size_t centre = (size_t (8) * 64 + 20) * 3;
    sky.rgb[centre + 0] = 5000.0f;
    sky.rgb[centre + 1] = 5000.0f;
    sky.rgb[centre + 2] = 5000.0f;

    const ShIrradiance sh = ProjectIrradiance (sky);
    for (int i = 0; i < 200; ++i) {
        const float t = float (i) / 200.0f * 6.2831853f;
        const float n[3] = {std::cos (t), std::sin (t), std::cos (t * 0.7f)};
        float out[3] = {};
        EvaluateDiffuse (sh, n, out);
        EXPECT_GE (out[0], 0.0f);
        EXPECT_GE (out[1], 0.0f);
        EXPECT_GE (out[2], 0.0f);
    }
}

// ---------------------------------------------------------------------------
// RESAMPLING -- it must not change how much light the sky delivers.
// ---------------------------------------------------------------------------
TEST (EnvironmentLighting, ResamplePreservesTheWeightedMean)
{
    // A sky with structure at a scale finer than the destination grid, so a
    // point sample would visibly lose energy while a box filter does not.
    EquirectImage sky = ConstantSky (256, 128, 0.0f, 0.0f, 0.0f);
    for (uint32_t y = 0; y < 128; ++y)
        for (uint32_t x = 0; x < 256; ++x)
            if (((x / 2) + (y / 2)) % 2 == 0) {
                const size_t texel = (size_t (y) * 256 + x) * 3;
                sky.rgb[texel + 0] = 4.0f;
                sky.rgb[texel + 1] = 2.0f;
                sky.rgb[texel + 2] = 1.0f;
            }

    float before[3] = {}, after[3] = {};
    AverageRadiance (sky, before);
    AverageRadiance (Resample (sky, 64, 32), after);

    EXPECT_NEAR (before[0], after[0], 0.02f);
    EXPECT_NEAR (before[1], after[1], 0.02f);
    EXPECT_NEAR (before[2], after[2], 0.02f);
}

TEST (EnvironmentLighting, ResampleRejectsInvalidInput)
{
    EXPECT_FALSE (Resample (EquirectImage {}, 32, 16).IsValid ());
    EXPECT_FALSE (Resample (ConstantSky (16, 8, 1, 1, 1), 0, 16).IsValid ());
}

// An invalid image must project to all-zero rather than read out of bounds --
// the load path can hand this in when a file is missing or malformed.
TEST (EnvironmentLighting, InvalidImageProjectsToZero)
{
    const ShIrradiance sh = ProjectIrradiance (EquirectImage {});
    for (int i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c)
            EXPECT_FLOAT_EQ (sh.c[i][c], 0.0f);

    const float n[3] = {0, 0, 1};
    float out[3] = {1.0f, 1.0f, 1.0f};
    EvaluateDiffuse (sh, n, out);
    EXPECT_FLOAT_EQ (out[0], 0.0f);
}

// ⚠️ THE DIRECTION CONVENTION IS AN ABI WITH THE PIXEL SHADER'S OWN EQUIRECT
// LOOKUP. If these two disagree the sky is mirrored or rolled, the diffuse SH
// and the specular reflection disagree with each other, and on a photographic
// sky that is genuinely hard to see. Pinning the corners here means the shader's
// lookup can be checked against a written-down convention rather than by eye.
TEST (EnvironmentLighting, DirectionConventionIsPinned)
{
    float dir[3];

    // Row 0 is the +Z pole.
    DirectionAt (0, 0, 64, 32, dir);
    EXPECT_GT (dir[2], 0.99f);

    // The last row is the -Z pole.
    DirectionAt (0, 31, 64, 32, dir);
    EXPECT_LT (dir[2], -0.99f);

    // The middle row is the horizon, and column 0 is phi = 0, i.e. +X.
    DirectionAt (0, 16, 64, 32, dir);
    EXPECT_NEAR (dir[2], 0.0f, 0.05f);
    EXPECT_GT (dir[0], 0.99f);

    // A quarter of the way round is +Y (counter-clockwise seen from +Z).
    DirectionAt (16, 16, 64, 32, dir);
    EXPECT_GT (dir[1], 0.99f);
}

// ---------------------------------------------------------------------------
// THE REAL SKY. Everything above is synthetic, and CLAUDE.md's rule is that a
// geometry algorithm is never validated on synthetic data alone -- synthetic
// tests have already missed a shipped bug in this repo.
//
// The fixture is `dump/cobblestone_parish_road_2k.hdr` (an outdoor Radiance
// probe: road, hedges, overcast sky) box-downsampled to 128x64 and stored as
// raw float32 RGB. The EXPECTED VALUES BELOW WERE NOT PRODUCED BY THIS CODE --
// they come from an independent projection written separately in NumPy from the
// Radiance spec and Ramamoorthi's paper. Two implementations agreeing on real
// sky data is evidence; one implementation agreeing with itself is not, which is
// the same standard test_serializer holds its hand-rolled msgpack reader to.
//
// ⚠️ WHAT MAKES THIS SKY A GOOD FIXTURE is that its answer is strongly
// DIRECTIONAL and physically legible: the zenith is bright and blue-shifted
// (1.35 / 1.36 / 1.39 -- blue highest) while the nadir is dark and warm
// (0.19 / 0.15 / 0.09 -- red highest, the road bouncing back). A projection that
// dropped the l=1 band, mirrored a direction, or lost the solid-angle weight
// would flatten or invert that seven-to-one contrast, and every one of those is
// a mistake that renders a perfectly plausible building.
// ---------------------------------------------------------------------------
namespace {

// The fixture path is compiled in by CMake so the test does not depend on the
// working directory ctest happens to use.
EquirectImage LoadFixtureSky ()
{
    EquirectImage image;
#ifdef EVP_TEST_FIXTURE_DIR
    const std::string path = std::string (EVP_TEST_FIXTURE_DIR) + "/sky_cobblestone_128x64.f32";
    std::FILE* file = std::fopen (path.c_str (), "rb");
    if (file == nullptr)
        return image;
    uint32_t header[2] = {0, 0};
    if (std::fread (header, sizeof (uint32_t), 2, file) == 2) {
        image.width = header[0];
        image.height = header[1];
        image.rgb.resize (size_t (image.width) * image.height * 3);
        if (std::fread (image.rgb.data (), sizeof (float), image.rgb.size (), file) !=
            image.rgb.size ())
            image = EquirectImage {};
    }
    std::fclose (file);
#endif
    return image;
}

}   // namespace

TEST (EnvironmentLighting, RealSkyMatchesAnIndependentProjection)
{
    const EquirectImage sky = LoadFixtureSky ();
    // SKIP, not fail, when the fixture is absent -- the convention the dump
    // fixtures already follow, so a clone without it still goes green.
    if (!sky.IsValid ())
        GTEST_SKIP () << "the real-sky fixture is not present";
    EXPECT_EQ (sky.width, 128u);
    EXPECT_EQ (sky.height, 64u);

    float mean[3] = {};
    AverageRadiance (sky, mean);
    EXPECT_NEAR (mean[0], 0.698594f, 1e-4f);
    EXPECT_NEAR (mean[1], 0.667868f, 1e-4f);
    EXPECT_NEAR (mean[2], 0.643446f, 1e-4f);

    const ShIrradiance sh = ProjectIrradiance (sky);

    struct Expected {
        const char* name;
        float n[3];
        float rgb[3];
    };
    const Expected cases[] = {
        {"zenith", {0, 0, 1}, {1.350800f, 1.358993f, 1.387489f}},
        {"nadir", {0, 0, -1}, {0.189484f, 0.152666f, 0.093014f}},
        {"east", {1, 0, 0}, {0.457099f, 0.409566f, 0.360437f}},
        {"south", {0, -1, 0}, {0.961494f, 0.922668f, 0.897033f}},
    };

    for (const Expected& c : cases) {
        float out[3] = {};
        EvaluateDiffuse (sh, c.n, out);
        EXPECT_NEAR (out[0], c.rgb[0], 1e-3f) << c.name << " red";
        EXPECT_NEAR (out[1], c.rgb[1], 1e-3f) << c.name << " green";
        EXPECT_NEAR (out[2], c.rgb[2], 1e-3f) << c.name << " blue";
    }
}

// The physical signature of an outdoor sky, asserted as a PROPERTY rather than
// as numbers -- this survives swapping the fixture for a different probe, and it
// is what the two-colour hemispheric ambient was a crude stand-in for.
TEST (EnvironmentLighting, RealSkyIsBlueAboveAndWarmBelow)
{
    const EquirectImage sky = LoadFixtureSky ();
    if (!sky.IsValid ())
        GTEST_SKIP () << "the real-sky fixture is not present";
    const ShIrradiance sh = ProjectIrradiance (sky);

    const float up[3] = {0, 0, 1};
    const float down[3] = {0, 0, -1};
    float zenith[3] = {}, nadir[3] = {};
    EvaluateDiffuse (sh, up, zenith);
    EvaluateDiffuse (sh, down, nadir);

    // The sky is much brighter than the ground bounce.
    EXPECT_GT (zenith[0], nadir[0] * 4.0f);
    // The sky is blue-shifted: blue exceeds red.
    EXPECT_GT (zenith[2], zenith[0]);
    // The ground bounce is warm: red exceeds blue. ⚠️ THIS IS THE ASSERTION A
    // MIRRORED OR ROLLED DIRECTION CONVENTION FAILS, because it depends on which
    // end of the image is the ground.
    EXPECT_GT (nadir[0], nadir[2]);
}
