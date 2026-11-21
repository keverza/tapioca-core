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

// #14: the depth range the frame actually shows, for the cascades (sample-distribution shadow maps).
// `nearNdc`/`farNdc` are the nearest and farthest depth-buffer values on screen (DiligentDepthRange,
// read back a frame or two late), `projection` the row-major camera projection they came through.
// Writes positive view depths, widened by `margin` (near / margin, far * margin) for the frames the
// readback lags and for geometry the camera turns towards, and quantised to quarter octaves like
// FitShadowDepthRange, so a still camera keeps exactly the same cascades.
//
// ⚠️ WHY: the scene box cannot narrow the near end when the camera stands inside it -- an urban
// model's always contains the camera -- so the split started at the 5 cm near plane. Live
// (2026-11-21, 806k-triangle project): cascades 0.05-1.7, 1.7-4.9, 4.9-19 and 19-110 m, texels
// 0.19, 0.56, 2.2 and 12 cm; every building 20-110 m away shared the last one, and its 12 cm
// texels drew the stair-stepped shadows that showed wherever that cascade began on screen.
//
// Returns false, leaving the range untouched, when the values are empty (nothing drawn) or invalid.
bool VisibleShadowDepthRange (const float projection[16], float nearNdc, float farNdc, float margin, float& nearDepth,
                              float& farDepth);

} // namespace archviz
} // namespace geomsrv

#endif
