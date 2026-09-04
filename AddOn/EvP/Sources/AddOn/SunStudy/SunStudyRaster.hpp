#ifndef EVP_SUNSTUDY_SUNSTUDYRASTER_HPP
#define EVP_SUNSTUDY_SUNSTUDYRASTER_HPP

// SunStudy/SunStudyRaster — the shadow camera's geometry, as pure arithmetic.
//
// Frustum fit per sun direction, the light view-projection, texel world size,
// bias derivation, the RTC origin, and the accumulator's texture layout. No
// Diligent, no ACAPI, no GPU: every function here takes numbers and returns
// numbers, so the whole file is exercised offline by tests/cpp.
//
// It is a transcription of private/Commands/SunStudy/sunraster.py, which the
// Python suite already pins. Where the two disagree the Python is the oracle
// until test_sunstudyraster.cpp says otherwise — see the parity vectors there.
//
// ⚠️ THREE THINGS HERE PRODUCE A PLAUSIBLE WRONG PICTURE IF LOST, which is why
// each carries its own warning below rather than a line of prose: the negative
// near plane, the RTC all-or-nothing rule, and the conservative bias.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace evp::sunstudy {

using Vec3 = std::array<double, 3>;

// Row-major. The same 16 numbers sunraster.light_vp_matrix returns after its
// transpose, so a graphics API wanting column-major floats takes them as they
// come.
using Mat4 = std::array<double, 16>;

// ---------------------------------------------------------------------------
// frustum fitting
// ---------------------------------------------------------------------------

struct OrthoFrustum {
    double left = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double top = 0.0;

    // ⚠️ `near` MAY BE NEGATIVE AND MUST NOT BE CLAMPED TO A POSITIVE FLOOR.
    // The view matrix is a pure rotation because a directional light has no eye
    // point, so the scene straddles the origin along the light axis and roughly
    // half its corners project to a negative distance. Clamping near to a small
    // positive value clips every occluder on the SUNWARD side out of the depth
    // pass, and the study then reports samples lit that the ray-cast engine
    // correctly calls shadowed. An orthographic projection accepts a negative
    // near without complaint; only the span matters.
    // ⚠️ NOT `near` AND `far`: BOTH ARE MACROS IN windows.h, and renaming them
    // back would break the add-on while every offline test kept passing. The
    // test build never includes windows.h; APIEnvir.h does, so the collision
    // appears only in the .apx compile and looks like a syntax error inside this
    // header rather than a name clash from three includes away.
    double nearPlane = 0.0;
    double farPlane = 0.0;

    double worldWidth = 0.0;
    double worldHeight = 0.0;
    double worldDepth = 0.0;

    Vec3 rightAxis { 1.0, 0.0, 0.0 };
    Vec3 upAxis { 0.0, 1.0, 0.0 };

    // Points FROM the sun TOWARD the scene: the direction light travels.
    Vec3 lightAxis { 0.0, 0.0, -1.0 };

    bool valid = false;
};

// Fit an orthographic frustum around an AABB for a given sun direction.
//
// `sunDir` points TO the sun, matching GetPlaceInfo's sunDirX/Y/Z. `margin` pads
// every edge, in metres. Returns `valid == false` for a zero-length sunDir or an
// inverted box rather than throwing — a degenerate timestep is skipped, not
// fatal.
OrthoFrustum FitOrthoFrustum (const Vec3& aabbMin, const Vec3& aabbMax, const Vec3& sunDir, double margin = 1.0);

// ---------------------------------------------------------------------------
// the shadow camera's matrices
// ---------------------------------------------------------------------------

Mat4 OrthoProjection (const OrthoFrustum& frustum);

// The RELATIVE-TO-CENTER origin every position handed to a float32 consumer is
// shifted by.
//
// ⚠️ A SURVEY COORDINATE DOES NOT SURVIVE float32. Roughly seven significant
// digits means consecutive representable values are ~0.06 m apart at 600,000 m
// east, which is an ordinary national-grid easting in Europe. The shadow compare
// then works on depths quantised to a grid coarser than the bias, and the result
// is stripes across whole facades that no bias tuning can fix, because the
// information was destroyed before the GPU saw it. At the model's own centre the
// same float32 resolves to well under a millimetre.
//
// ⚠️ APPLY IT TO EVERYTHING OR TO NOTHING — occluder vertices, sample positions,
// the camera, AND the light view-projection (LightViewProjection folds it in).
// Shift the geometry but not the matrix and every shadow lands in the wrong
// place, at full confidence.
//
// Rounded to whole metres so it is stable: the same project shifts by the same
// vector on every run, which keeps two dumps comparable.
Vec3 RtcOrigin (const Vec3& aabbMin, const Vec3& aabbMax);

// View-projection for the shadow camera.
//
// `origin` is the RTC shift. The returned matrix consumes positions with
// `origin` ALREADY SUBTRACTED, and is exactly equivalent to the unshifted matrix
// on unshifted points:
//
//     Vp(f, o) * (p - o, 1)  ==  Vp(f, nullptr) * (p, 1)
//
// Pass nullptr for no shift.
Mat4 LightViewProjection (const OrthoFrustum& frustum, const Vec3* origin = nullptr);

// p' = m * (p, 1), perspective divide included. For tests, and for CPU-side
// checks of what the GPU is about to do.
Vec3 TransformPoint (const Mat4& m, const Vec3& p);

// ---------------------------------------------------------------------------
// texel world size and bias
// ---------------------------------------------------------------------------

