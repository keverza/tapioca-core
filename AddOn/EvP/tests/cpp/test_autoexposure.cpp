// ArchViz/AutoExposure — RE51.B9's exposure estimate and its white balance.
//
// ⚠️ WHAT THESE TESTS CAN AND CANNOT SETTLE. They pin the SHAPE of the answer:
// monotonicity, the clamps, the exact identities, and the sense of each control
// (a warm light must cool the image, not warm it). They cannot say that middle
// grey is the right target for architectural stills on this project's materials
// -- that is one number, and only a live run against a real render can produce
// it. The probe reports the estimate beside the fixed exposure for exactly that
// reason, and the auto path ships OFF until it has been read once.

#include "ArchViz/AutoExposure.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv::archviz;

namespace {

SceneLight SunnyExterior ()
{
    SceneLight light;
    light.environmentActive = true;
    light.skyIrradiance[0] = 0.60f;
    light.skyIrradiance[1] = 0.62f;
    light.skyIrradiance[2] = 0.68f;
    light.sunStrength = 1.0f;
    light.sunWeight = 0.55f;
    light.meanAlbedo = 0.25f;
    return light;
}

}   // namespace

TEST (AutoExposure, BrighterSceneNeedsLessExposure)
{
    SceneLight dim = SunnyExterior ();
    SceneLight bright = SunnyExterior ();
    for (int c = 0; c < 3; ++c)
        bright.skyIrradiance[c] = dim.skyIrradiance[c] * 4.0f;

    EXPECT_LT (AutoExposure (bright), AutoExposure (dim));
}

// ⚠️ THE CASE B9 EXISTS FOR. "Fixed key is fine for well-lit exteriors, wrong
// for dusk" -- so a sky two and a half stops down must move the exposure, and
// move it UP.
TEST (AutoExposure, DuskOpensUp)
{
    SceneLight day = SunnyExterior ();
    SceneLight dusk = SunnyExterior ();
    for (int c = 0; c < 3; ++c)
        dusk.skyIrradiance[c] *= 0.15f;
    dusk.sunStrength = 0.2f;

    EXPECT_GT (AutoExposure (dusk), AutoExposure (day) * 1.5f);
}

TEST (AutoExposure, DarkerSurfacesNeedMoreExposure)
{
    SceneLight pale = SunnyExterior ();
    SceneLight dark = SunnyExterior ();
    dark.meanAlbedo = pale.meanAlbedo * 0.25f;

    EXPECT_GT (AutoExposure (dark), AutoExposure (pale));
}

// A model that arrives before its sky must not produce an infinity, a NaN, or a
// white screen. This is the frame the viewer actually draws on startup.
TEST (AutoExposure, NoLightAtAllClampsRatherThanDividing)
{
    SceneLight none;
    none.environmentActive = true;
    none.sunStrength = 0.0f;
    none.meanAlbedo = 0.0f;

    const float exposure = AutoExposure (none);
    EXPECT_TRUE (std::isfinite (exposure));
    EXPECT_FLOAT_EQ (exposure, kMaxAutoExposure);
}

TEST (AutoExposure, AbsurdlyBrightSceneStillClamps)
{
    SceneLight blinding = SunnyExterior ();
    for (int c = 0; c < 3; ++c)
        blinding.skyIrradiance[c] = 10000.0f;

    EXPECT_FLOAT_EQ (AutoExposure (blinding), kMinAutoExposure);
}

// A scene already AT middle grey needs no correction, which is what makes the
// constant a calibration rather than a fudge.
TEST (AutoExposure, MiddleGreySceneGetsUnitExposure)
{
    SceneLight light;
    light.environmentActive = true;
    light.sunStrength = 0.0f;
    light.sunWeight = 0.0f;
    light.meanAlbedo = 1.0f;
    for (int c = 0; c < 3; ++c)
        light.skyIrradiance[c] = kMiddleGrey;

    EXPECT_NEAR (MeanSceneLuminance (light), kMiddleGrey, 1e-6f);
    EXPECT_NEAR (AutoExposure (light), 1.0f, 1e-4f);
}

