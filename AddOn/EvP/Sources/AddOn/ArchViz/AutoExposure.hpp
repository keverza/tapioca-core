#ifndef EVP_ARCHVIZ_AUTOEXPOSURE_HPP
#define EVP_ARCHVIZ_AUTOEXPOSURE_HPP

// ArchViz/AutoExposure — RE51.B9's second half: an exposure that follows the
// light, and a white balance that follows its colour.
//
// ---------------------------------------------------------------------------
// ⚠️ IT IS ANALYTIC, NOT A FRAMEBUFFER HISTOGRAM, AND THAT IS A DECISION WITH A
// REASON RATHER THAN A SHORTCUT. B9's wording asks for "automatic exposure from
// luminance percentiles". A percentile needs the distribution of the SHADED
// image before tone mapping, and this renderer has nowhere to read that from:
// the mesh pixel shader tone maps and writes straight to the swap chain, whose
// _SRGB view is already display-referred and already clipped. Getting a real
// percentile means an HDR intermediate colour target plus a resolve pass -- a
// change to the frame graph, which is PLAT-RE135's job and not this one's.
//
// What is available instead is BETTER-CONDITIONED for this renderer, and it is
// what the analytic form uses: the scene's light is entirely KNOWN before a
// pixel is drawn. The sky's irradiance is nine measured SH coefficients, the
// sun's strength and its share are constants of the frame, and the mean
// reflectance of the building is the mean of a surface pool that was just read
// out of Archicad. Mean luminance is the product, and no readback, no stall and
// no frame of latency is involved.
//
// ⚠️ AND IT CANNOT FLICKER OR HUNT, which a histogram over a moving camera
// does. There is no feedback loop here at all: the estimate does not depend on
// the image, so pointing the camera at a dark wall cannot brighten the world.
// That is the failure mode auto-exposure is notorious for in walkthroughs.
//
// ⚠️ WHAT IT GIVES UP, SAID OUT LOUD. It is a MEAN, so it cannot protect
// highlights the way a 95th-percentile metering would, and it is scene-wide, so
// it does not adapt when the camera moves from an atrium into a basement. Both
// of those need the HDR target above. Recorded rather than hidden.
//
// ---------------------------------------------------------------------------
// ⚠️ PURE. No Diligent, no ACAPI, no I/O -- so the whole of it runs in the
// offline suite, which is the entire reason the Kelvin arithmetic lives here
// and not in HLSL.

#include "ArchViz/MaterialTable.hpp"

#include <cstdint>

namespace geomsrv {
namespace archviz {

// What the frame knows about its own light, before it is drawn.
struct SceneLight {
    // The sky's DC irradiance term, per channel -- `sh[0] * 0.282095`, which is
    // exactly what ShDiffuse returns averaged over all normals. ⚠️ THE 1/pi AND
    // THE COSINE CONVOLUTION ARE ALREADY IN THE COEFFICIENTS (see
    // EnvironmentLighting), so this multiplies albedo directly and is not an
    // irradiance in the textbook sense. Do not divide by pi again.
    float skyIrradiance[3] = { 0.0f, 0.0f, 0.0f };

    // The analytic sun's overall strength (`skyColor.w`) and the share of the
    // budget it keeps beside an active sky (`materialParams.y`). Both are
    // exactly the numbers the shader uses.
    float sunStrength = 1.0f;
    float sunWeight = 1.0f;

    // The two-colour hemispheric ambient, used only when no HDR sky is loaded.
    float ambientSky[3] = { 0.55f, 0.62f, 0.75f };
    float ambientGround[3] = { 0.22f, 0.20f, 0.18f };
    float ambientShare = 0.35f;

    bool environmentActive = false;

