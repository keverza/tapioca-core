#ifndef EVP_ARCHVIZ_DILIGENT_POSTFX_CAMERA_HPP
#define EVP_ARCHVIZ_DILIGENT_POSTFX_CAMERA_HPP

// The one conversion every DiligentFX post-process needs from this renderer:
// the right-handed camera pair, restated in the left-handed convention
// PostProcess/ assumes without ever checking. MatrixMath::ToLeftHandedView and
// ToLeftHandedProjection are the algebra and carry the argument for why the
// view-projection survives it unchanged; this header is the Diligent adapter,
// and this comment is the symptom, so the next reader can recognise it.
//
// ⚠️ WHAT IT LOOKS LIKE WHEN IT IS MISSING. PostFX_Common.fxh reconstructs view
// space with
//
//     ScreenXYDepthToViewSpace (Coord, mProj)
//         -> float3 (z * ndc.x / m00, z * ndc.y / m11, z)
//
// where z = NormalizedDeviceZToCameraZ (...). That algebra recovers the true
// view-space z from EITHER convention -- but under a right-handed projection
// that z is NEGATIVE, so the x and y it scales come out NEGATED. The
// reconstructed point is the true one rotated 180 degrees about the view axis.
// Measured: view-space (1.3, -0.7, -8) reconstructs as (-1.3, 0.7, -8).
//
// SSR then traces every ray from a mirrored origin, so reflections appear,
// badly warped, nowhere near the geometry that should cast them. Worse,
// SSR_ComputeIntersection's thickness rejection is
//
//     1 - smoothstep (0, DepthBufferThickness, Distance / SurfaceVS.z)
//
// and a negative SurfaceVS.z makes that argument negative, smoothstep 0 and the
// confidence 1 -- so EVERY hit is accepted at full strength and nothing is
// culled. ⚠️ SSAO READS THE SAME HELPER and is wrong in the same way, just far
// less visibly: a mirrored sample pattern still looks like ambient occlusion.
//
// ⚠️ THIS DOES NOT MAKE SSR CORRECT UNDER A PARALLEL CAMERA.
// ScreenXYDepthToViewSpace divides by m00 and m11 as though the projection were
// perspective, which holds only when w = z. That is DiligentFX's limit, not
// this conversion's; the plan overlay does not run SSR.

#include "ArchViz/MatrixMath.hpp"

#include <BasicMath.hpp>

namespace geomsrv {
namespace archviz {

inline Diligent::float4x4 PostFxViewMatrix (const float view[16])
{
    float lh[16];
    ToLeftHandedView (lh, view);
    return Diligent::float4x4::MakeMatrix (lh);
}

inline Diligent::float4x4 PostFxProjMatrix (const float proj[16])
{
    float lh[16];
    ToLeftHandedProjection (lh, proj);
    return Diligent::float4x4::MakeMatrix (lh);
}

} // namespace archviz
} // namespace geomsrv

#endif