// The two lighting branches are separate arithmetic and the shader picks between
// them; the estimate must pick the same way or it computes the exposure for a
// scene that is not being drawn.
TEST (AutoExposure, TheHemisphericFallbackIsUsedWhenNoSkyIsLoaded)
{
    SceneLight withSky = SunnyExterior ();
    SceneLight withoutSky = SunnyExterior ();
    withoutSky.environmentActive = false;

    EXPECT_NE (MeanSceneLuminance (withSky), MeanSceneLuminance (withoutSky));
}

// ---- white balance ---------------------------------------------------------

// ⚠️ THE CONTRACT THAT PROTECTS EVERY IMAGE RENDERED BEFORE THIS EXISTED.
TEST (WhiteBalance, D65IsExactlyTheIdentity)
{
    const WhiteBalanceGains gains = ComputeWhiteBalance (kNeutralKelvin, 0.0f);
    EXPECT_FLOAT_EQ (gains.rgb[0], 1.0f);
    EXPECT_FLOAT_EQ (gains.rgb[1], 1.0f);
    EXPECT_FLOAT_EQ (gains.rgb[2], 1.0f);
}

// ⚠️ THE SENSE OF THE CONTROL, WHICH IS THE ONE THING EVERYBODY GETS BACKWARDS.
// Correcting FOR a warm 3000 K tungsten light must COOL the render: less red,
// more blue. The opposite convention also "works" and produces a picture that
// looks warmer when you ask for warmer, which is what makes it so easy to ship.
TEST (WhiteBalance, CorrectingForAWarmLightCoolsTheImage)
{
    const WhiteBalanceGains warm = ComputeWhiteBalance (3000.0f, 0.0f);
    EXPECT_LT (warm.rgb[0], 1.0f);
    EXPECT_GT (warm.rgb[2], 1.0f);
}

TEST (WhiteBalance, CorrectingForACoolLightWarmsTheImage)
{
    const WhiteBalanceGains cool = ComputeWhiteBalance (9000.0f, 0.0f);
    EXPECT_GT (cool.rgb[0], 1.0f);
    EXPECT_LT (cool.rgb[2], 1.0f);
}

// Green stays the pivot, so a temperature change is not also an exposure change.
TEST (WhiteBalance, GreenIsUnityAtEveryTemperature)
{
    for (float kelvin : { 2000.0f, 3200.0f, 5000.0f, 6500.0f, 9000.0f, 20000.0f })
        EXPECT_FLOAT_EQ (ComputeWhiteBalance (kelvin, 0.0f).rgb[1], 1.0f) << kelvin;
}

TEST (WhiteBalance, TintIsIndependentOfTemperature)
{
    const WhiteBalanceGains plain = ComputeWhiteBalance (5000.0f, 0.0f);
    const WhiteBalanceGains tinted = ComputeWhiteBalance (5000.0f, 0.5f);
    EXPECT_NE (plain.rgb[0], tinted.rgb[0]);
    EXPECT_FLOAT_EQ (tinted.rgb[1], 1.0f);
}

// ⚠️ THIS TEST FOUND A REAL ONE AND IS THE REASON THE GAIN CLAMP EXISTS. The
// first version clamped only the TEMPERATURE, and 1667 K -- the bottom of the
// Planckian fit's valid range -- is so nearly pure red that its blue component
// rounds to nothing: the corrective blue gain came out at 5209x. Arithmetically
// right, and useless. See kMaxWhiteBalanceGain.
TEST (WhiteBalance, OutOfRangeTemperaturesAreClampedNotExtrapolated)
{
    for (float kelvin : { 1.0f, 100.0f, 1667.0f, 2000.0f, 1e6f, -500.0f }) {
        const WhiteBalanceGains gains = ComputeWhiteBalance (kelvin, 0.0f);
        for (int c = 0; c < 3; ++c) {
            EXPECT_TRUE (std::isfinite (gains.rgb[c])) << kelvin;
            EXPECT_GE (gains.rgb[c], kMinWhiteBalanceGain) << kelvin;
            EXPECT_LE (gains.rgb[c], kMaxWhiteBalanceGain) << kelvin;
        }
    }
}
