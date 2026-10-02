#ifndef EVP_ARCHVIZ_DXGI_CAMERASHADERSOURCE_HPP
#define EVP_ARCHVIZ_DXGI_CAMERASHADERSOURCE_HPP

// The two constant-buffer declarations every injected shader reads Archicad's
// camera through (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ ONE COPY, BECAUSE TWO WOULD DRIFT AND NOTHING WOULD COMPARE THEM. These
// four lines are the entire camera contract: our `b1` holds Archicad's view and
// our `b2` the window its projection was copied from (Archicad's `b0` since
// 2026-09-27, CameraLayout.hpp), both 256-byte windows with the matrix at offset
// 0 -- and, at offset 64 of ours, Archicad's own `b2` (`Composite`), read for the
// lens shift alone (2026-10-02) -- and whether each is read `row_major` or `column_major` is what the census
// MEASURED rather than what anyone assumed. Run thirty-three spent a whole run
// with the census proving one reading while the shader implemented another,
// because the claim lived in a comment instead of in the code.
//
// The ghost mesh and the proof primitives are different shaders with different
// inputs, and they must read the camera identically. So the declarations live
// here and the bodies live with whatever draws them.
//
// ⚠️ THE VARIANT NUMBERING IS THE ORACLE'S, NOT A LOCAL CONVENTION:
//
//     bit 0 -- the VIEW is read transposed       (column_major)
//     bit 1 -- the PROJECTION is read transposed (column_major)
//     bit 3 -- the projection window is Archicad's rotation x projection, drawn
//              as (p - eye) * b0                 (cameralayout::kRelative)
//
// so no translation is ever needed between what was measured and what is bound.
// Variants 4 to 7 are reversed multiplication orders, which no declaration can
// express; callers refuse those upstream rather than silently drawing variant 0.
//
// ⚠️ NO PADDING IS DECLARED BECAUSE THE MATRIX SITS AT OFFSET 0 OF ITS WINDOW.
// Run twenty-one measured `numConstants = 16` for both -- a 256-byte window, the
// D3D11.1 minimum granularity -- and a 4x4 fills it exactly.
//
// ⚠️ BODIES NEVER MULTIPLY `View` AND `Projection` THEMSELVES; THEY CALL
// `ArchicadClip`. The census's camera is Archicad's rotation x projection moved to
// the eye (CameraLayout.hpp), and the product a body would write is wrong for it.
// The layout is the interpretation's `cameralayout::kRelative` bit, so the camera
// contract stays in these lines. `View` is Archicad's view in every layout.

#include "ArchViz/Dxgi/CameraLayout.hpp"

#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace camerashader {

constexpr size_t kMaxSource = 8192;
constexpr uint32_t kDeclarableVariants = 4;
// Every declarable variant in each layout; pipelines size their shaders by this.
constexpr uint32_t kShaderSlots = 2 * kDeclarableVariants;

// A transpose (bits 0 and 1), optionally in the relative layout, and nothing else.
inline bool Declarable (uint32_t interpretation)
{
    return (interpretation & ~(3u | cameralayout::kRelative)) == 0;
}

inline uint32_t SlotOf (uint32_t interpretation)
{
    return (interpretation & 3u) | (cameralayout::IsRelative (interpretation) ? kDeclarableVariants : 0u);
}

// Every slot its own declaration. Not `cameralayout::Interpretation`, which folds a
// relative choice onto its one reading: the slots exist for what can be declared.
inline uint32_t InterpretationOfSlot (uint32_t slot)
{
    return (slot & 3u) | ((slot & kDeclarableVariants) != 0 ? cameralayout::kRelative : 0u);
}

// The slot a draw binds: the interpretation's own, or variant 0 when it has no
// declaration or its shader is missing -- the refusal every pipeline already made.
template <typename Shader> uint32_t SlotToBind (uint32_t interpretation, Shader* const* shaders)
{
    const uint32_t slot = Declarable (interpretation) ? SlotOf (interpretation) : 0u;
    return shaders[slot] != nullptr ? slot : 0u;
}

