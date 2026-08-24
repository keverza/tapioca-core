#ifndef EVP_ARCHVIZ_MATRIXMATH_HPP
#define EVP_ARCHVIZ_MATRIXMATH_HPP

// ArchViz/MatrixMath — the view and projection matrices, without bx.
//
// WHY THIS FILE EXISTS. `Camera.cpp` used `bx::mtxLookAt` and `bx::mtxProj`,
// and bx's headers cannot survive the .apx's `/Zc:wchar_t-` — which is the
// entire reason `cmake/Bgfx.cmake` has the `archviz_render` seam that cuts six
// ArchViz TUs off from `GS::UniString`, DG and every DevKit type. Diligent
// needs no such seam (Probe 1a), so the camera can come back across it as soon
// as its two bx calls do. These are those two calls.
//
// ⚠️ THESE REPRODUCE bx's RIGHT-HANDED, NON-HOMOGENEOUS-NDC MATRICES EXACTLY,
// element for element, and that is a requirement rather than a coincidence. The
// bgfx viewer and the Diligent viewer must frame the model identically or "the
// port changed the picture" and "the port broke the camera" become one symptom.
// tests/cpp/test_matrixmath.cpp checks the elements against the algorithm in
// bx's `src/math.cpp`, worked out by hand.
//
// ⚠️ CONVENTIONS, ALL THREE OF WHICH HAVE SHIPPED WRONG IN THIS REPO ONCE:
//
//   Z-UP. Archicad is Z-up and metres, and ArchViz converts nothing. The up
//   vector is {0,0,1}. The axis swap (x,y,z)->(x,z,y) that looks equivalent is
//   a MIRROR (determinant -1).
//
//   RIGHT-HANDED. Archicad's world is right-handed (X east, Y north, Z up). bx
//   DEFAULTS to left-handed and a LH view+proj pair over a RH world renders the
//   building mirrored — north and south swapped — while a debug cube survives
//   it looking fine. There is no handedness argument here: right-handed is the
//   only thing these functions do.
//
//   ROW-VECTOR, ROW-MAJOR STORAGE, like bx. `out[0..3]` is the first ROW, and a
//   point transforms as `v * M`. Uploaded unchanged into an HLSL `float4x4`
//   (which packs column-major by default) the constant buffer holds the
//   TRANSPOSE, so the shader writes the ordinary `mul(matrix, vector)`. Do not
//   "fix" one side without the other.
//
//   DEPTH 0..1, not -1..1. D3D and Vulkan clip z to [0,1]; OpenGL to [-1,1].
//   bx took this from `bgfx::getCaps()->homogeneousDepth`; Diligent's D3D11
//   backend is 0..1, so it is the only thing offered here. Getting it wrong
//   inverts the depth test over half the scene rather than failing outright.
//
// No Diligent, no bgfx, no DevKit, no DG: this is arithmetic, and it is tested
// as arithmetic.

