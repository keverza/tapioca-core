#include "ArchViz/AutoExposure.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

// Rec. 709 luminance, which is the right weighting because everything reaching
// this file is LINEAR sRGB primaries (ColourSpace decodes on the way in).
float Luminance (const float rgb[3])
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

// ⚠️ THE MEAN OF max(dot(n, l), 0) OVER A SPHERE OF NORMALS IS 0.25, and the
// mesh shader's two-sided flip does NOT change it the way it first appears to.
// The flip turns the normal toward the VIEWER, not toward the light, so a
// surface facing away from the sun still receives nothing; what the flip fixes
// is back-facing geometry, not the sun term. A building is also not a sphere --
// it is mostly vertical with a horizontal roof -- but 0.25 is within a few
// percent of the integral over that distribution too, and pretending to more
// precision than that would be inventing it.
constexpr float kMeanNdotL = 0.25f;

// ⚠️ AND THE SHADOW TERM IS THE OTHER HALF. Roughly a third of the sunlit half
// of an ordinary massing is in its own shadow. This is the one factor here with
// no measurement behind it at all, which is why it is a named constant rather
// than folded into kMeanNdotL: it is the first thing to correct when the probe
// reports the estimate against a real render.
constexpr float kMeanShadow = 0.65f;

} // namespace

float MeanSceneLuminance (const SceneLight& light)
{
    float ambient[3] = { 0.0f, 0.0f, 0.0f };
    float directWeight = 0.0f;

    if (light.environmentActive) {
        // The SH already carries the sky's measured magnitude and is used at
        // full strength; the sun is held back to its slider's share. This
        // mirrors kArchVizMeshPSMain exactly -- if that branch changes, so must
        // this one, or the exposure will be computed for a different scene from
        // the one being drawn.
        for (int c = 0; c < 3; ++c)
            ambient[c] = light.skyIrradiance[c];
        directWeight = std::clamp (light.sunWeight, 0.0f, 1.0f);
    } else {
        // The two-colour hemispheric ambient, averaged over normals: the lerp
        // parameter saturate(n.z * 0.5 + 0.5) has mean 0.5 over a sphere, so the
        // mean ambient colour is the midpoint of the two.
        const float share = std::clamp (light.ambientShare, 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c)
            ambient[c] = share * 0.5f * (light.ambientSky[c] + light.ambientGround[c]);
        directWeight = 1.0f - share;
    }

    const float direct = directWeight * kMeanNdotL * kMeanShadow * std::max (light.sunStrength, 0.0f);
    const float lighting = Luminance (ambient) + direct;

    // ⚠️ ALBEDO MULTIPLIES, IT DOES NOT ADD. The specular share is deliberately
    // left out: it is a small fraction of an architectural image's energy and it
    // is exactly the part a tone curve is there to roll off. Including it would
    // pull the exposure down whenever a glass facade came into view, which is
    // the "auto-exposure hunts" complaint in a different costume.
    return std::max (light.meanAlbedo, 0.0f) * std::max (lighting, 0.0f);
}

float AutoExposure (const SceneLight& light)
{
    const float luminance = MeanSceneLuminance (light);
    // ⚠️ THE GUARD IS BEFORE THE DIVISION, NOT AFTER IT. A scene with no light
    // loaded yet has luminance 0, and 0.18/0 is an infinity that a clamp on the
    // RESULT would still have to special-case for NaN.
    if (!(luminance > 1e-6f))
        return kMaxAutoExposure;
    return std::clamp (kMiddleGrey / luminance, kMinAutoExposure, kMaxAutoExposure);
}

// ---------------------------------------------------------------------------

