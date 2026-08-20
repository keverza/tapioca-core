#ifndef EVP_ARCHVIZ_SUNSHADOWMATH_HPP
#define EVP_ARCHVIZ_SUNSHADOWMATH_HPP

// ArchViz/SunShadowMath — where the sun's shadow camera goes.
//
// WHY A SEPARATE FILE. This is the part of shadow mapping that is arithmetic
// rather than graphics: given the model's bounds and the sun's direction,
// produce the light's view-projection and the two scale numbers the pixel
// shader needs to bias its comparison. Kept free of Diligent so it can be
// tested offline against the AABB it claims to cover -- and it is worth testing,
// because every way of getting it wrong produces a PICTURE rather than an error:
// a frustum that is too small clips the shadow off at a straight edge, one that
// is too large makes it a blur, and one that is inverted in depth shadows
// everything except what should be in shadow.
//
// ⚠️ THE BOUNDING SPHERE, NOT THE AABB'S PROJECTED EXTENT. Fitting the eight
// transformed corners is tighter and it is what a first attempt reaches for, but
// the extent then CHANGES AS THE SUN MOVES, so the world size of a shadow texel
// changes with it and the shadow's edges crawl and shimmer while the sun animates
// -- and the shadow's quality silently depends on the time of day. A sphere is
// rotation-invariant: one texel size, always.

#include <cstdint>

namespace geomsrv {
namespace archviz {

struct SunShadow {
    // False when the scene is empty or the sun vector is degenerate. The caller
    // must then render WITHOUT shadows rather than with a matrix of NaNs, which
    // shadows the entire model to the ambient floor and reads as night.
    bool valid = false;
    float lightViewProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // World metres covered by one shadow-map texel. The pixel shader offsets
    // along the surface normal by a small multiple of this before comparing --
    // the one bias that scales correctly with both model size and map
    // resolution, and it does not need the surface slope.
    float texelWorldSize = 0.0f;
    // World metres spanned by the light's depth range, so a world-space bias can
    // be converted into the 0..1 depth the map stores.
    float depthRange = 0.0f;
};

// `sunDir` points TOWARD the sun, the same convention the shader's
// `g_sunAndAmbient.xyz` uses and the same one `EnvironmentUpload` delivers. It
// need not be normalised. `resolution` is the shadow map's edge in texels.
//
// The frustum covers the whole AABB from the sun's side, with a small margin so
// geometry exactly on the boundary is not clipped by the near plane.
SunShadow FitSunShadow (const float boundsMin[3], const float boundsMax[3],
                        const float sunDir[3], uint32_t resolution);

}   // namespace archviz
}   // namespace geomsrv

#endif