namespace geomsrv {
namespace archviz {

// The camera's orthonormal basis: `forward` from eye toward target, `right` and
// `up` completing a right-handed frame against world +Z.
//
// ⚠️ ONE DERIVATION, TWO CALLERS, AND THAT IS THE POINT. The pick's Aim and the
// camera's cursor ray both need this frame, and a second copy is exactly the
// "two descriptions of one camera" that Aim's own comment warns about -- they
// agree today and drift the moment one of them gains a roll term.
//
// Degenerate inputs are handled the way LookAtRH handles them: a zero-length
// eye-to-target falls back to +X, and a view parallel to world up falls back to
// a right of +X, because any frame perpendicular to the view will do there.
void CameraBasis (const float eye[3], const float target[3],
                  float forward[3], float right[3], float up[3]);

// view = normalize(eye - at); right = normalize(cross(up, view));
// up' = cross(view, right). When up and the view direction are parallel the
// cross product vanishes and `right` falls back to {1,0,0} — bx does the same,
// and without it a camera looking straight down produces a matrix of NaNs.
void LookAtRH (float out[16], const float eye[3], const float at[3], const float up[3]);

// fovYDegrees is VERTICAL. ⚠️ Archicad's own `viewCone` is HORIZONTAL and in
// degrees; whichever way this camera is later slaved to Archicad's, that
// conversion is not optional.
void PerspectiveRH (float out[16], float fovYDegrees, float aspect,
                    float nearZ, float farZ);

// An orthographic box, right-handed, depth 0..1 — the sun's projection. Same
// conventions as PerspectiveRH in every respect; bx spells it `mtxOrtho` with
// `offset = 0`.
//
// ⚠️ nearZ AND farZ ARE DISTANCES IN FRONT OF THE EYE, POSITIVE, exactly as
// PerspectiveRH's are, even though a right-handed camera looks down -z and the
// geometry therefore lands at view-space z in [-farZ, -nearZ]. Passing the
// signed view-space bounds instead inverts the depth range, and a shadow map
// with inverted depth shadows everything except what should be in shadow.
void OrthographicRH (float out[16], float left, float right, float bottom, float top,
                     float nearZ, float farZ);

// out = a * b, row-vector order: transforming by `out` is transforming by `a`
// and then by `b`. So a view-projection is Multiply (out, view, proj).
// Safe to alias: out may be a or b.
void Multiply (float out[16], const float a[16], const float b[16]);

// Row-vector transform: out = v * m, with v.w supplied and out.w returned. The
// projection makes w meaningful, so it is not dropped.
void TransformPoint (float out[4], const float v[4], const float m[16]);

// ---- the left-handed pair DiligentFX's post-processing insists on -----------
//
// ⚠️ THIS RENDERER IS RIGHT-HANDED AND DILIGENTFX'S PostProcess/ IS NOT. Both
// facts are settled: Camera.hpp argues the right-handed decision at length, and
// DiligentCore spells its own convention out in BasicMath.hpp ("Left-handed
// projection"). CameraAttribs looks like a neutral container and is not -- the
// shaders that read it assume view-space z is POSITIVE in front of the eye, and
// `fHandness`, the field that exists to say otherwise, is read by nothing under
// PostProcess/. So the mismatch compiles, runs, and is silently wrong. See
// DiligentPostFxCamera.hpp for what it does to a reflection.
//
// With S = diag (1, 1, -1, 1),
//
//     view_lh = view * S      proj_lh = S * proj      view_lh * proj_lh
//                                                       == view * proj
//
// because S * S is the identity. ⚠️ THE VIEW-PROJECTION IS UNCHANGED, which is
// the whole reason this belongs at the post-process boundary rather than in the
// camera: the depth buffer these effects read, and the rasterisation that
// produced it, are untouched.
//
// Safe to alias: out may be m.
void ToLeftHandedView (float out[16], const float m[16]);
void ToLeftHandedProjection (float out[16], const float m[16]);

// The projection with TAA's sub-pixel jitter folded in, as an NDC offset.
//
// ⚠️ NOT `m20 += jitterX`, WHICH IS WHAT DILIGENT'S OWN HELPER SAYS AND WHAT
// THIS RENDERER CANNOT COPY. TemporalAntiAliasing::GetJitteredProjMatrix is
// written for Diligent's left-handed projection, where w = +z and the shift the
// pixel sees is therefore +jitter. Under PerspectiveRH, w = -z, so the SAME
// edit shifts the pixel by -jitter -- while CameraAttribs::f2Jitter still
// reports +jitter, because that is the number TAA was handed. The two disagree
// by a sign and the accumulation resolves against a history offset the wrong
// way, which does not look like a broken projection: it looks like TAA is
// simply not sharpening.
//
// So the jitter is applied through the convention the helper was written for:
// convert, apply it verbatim, convert back. ToLeftHandedProjection is its own
// inverse, so the round trip costs eight sign flips and needs no case analysis
// -- ⚠️ INCLUDING THE ORTHOGRAPHIC BRANCH, which is affine, whose offset lives
// in row 3, and which the conversion therefore does not touch at all. That
// branch was already right; the round trip leaves it that way rather than
// requiring anyone to notice.
//
// Safe to alias: out may be m.
void JitterProjection (float out[16], const float m[16], float jitterX, float jitterY);

}   // namespace archviz
}   // namespace geomsrv

#endif
