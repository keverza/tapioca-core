#ifndef EVP_SUNSTUDY_SUNSTUDYRASTER_HPP
#define EVP_SUNSTUDY_SUNSTUDYRASTER_HPP

// SunStudy/SunStudyRaster -- the shadow camera's and the ground grid's value
// types. The arithmetic (frustum fit, light view-projection, texel size, bias,
// RTC origin, accumulator layout, ground grid, grid ladder) runs in the Rust
// core (tapioca_sunstudy::raster), reached through rust::FitShadowCamera,
// rust::ComputeAccumLayout, rust::GridLadder and kernels::MakeGroundSampleGrid.
// Its warnings (negative near plane, RTC all-or-nothing, conservative bias)
// live with that code.

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

// ---------------------------------------------------------------------------
// texel world size and bias
// ---------------------------------------------------------------------------

constexpr uint32_t kDefaultShadowMapResolution = 2048;
constexpr uint32_t kMaxTextureSize = 16384;
constexpr double kMinBiasMetres = 0.005;

// ---------------------------------------------------------------------------
// accumulator layout
// ---------------------------------------------------------------------------

struct AccumLayout {
    uint32_t width = 1;
    uint32_t height = 1;
    bool valid = false;
};

// ---------------------------------------------------------------------------
// the ground sample grid
// ---------------------------------------------------------------------------

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

} // namespace evp::sunstudy

#endif
