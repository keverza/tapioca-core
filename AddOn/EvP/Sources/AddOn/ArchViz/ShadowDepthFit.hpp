// ArchViz/ShadowDepthFit -- the depth range the sun's shadow cascades split (#234).
//
// Pure, with no Diligent dependency, so the offline suite pins it (test_shadow_depth_fit.cpp).
// DiligentShadowMap hands it to DiligentFX's ShadowMapManager through AdjustCascadeRange.

#ifndef ARCHVIZ_SHADOWDEPTHFIT_HPP
#define ARCHVIZ_SHADOWDEPTHFIT_HPP

namespace geomsrv {
namespace archviz {

// Narrows the projection's [nearDepth, farDepth] to the part a box occupies in front of the camera.
// `view` is the row-major right-handed camera view (camera looks down -Z, row-vector order as
// MatrixMath.hpp); depths are positive metres along the view direction.
//
// ⚠️ THE PROJECTION'S RANGE IS 0.05..20,000 m IN PERSPECTIVE (Camera::FarClip), and the cascades
// were split across all of it: with four, a room 3-5 m from the eye fell in a cascade about 30 m
// deep, some 1.5 cm per texel at 2048. Every shadow offset (fixed bias, normal offset, the filter
// floor) is sized in texels, so each leaked about that far into fully occluded corners and no
// bias setting could close it. Fitted, the same room gets millimetre texels.
//
// ⚠️ QUANTISED TO QUARTER OCTAVES (near down, far up). A range that tracks the camera continuously
// re-grids every cascade on every frame of an orbit and the shadow edges shimmer; steps of 2^(1/4)
// change it rarely, at the cost of at most 19 % of the range.
//
// Returns false, leaving the range untouched, when the box lies entirely behind the near plane or
// does not narrow the range.
bool FitShadowDepthRange (const float view[16], const float boxMin[3], const float boxMax[3], float& nearDepth,
                          float& farDepth);

} // namespace archviz
} // namespace geomsrv

#endif