    // The mean LINEAR reflectance of the surfaces in the model, 0..1.
    // ⚠️ LINEAR, WHICH IS WHY IT IS SO MUCH DARKER THAN A COLOUR PICKER SAYS.
    // The pool's colours are sRGB-decoded on the way in (RE51.B7), so a project
    // of mid-grey paint sits near 0.2, not near 0.5. Feeding an encoded mean
    // here would over-expose every image by roughly a factor of two.
    float meanAlbedo = 0.18f;
};

// ⚠️ 0.18, THE PHOTOGRAPHIC MIDDLE GREY, AND IT IS THE ONE CALIBRATION CONSTANT
// IN THE FILE. A scene whose mean shaded luminance already IS middle grey needs
// no correction, so it gets exposure 1.0. Everything else is that ratio.
constexpr float kMiddleGrey = 0.18f;

// ⚠️ THE CLAMP IS NOT DEFENSIVE, IT IS THE DIFFERENCE BETWEEN AUTO-EXPOSURE AND
// A WHITE SCREEN. A model loaded before its sky, or a project of black surfaces,
// produces a mean luminance near zero and an unbounded correction; two and a
// half stops either side of neutral covers overcast-to-noon and still leaves a
// genuinely dark dusk scene looking like dusk rather than like noon.
constexpr float kMinAutoExposure = 0.2f;
constexpr float kMaxAutoExposure = 6.0f;

// The mean shaded luminance the frame will produce, before tone mapping.
// Exposed separately from the exposure so the probe can report the INPUT as
// well as the answer -- a wrong exposure and a wrong luminance estimate look
// identical from the outside, and only one of them is this file's fault.
float MeanSceneLuminance (const SceneLight& light);

// The pre-scale into the tone curve. Monotonically DECREASING in the scene's
// luminance: a brighter scene needs less exposure.
float AutoExposure (const SceneLight& light);

// ---- white balance ---------------------------------------------------------
//
// ⚠️ GAINS, NOT A MATRIX, AND NOT A CHROMATIC ADAPTATION TRANSFORM. A proper
// von Kries adaptation would go through LMS; this is the per-channel form every
// real-time renderer ships, and at the temperatures architecture is graded at
// (roughly 3000 K to 9000 K) the difference is below the precision of anything
// else in this pipeline. Said here so nobody re-derives it as a bug.
//
// `kelvin` is the temperature of the light being CORRECTED FOR, so a warm
// 3000 K light yields gains that COOL the image -- the same sense as a camera's
// white-balance setting, and the opposite of "make the image warmer".
//
// `tint` shifts green (negative) to magenta (positive), -1..1, and is the axis
// perpendicular to temperature. 0 leaves it alone.
struct WhiteBalanceGains {
    float rgb[3] = { 1.0f, 1.0f, 1.0f };
};

// ⚠️ 6500 K IS THE IDENTITY, EXACTLY. sRGB's white point is D65, so correcting
// for 6500 K must return 1,1,1 and change no image ever rendered before this
// existed. The offline test pins that, because a white balance that quietly
// tints a neutral scene is indistinguishable from a colour-space bug.
constexpr float kNeutralKelvin = 6500.0f;

// ⚠️ THE GAINS ARE CLAMPED, AND CLAMPING THE TEMPERATURE IS NOT ENOUGH -- the
// offline test caught this. A 1667 K illuminant is very nearly pure red, so its
// blue component is near zero and the corrective blue gain came out at 5209x.
// That is arithmetically correct and physically meaningless: multiplying a
// channel by five thousand does not recover colour that was never captured, it
// amplifies whatever noise and quantisation that channel holds. A balance that
// needs more than two stops either way is not correcting an image, it is
// inventing one, and the honest behaviour at that point is to stop short and
// let the result look like what it is.
constexpr float kMinWhiteBalanceGain = 0.25f;
constexpr float kMaxWhiteBalanceGain = 4.0f;

WhiteBalanceGains ComputeWhiteBalance (float kelvin, float tint);

// The mean LINEAR luminance of the surface pool -- the auto exposure's one
// input that comes from the model rather than from the light.
//
// ⚠️ UNWEIGHTED BY AREA, AND THAT IS A KNOWN APPROXIMATION rather than an
// oversight. Weighting by projected area would need the geometry, which this
// file deliberately cannot see; a pool of forty surfaces where the two used on
// every wall are dark and the thirty-eight used on door handles are pale will
// read too bright. It is the second thing to correct if the live estimate comes
// out wrong, after kMeanShadow.
//
// ⚠️ SURFACES AT ZERO OPACITY ARE SKIPPED. Archicad's templates carry cut-plane
// and helper surfaces authored fully transparent; they contribute no reflected
// light at all, and averaging their colours in drags the mean toward whatever
// they happen to be.
float MeanPoolAlbedo (const MaterialTable& pool);

} // namespace archviz
} // namespace geomsrv

#endif
