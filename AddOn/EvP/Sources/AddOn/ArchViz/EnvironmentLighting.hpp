#ifndef EVP_ARCHVIZ_ENVIRONMENTLIGHTING_HPP
#define EVP_ARCHVIZ_ENVIRONMENTLIGHTING_HPP

// ArchViz/EnvironmentLighting — an HDR sky, reduced to what a shader can afford.
//
// WHY IT EXISTS. The forward pass lights ambient from TWO COLOURS: a sky above
// and a ground below, lerped on the normal's z (DiligentShaders.hpp). That is
// enough to make soffits and reveals read, and it is the reason the building
// stopped looking flat -- but it is still a two-colour world. Every surface
// facing north gets the same light as every surface facing south, a courtyard is
// lit like an open field, and nothing in the picture knows there is a sun on one
// side of the sky and a horizon on the other. A real environment map is the
// single largest remaining step toward a photographic image, and it costs one
// texture.
//
// ⚠️ THIS FILE IS THE CPU HALF AND IT DELIBERATELY KNOWS NOTHING ABOUT DILIGENT.
// No device, no texture, no D3D -- the same rule MeshGroups, MaterialTable and
// SunShadowMath are held to, and for the same reason: every way of getting
// spherical harmonics wrong produces a PICTURE rather than an error. A factor of
// pi in the wrong place is a building lit three times too brightly, which reads
// as "the new environment code is too strong" and gets TUNED rather than fixed.
// Keeping it here means tests/cpp can pin the arithmetic down against a constant
// sky, where the right answer is known exactly.
//
// THE TWO OUTPUTS, and why the split:
//
//   DIFFUSE  -> nine spherical-harmonic coefficients. Irradiance over a
//               hemisphere is so smooth that an order-2 SH reconstructs it to
//               within about 1% (Ramamoorthi & Hanrahan 2001), so the entire
//               diffuse contribution of a 2048x1024 sky becomes NINE RGB
//               CONSTANTS in the cbuffer. No cubemap, no convolution pass, no
//               second texture fetch.
//
//   SPECULAR -> the equirectangular image itself, mip-mapped, sampled along the
//               reflected view direction with the mip chosen by roughness.
//               ⚠️ THIS IS NOT A PROPERLY PREFILTERED GGX ENVIRONMENT. A real
//               one convolves each mip with the GGX lobe for that roughness;
//               this borrows the box-filtered mip chain, which is the wrong
//               kernel and blurs across the equirect's distorted poles. It is a
//               deliberate approximation -- honest for architectural surfaces,
//               which are mostly rough, and wrong for a mirror. Replacing it is
//               the job of DiligentFX's PBR_Renderer once the G-Buffer lands.

#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {

// An equirectangular (latitude-longitude) HDR image, linear radiance, 3 floats
// per texel. ⚠️ ROW 0 IS THE +Z POLE and rows advance toward -Z; column 0 is
// phi = 0, advancing counter-clockwise. That is the convention `DirectionAt`
// implements and the shader's own lookup must match it exactly -- a mismatch
// mirrors the sky, which on a photograph of a road and a hedge is remarkably
// hard to notice and puts the sun's reflection on the wrong side of every window.
struct EquirectImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgb;   // width * height * 3, linear

    bool IsValid () const { return width > 0 && height > 0 && rgb.size () == size_t (width) * height * 3; }
};

// Order-2 spherical harmonics: 9 coefficients, RGB each.
//
// ⚠️ THE IRRADIANCE CONVOLUTION AND THE 1/pi ARE ALREADY FOLDED IN, so
// `EvaluateDiffuse` returns the number that multiplies albedo DIRECTLY. This is
// the whole reason the struct exists rather than a bare float[27]: the pi
// bookkeeping in image-based lighting is where this goes wrong, and burying it
// in one place with one test (a constant sky must round-trip to itself) is the
// only way it stays right.
struct ShIrradiance {
    float c[9][3] = {};
};

// The world-space direction a texel's centre looks along. Z-up, matching
// Archicad and everything else in ArchViz.
void DirectionAt (uint32_t x, uint32_t y, uint32_t width, uint32_t height, float outDir[3]);

// Project an equirect sky onto order-2 SH, convolved for Lambertian irradiance.
//
// ⚠️ THE SOLID-ANGLE WEIGHT IS NOT OPTIONAL. Equirect rows near the poles cover
// far less of the sphere than rows at the equator; summing texels unweighted
// makes the poles roughly 300 times too influential on a 1024-row image, which
// on an outdoor sky means the (usually bright, usually empty) zenith swamps
// everything and the model is lit flat from above -- the exact look this file
// exists to remove.
ShIrradiance ProjectIrradiance (const EquirectImage& image);

// The diffuse light arriving at a surface with world normal `n`. Multiply by
// albedo. `n` need not be normalised.
//
// ⚠️ IT CAN GO NEGATIVE, and the caller must clamp. Order-2 SH is a smooth
// approximation, and a sky with a very bright, very small sun rings -- the
// reconstruction dips below zero on the side facing away from it. Clamping is
// correct; "fixing" it by adding a floor changes the average brightness.
void EvaluateDiffuse (const ShIrradiance& sh, const float n[3], float outRgb[3]);

// Box-filter an equirect down to `width` x `height`.
//
// ⚠️ IT EXISTS FOR A BINDING CONSTRAINT, NOT FOR SPEED. The scene's shader
// resources are bound as STATIC variables at pipeline creation, which the shadow
// map gets away with because its texture is created once and only its CONTENTS
// change (DiligentScene.cpp). An environment texture whose size followed
// whatever HDR the user loaded would have to be RECREATED on every load, and a
// recreated texture is a new object that the already-built SRBs do not point at.
// Resampling every sky to one fixed size makes the environment behave exactly
// like the shadow map -- allocate once, refill on load -- which is the pattern
// this codebase has already proven.
//
// Returns an invalid image when the source is invalid or the target is empty.
EquirectImage Resample (const EquirectImage& source, uint32_t width, uint32_t height);

// The average linear radiance over the sphere, solid-angle weighted. Used to
// report what was loaded -- a sky whose mean is 0 loaded but is black, which is
// otherwise indistinguishable from a binding that silently failed.
void AverageRadiance (const EquirectImage& image, float outRgb[3]);

}   // namespace archviz
}   // namespace geomsrv

#endif
