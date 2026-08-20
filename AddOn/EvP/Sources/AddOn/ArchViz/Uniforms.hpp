#ifndef EVP_ARCHVIZ_UNIFORMS_HPP
#define EVP_ARCHVIZ_UNIFORMS_HPP

// The C++ half of Shaders/uniforms.sh.
//
// ⚠️ THE TWO FILES ARE ONE ABI, and the FIELD ORDER is the contract. bgfx
// uploads uniforms as an array of vec4s and the shader indexes into it by
// position, so inserting a field in the middle silently reinterprets every field
// after it — geometry that lights wrongly rather than a compile error. APPEND
// ONLY, and change both files in the same commit.
//
// One packed array rather than several scalar uniforms, following bgfx's 18-ibl
// example: it is one setUniform per frame instead of several, and there is one
// place to look for what the shader can see (plan §6.7).

#include <cstdint>

namespace geomsrv {
namespace archviz {

// Must equal the name in uniforms.sh, exactly. bgfx matches uniforms by string.
constexpr const char* kArchVizParamsName  = "u_archvizParams";
constexpr uint16_t    kArchVizParamsCount = 3;   // vec4s

struct ArchVizParams {
    // [0].xyz — direction TOWARD the sun, world space, Z-up, NORMALISED.
    //           Z-up because ArchViz converts nothing (plan §6.4). Normalised
    //           here because the shader does not: normalising a per-frame
    //           constant per fragment is waste.
    //           Phase 6 replaces the placeholder with Archicad's own
    //           sunAngXY/sunAngZ — do NOT implement a solar position model
    //           (plan §3).
    float sunDirX = 0.0f, sunDirY = 0.0f, sunDirZ = 1.0f;
    // [0].w   — ambient floor, 0..1: what a surface facing away still receives.
    float ambient = 0.35f;

    // [1].rgb — the surface colour of the draw range being submitted, 0..1,
    //           from the model's material pool (ArchViz/MaterialTable.hpp).
    //           WHITE means "no material", and it is multiplied with the vertex
    //           colour rather than replacing it so the debug cube's six painted
    //           faces and real geometry can share one shader.
    // [1].a   — opacity, 1 = opaque. Already flipped from ModelerAPI's
    //           transparency by the producer.
    //
    // ⚠️ SET PER SUBMIT, NOT PER FRAME, unlike everything in [0]. That is why
    // SceneCache calls setUniform inside its per-range loop: bgfx captures
    // uniform values at submit time, so one call before the loop would paint
    // every range in the last material's colour — the exact failure MeshGroups
    // exists to prevent, reintroduced one layer up.
    float baseR = 1.0f, baseG = 1.0f, baseB = 1.0f, baseA = 1.0f;

    // [2].xyz — the CAMERA's world position.
    //
    // ⚠️ WITHOUT THIS THE SUN HAS NO DIRECTION. Two-sided lighting used
    // `abs(dot(n, l))`, which lights a north wall exactly as brightly as a south
    // one — the building reads flat and its lit side disagrees with Archicad's
    // own 3D window, which is precisely what the 2026-08-07 run reported. The
    // fragment shader now flips the normal toward the eye instead, and that
    // needs the eye. [2].w is spare.
    float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f, eyeSpare = 0.0f;
};

static_assert (sizeof (ArchVizParams) == 16 * kArchVizParamsCount,
               "ArchVizParams must be exactly the vec4 array uniforms.sh declares");

}   // namespace archviz
}   // namespace geomsrv

#endif