constexpr uint32_t kDefaultShadowMapResolution = 2048;
constexpr uint32_t kMaxTextureSize = 16384;
constexpr double kMinBiasMetres = 0.005;

// Metres per shadow-map texel. Uses the LARGER of width and height so the answer
// is conservative on a non-square frustum.
double TexelWorldSize (const OrthoFrustum& frustum, uint32_t shadowMapResolution = kDefaultShadowMapResolution);

// ⚠️ CONSERVATIVE ON PURPOSE: bias = max(2 x texel, floor). Too small stripes the
// surface with self-shadow acne; too large detaches contact shadows. Acne is the
// worse failure for an ANALYSIS, because it reads as a real result rather than
// as a rendering artefact.
double DeriveBias (double texelWorldSize, double minBias = kMinBiasMetres);

double DeriveBiasFromFrustum (const OrthoFrustum& frustum, uint32_t shadowMapResolution = kDefaultShadowMapResolution,
                              double minBias = kMinBiasMetres);

// World metres to the depth buffer's [0, 1] units. An orthographic projection is
// linear in depth, so one metre along the light axis is 1 / (far - near) of the
// range. Clamped to 0.5 so a degenerate frustum cannot make everything
// unconditionally lit.
double DepthBiasNdc (double biasMetres, double nearPlane, double farPlane);

// The largest bias across a sequence of per-timestep frustums: ONE bias for the
// whole study, so a sample cannot change classification with the hour for a
// reason that is not the sun.
double PickBias (const std::vector<OrthoFrustum>& frustums, uint32_t shadowMapResolution = kDefaultShadowMapResolution,
                 double minBias = kMinBiasMetres);

// ---------------------------------------------------------------------------
// accumulator layout
// ---------------------------------------------------------------------------

struct AccumLayout {
    uint32_t width = 1;
    uint32_t height = 1;
    bool valid = false;
};

// Roughly square, capped at `maxWidth` per side. `valid == false` when the
// sample count exceeds maxWidth^2 — refused rather than silently truncated,
// because a truncated accumulator drops samples with no visible symptom.
AccumLayout ComputeAccumLayout (size_t sampleCount, uint32_t maxWidth = kMaxTextureSize);

// Linear sample index to its texel.
void SampleIndexToTexel (size_t sampleIndex, uint32_t accumWidth, uint32_t& column, uint32_t& row);

// ---------------------------------------------------------------------------
// the ground sample grid
// ---------------------------------------------------------------------------

// How far the analysis ground plane must reach beyond the model.
//
// ⚠️ THE STUDY IS MEANINGLESS WITHOUT THIS. Sampling the model's own footprint
// puts every sample directly underneath a building, so the engine correctly
// reports about zero sun hours everywhere and the run says nothing. A sun study
// measures the ground AROUND the massing -- that is where the shadows land.
//
// The rule: reach out by the model's height (a shadow is as long as its caster
// is tall at 45 degrees of altitude, and longer below that), but never less than
// a quarter of the footprint and never less than `minPad`.
double AutoGroundPad (const Vec3& aabbMin, const Vec3& aabbMax, double minPad = 2.0);

struct GroundGrid {
    // xyz interleaved, ready for SampleSet::positions.
    std::vector<double> positions;

    // All +Z: a horizontal plane. Supplied rather than left null so the
    // back-face cull still retires a sun below the horizon.
    std::vector<double> normals;

    double pad = 0.0;
    double groundZ = 0.0;
    size_t columns = 0;
    size_t rows = 0;
    bool valid = false;
};

// A REGULAR grid over the padded footprint.
//
// ⚠️ REGULAR, WHERE THE PYTHON ORACLE IS RANDOM, AND THAT IS A DELIBERATE
// DIVERGENCE rather than an incomplete port. The Python version scatters points
// with a seeded generator so its two engines could be compared on an identical
// set; reproducing one language's generator in another is fragile and buys
// nothing here. A regular grid is deterministic without a seed, covers the
// footprint evenly instead of clumping, and makes the sample count a function of
// spacing rather than a parameter. The test file records the consequence: totals
// from the two samplers agree in distribution, not sample for sample.
//
// `zOffset` lifts each sample off the ground so a ray does not start exactly on
// the surface it stands on.
//
// `maxSamples` refuses rather than truncates: a silently shortened grid analyses
// part of the site and looks like a complete answer.
GroundGrid MakeGroundSampleGrid (const Vec3& aabbMin, const Vec3& aabbMax, double spacing, double pad = -1.0,
                                 double zOffset = 0.10, size_t maxSamples = 4000000);

// ---------------------------------------------------------------------------
// progressive refinement
// ---------------------------------------------------------------------------

// Coarse-to-fine analysis grids, coarsest first, always ending at `targetGrid`.
//
// ⚠️ REFINE BY SAMPLE DENSITY, NEVER BY TIMESTEP. Every rung is a COMPLETE,
// CORRECT study at its own resolution — a coarse one is simply blockier. A
// partial set of timesteps is instead a WRONG NUMBER OF HOURS, and a user reads
// whatever is on screen as though it were right.
//
// Rungs are added only when the full run is big enough to be worth waiting
// through; on a small model the coarse pass finishes in the same breath and all
// it buys is a flicker.
std::vector<double> GridLadder (double areaM2, double targetGrid, double coarseThreshold = 20000.0,
                                double midThreshold = 5000.0);

} // namespace evp::sunstudy

#endif
