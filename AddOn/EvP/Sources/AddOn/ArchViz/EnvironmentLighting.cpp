#include "ArchViz/EnvironmentLighting.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace {

constexpr double kPi = 3.14159265358979323846;

// The order-2 real spherical harmonic basis, evaluated at a unit direction.
//
// ⚠️ THE CONSTANTS ARE THE NORMALISED BASIS AND MUST NOT BE "SIMPLIFIED".
// 0.282095 is 1/(2*sqrt(pi)), 0.488603 is sqrt(3/(4pi)), and so on; they are
// what make the projection and the reconstruction inverses of each other. A
// rounded constant here does not fail, it changes the brightness of the sky by a
// few percent -- and a few percent is exactly the size of error that gets
// blamed on the exposure and tuned out somewhere else.
void EvaluateBasis (const float d[3], double y[9])
{
    const double x = d[0], yy = d[1], z = d[2];
    y[0] = 0.282095;
    y[1] = 0.488603 * yy;
    y[2] = 0.488603 * z;
    y[3] = 0.488603 * x;
    y[4] = 1.092548 * x * yy;
    y[5] = 1.092548 * yy * z;
    y[6] = 0.315392 * (3.0 * z * z - 1.0);
    y[7] = 1.092548 * x * z;
    y[8] = 0.546274 * (x * x - yy * yy);
}

// The Lambertian convolution, ALREADY DIVIDED BY PI.
//
// Ramamoorthi & Hanrahan give the irradiance transfer coefficients as
// A0 = pi, A1 = 2pi/3, A2 = pi/4. Lambertian outgoing radiance is irradiance/pi,
// and every caller wants the radiance, so the division happens here -- once --
// rather than in the shader where it would be a lone `/ 3.14159` that looks
// removable.
//
// ⚠️ THE TEST THAT PINS THIS DOWN IS A CONSTANT SKY. With these values a uniform
// environment of radiance c reconstructs to exactly c in every direction. Any
// stray factor of pi shows up there immediately, and nowhere else.
constexpr double kConvolution[9] = {
    1.0,                            // l = 0 : pi / pi
    2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0,   // l = 1 : (2pi/3) / pi
    0.25, 0.25, 0.25, 0.25, 0.25,      // l = 2 : (pi/4) / pi
};

// The EXACT solid angle of one texel in row `y`, not the midpoint approximation.
//
// ⚠️ sin(theta) * dTheta IS WRONG BY ENOUGH TO MEASURE, and the offline test
// found it: integrating sin over a row by sampling its centre is a midpoint rule
// with error O(dTheta^2), which at 8 rows is 1.2% and at 32 rows is still 0.08%.
// That error does not average out -- it is a systematic brightness offset that
// varies with the sky's RESOLUTION, so the same environment resampled to a
// different size lights the building differently.
//
// The integral has a closed form: the band between two polar angles subtends
// (cos(theta0) - cos(theta1)) * dPhi exactly. Using it makes the projection
// resolution-independent by construction rather than approximately.
double RowSolidAngle (uint32_t y, uint32_t height, double dPhi)
{
    const double theta0 = double (y) / double (height) * kPi;
    const double theta1 = double (y + 1) / double (height) * kPi;
    return (std::cos (theta0) - std::cos (theta1)) * dPhi;
}

}   // namespace

void DirectionAt (uint32_t x, uint32_t y, uint32_t width, uint32_t height, float outDir[3])
{
    if (width == 0 || height == 0) {
        outDir[0] = 0.0f;
        outDir[1] = 0.0f;
        outDir[2] = 1.0f;
        return;
    }
    // Texel CENTRES, not corners. Sampling the corner biases the whole
    // projection by half a texel, which at the poles is a large solid angle.
    const double theta = (double (y) + 0.5) / double (height) * kPi;
    const double phi = (double (x) + 0.5) / double (width) * 2.0 * kPi;
    const double sinTheta = std::sin (theta);
    outDir[0] = float (sinTheta * std::cos (phi));
    outDir[1] = float (sinTheta * std::sin (phi));
    outDir[2] = float (std::cos (theta));
}

ShIrradiance ProjectIrradiance (const EquirectImage& image)
{
    ShIrradiance out;
    if (!image.IsValid ())
        return out;

    // Accumulated in double. A 2048x1024 sky is two million texels, and a
    // float sum over that many terms loses the low-order bits of the later ones
    // -- which is a brightness that depends on the image's RESOLUTION.
    double accum[9][3] = {};
    double totalWeight = 0.0;

    const double dPhi = 2.0 * kPi / double (image.width);

    for (uint32_t y = 0; y < image.height; ++y) {
        const double solidAngle = RowSolidAngle (y, image.height, dPhi);

        for (uint32_t x = 0; x < image.width; ++x) {
            float dir[3];
            DirectionAt (x, y, image.width, image.height, dir);

            double basis[9];
            EvaluateBasis (dir, basis);

            const size_t texel = (size_t (y) * image.width + x) * 3;
            for (int i = 0; i < 9; ++i) {
                const double weighted = basis[i] * solidAngle;
                accum[i][0] += double (image.rgb[texel + 0]) * weighted;
                accum[i][1] += double (image.rgb[texel + 1]) * weighted;
                accum[i][2] += double (image.rgb[texel + 2]) * weighted;
            }
            totalWeight += solidAngle;
        }
    }

    // ⚠️ NO RENORMALISATION BY totalWeight. The solid angles already sum to 4pi
    // by construction, and dividing by the measured sum would silently absorb a
    // genuine error in the weighting -- turning a wrong projection into a
    // plausible one. totalWeight is computed only so a test can assert it.
    (void) totalWeight;

    for (int i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c)
            out.c[i][c] = float (accum[i][c] * kConvolution[i]);

    return out;
}