WhiteBalanceGains ComputeWhiteBalance (float kelvin, float tint)
{
    WhiteBalanceGains gains;

    // ⚠️ THE EXACT IDENTITY AT D65 IS A CONTRACT, NOT A LIMIT OF THE FIT. The
    // approximations below are good to a percent or so, and a percent of tint on
    // a neutral render is precisely the kind of thing that gets mistaken for a
    // colour-space bug six weeks later. So the neutral case returns early.
    const bool neutralTemperature = std::abs (kelvin - kNeutralKelvin) < 1.0f;
    const bool neutralTint = std::abs (tint) < 1e-4f;
    if (neutralTemperature && neutralTint)
        return gains;

    // Kim et al.'s cubic fit to the Planckian locus in CIE 1931 xy, which is the
    // standard closed form and is valid over 1667 K to 25000 K. Clamped rather
    // than extrapolated: outside that range the cubic diverges quickly and
    // produces gains that look like a corrupted image.
    const double t = std::clamp (double (kelvin), 1667.0, 25000.0);
    const double t2 = t * t;
    const double t3 = t2 * t;

    double x = 0.0;
    if (t < 4000.0)
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    else
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;

    double y = 0.0;
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (t < 2222.0)
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    else if (t < 4000.0)
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;

    // ⚠️ THE TINT AXIS IS APPLIED IN xy, BEFORE THE CONVERSION, because it is a
    // displacement PERPENDICULAR to the locus -- a green/magenta shift is not a
    // temperature and cannot be expressed as one. Applying it to the finished
    // RGB gains instead would couple the two controls.
    y += double (std::clamp (tint, -1.0f, 1.0f)) * 0.05;

    if (!(y > 1e-6))
        return gains;

    // xyY (Y = 1) -> XYZ -> linear sRGB.
    const double xyzX = x / y;
    const double xyzY = 1.0;
    const double xyzZ = (1.0 - x - y) / y;

    const double r = 3.2404542 * xyzX - 1.5371385 * xyzY - 0.4985314 * xyzZ;
    const double g = -0.9692660 * xyzX + 1.8760108 * xyzY + 0.0415560 * xyzZ;
    const double b = 0.0556434 * xyzX - 0.2040259 * xyzY + 1.0572252 * xyzZ;

    // ⚠️ THE GAIN IS THE RECIPROCAL OF THE ILLUMINANT, WHICH IS THE WHOLE
    // MEANING OF WHITE BALANCE: dividing the image by the light's colour is what
    // makes a surface that reflects everything come out neutral. Multiplying by
    // it instead would double the cast, and it looks plausible enough on a
    // screenshot to survive review.
    const double illuminant[3] = { std::max (r, 1e-4), std::max (g, 1e-4), std::max (b, 1e-4) };

    // Normalised on GREEN so the correction changes the image's colour without
    // changing its brightness -- otherwise every temperature change would also
    // be an exposure change, and the two controls would fight.
    // ⚠️ NORMALISED ON GREEN *BEFORE* THE CLAMP, so the clamp bites on the
    // channel that is actually out of range rather than on all three at once.
    // See kMaxWhiteBalanceGain for why the clamp exists at all.
    for (int c = 0; c < 3; ++c)
        gains.rgb[c] = std::clamp (float (illuminant[1] / illuminant[c]),
                                   kMinWhiteBalanceGain, kMaxWhiteBalanceGain);
    return gains;
}

float MeanPoolAlbedo (const MaterialTable& pool)
{
    double sum = 0.0;
    int counted = 0;
    for (const SurfaceMaterial& surface : pool.All ()) {
        if (surface.alpha <= 0.0f)
            continue;
        const float rgb[3] = { surface.r, surface.g, surface.b };
        sum += Luminance (rgb);
        ++counted;
    }
    // ⚠️ THE FALLBACK IS MIDDLE GREY, WHICH MAKES AN EMPTY POOL A NO-OP RATHER
    // THAN A BLACK ONE. A pool of zero surfaces is the ordinary state of the
    // first frames after the viewer opens, and returning 0 there would drive the
    // exposure to its maximum and flash the model white as the model arrived.
    return counted > 0 ? float (sum / counted) : kMiddleGrey;
}

} // namespace archviz
} // namespace geomsrv