// Writes the declarations for `interpretation` followed by `body` into `out`.
// Returns false when it has no declaration -- a reversed multiplication order
// rather than a transpose.
inline bool Compose (uint32_t interpretation, const char* body, char* out, size_t outBytes)
{
    if (!Declarable (interpretation) || body == nullptr || out == nullptr)
        return false;
    const char* const layouts[2] = { "row_major", "column_major" };
    const bool relative = cameralayout::IsRelative (interpretation);
    // Relative: the eye from `View` (eye = -t * R^T, `cameralayout::Eye`), the
    // point moved to it, through Archicad's rotation x projection -- x, y and w are
    // Archicad's -- and OUR depth, the line `cameralayout::DecodedDepth` applies to
    // what the census scores.
    //
    // ⚠️ AND THE LENS SHIFT ONLY `b2` CARRIES (CameraLayout.hpp, finding 1 amended
    // 2026-10-02): two-point perspective's image is that clip moved by `shift * w`.
    // `Composite` is Archicad's `b2`, copied beside `b0` at offset 64 of the same
    // window, in the same layout, and `ArchicadLensShift` is `cameralayout::LensShift`
    // -- the shift only when `b2` is `b0` moved along w and nothing else, so a screen
    // map, the previous image's camera and a still 3-point view all give none.
    char clip[400] = {};
    if (relative)
        _snprintf_s (clip, sizeof (clip), _TRUNCATE,
                     "float3 eye = -mul (View[3].xyz, transpose ((float3x3) View)); "
                     "float4 c = mul (float4 (world.xyz - eye * world.w, world.w), Projection); "
                     "c.xy += ArchicadLensShift () * c.w; "
                     "c.z = %.9g * c.w + (%.9g); return c;",
                     cameralayout::kDepthA, cameralayout::kDepthB);
    else
        _snprintf_s (clip, sizeof (clip), _TRUNCATE, "return mul (mul (world, View), Projection);");
    char shift[900] = {};
    _snprintf_s (shift, sizeof (shift), _TRUNCATE,
                 "float2 ArchicadLensShift () { "
                 "float3 w = float3 (Projection._14, Projection._24, Projection._34); "
                 "float ww = dot (w, w); "
                 "float3 rw = float3 (Composite._14, Composite._24, Composite._34) - w; "
                 "float3 rx = float3 (Composite._11, Composite._21, Composite._31) - "
                 "float3 (Projection._11, Projection._21, Projection._31); "
                 "float3 ry = float3 (Composite._12, Composite._22, Composite._32) - "
                 "float3 (Projection._12, Projection._22, Projection._32); "
                 "if (!(ww > 1e-6)) return float2 (0, 0); "
                 "float2 s = float2 (dot (rx, w), dot (ry, w)) / ww; "
                 "if (any (abs (rw) > %.9g) || any (abs (rx - s.x * w) > %.9g) || any (abs (ry - s.y * w) > %.9g)) "
                 "return float2 (0, 0); "
                 "return s; }\n",
                 cameralayout::kShiftTolerance, cameralayout::kShiftTolerance, cameralayout::kShiftTolerance);
    // `ArchicadParallel`: `_44` alone is 1 for a parallel projection; for a
    // rotation x projection the whole w column must also be (0, 0, 0, 1).
    const char* const projectionLayout = layouts[(interpretation >> 1) & 1u];
    const int written =
        _snprintf_s (out, outBytes, _TRUNCATE,
                     "cbuffer ArchicadView : register (b1)       { %s float4x4 View; };\n"
                     "cbuffer ArchicadProjection : register (b2) { %s float4x4 Projection; %s float4x4 Composite; };\n"
                     "%s"
                     "float4 ArchicadClip (float4 world) { %s }\n"
                     "bool ArchicadParallel () { return %s; }\n"
                     "%s",
                     layouts[interpretation & 1u], projectionLayout, projectionLayout, shift, clip,
                     relative ? "all (abs (float3 (Projection._14, Projection._24, Projection._34)) < 1e-4) && "
                                "abs (Projection._44 - 1.0) < 1e-4"
                              : "abs (Projection._44 - 1.0) < 1e-4",
                     body);
    return written > 0;
}

} // namespace camerashader
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