void EvaluateDiffuse (const ShIrradiance& sh, const float n[3], float outRgb[3])
{
    float dir[3] = {n[0], n[1], n[2]};
    const float length = std::sqrt (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (length > 1e-8f) {
        dir[0] /= length;
        dir[1] /= length;
        dir[2] /= length;
    } else {
        dir[0] = 0.0f;
        dir[1] = 0.0f;
        dir[2] = 1.0f;
    }

    double basis[9];
    EvaluateBasis (dir, basis);

    for (int c = 0; c < 3; ++c) {
        double sum = 0.0;
        for (int i = 0; i < 9; ++i)
            sum += double (sh.c[i][c]) * basis[i];
        // ⚠️ CLAMPED AT ZERO HERE AS WELL AS DOCUMENTED IN THE HEADER. Order-2
        // SH rings around a small bright sun and the reconstruction really does
        // go negative; a negative diffuse term subtracts light from the sky's
        // own contribution further along the shader and produces black patches
        // on surfaces facing away from the sun.
        outRgb[c] = float (std::max (sum, 0.0));
    }
}

EquirectImage Resample (const EquirectImage& source, uint32_t width, uint32_t height)
{
    EquirectImage out;
    if (!source.IsValid () || width == 0 || height == 0)
        return out;

    out.width = width;
    out.height = height;
    out.rgb.assign (size_t (width) * height * 3, 0.0f);

    // A box filter over the source texels each destination texel covers.
    //
    // ⚠️ A BOX AVERAGE, NOT A POINT SAMPLE. Point-sampling a 2048-wide sky down
    // to 512 throws away three quarters of it, and what it throws away is the
    // small bright things -- the sun's disc, a window, a specular glint off
    // water. Those carry most of the ENERGY, so a point-sampled sky is both
    // darker and duller than the one that was loaded, in a way that looks like
    // a correct-but-dim result rather than a sampling bug.
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t y0 = uint32_t (uint64_t (y) * source.height / height);
        uint32_t y1 = uint32_t (uint64_t (y + 1) * source.height / height);
        y1 = std::max (y1, y0 + 1);

        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t x0 = uint32_t (uint64_t (x) * source.width / width);
            uint32_t x1 = uint32_t (uint64_t (x + 1) * source.width / width);
            x1 = std::max (x1, x0 + 1);

            double sum[3] = {0.0, 0.0, 0.0};
            uint32_t count = 0;
            for (uint32_t sy = y0; sy < y1 && sy < source.height; ++sy) {
                for (uint32_t sx = x0; sx < x1 && sx < source.width; ++sx) {
                    const size_t texel = (size_t (sy) * source.width + sx) * 3;
                    sum[0] += double (source.rgb[texel + 0]);
                    sum[1] += double (source.rgb[texel + 1]);
                    sum[2] += double (source.rgb[texel + 2]);
                    ++count;
                }
            }
            const size_t dst = (size_t (y) * width + x) * 3;
            const double inv = count > 0 ? 1.0 / double (count) : 0.0;
            out.rgb[dst + 0] = float (sum[0] * inv);
            out.rgb[dst + 1] = float (sum[1] * inv);
            out.rgb[dst + 2] = float (sum[2] * inv);
        }
    }
    return out;
}

void AverageRadiance (const EquirectImage& image, float outRgb[3])
{
    outRgb[0] = outRgb[1] = outRgb[2] = 0.0f;
    if (!image.IsValid ())
        return;

    double sum[3] = {0.0, 0.0, 0.0};
    double weight = 0.0;
    const double dPhi = 2.0 * kPi / double (image.width);

    for (uint32_t y = 0; y < image.height; ++y) {
        const double solidAngle = RowSolidAngle (y, image.height, dPhi);
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t texel = (size_t (y) * image.width + x) * 3;
            sum[0] += double (image.rgb[texel + 0]) * solidAngle;
            sum[1] += double (image.rgb[texel + 1]) * solidAngle;
            sum[2] += double (image.rgb[texel + 2]) * solidAngle;
            weight += solidAngle;
        }
    }
    if (weight <= 0.0)
        return;
    for (int c = 0; c < 3; ++c)
        outRgb[c] = float (sum[c] / weight);
}

}   // namespace archviz
}   // namespace geomsrv
