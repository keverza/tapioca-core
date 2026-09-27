#ifndef EVP_ARCHVIZ_DXGI_CAMERASHADERSOURCE_HPP
#define EVP_ARCHVIZ_DXGI_CAMERASHADERSOURCE_HPP

// The two constant-buffer declarations every injected shader reads Archicad's
// camera through (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ ONE COPY, BECAUSE TWO WOULD DRIFT AND NOTHING WOULD COMPARE THEM. These
// four lines are the entire camera contract: `b1` is Archicad's view, `b2` its
// projection, both 256-byte windows with the matrix at offset 0, and whether
// each is read `row_major` or `column_major` is what the census MEASURED rather
// than what anyone assumed. Run thirty-three spent a whole run with the census
// proving one reading while the shader implemented another, because the claim
// lived in a comment instead of in the code.
//
// The ghost mesh and the proof primitives are different shaders with different
// inputs, and they must read the camera identically. So the declarations live
// here and the bodies live with whatever draws them.
//
// ⚠️ THE VARIANT NUMBERING IS THE ORACLE'S, NOT A LOCAL CONVENTION:
//
//     bit 0 -- the VIEW is read transposed       (column_major)
//     bit 1 -- the PROJECTION is read transposed (column_major)
//     bit 3 -- `b2` holds view x projection      (cameralayout::kCombined)
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
// `ArchicadClip`. Archicad's `b2` is the projection in one layout and view x
// projection in the other (CameraLayout.hpp), and the product a body would
// write is right in only one of them. The layout is the interpretation's
// `cameralayout::kCombined` bit, so the camera contract stays in these lines.
// `View` keeps its meaning in both: `b1` is the view either way.

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

// A transpose (bits 0 and 1), optionally in the combined layout, and nothing else.
inline bool Declarable (uint32_t interpretation)
{
    return (interpretation & ~(3u | cameralayout::kCombined)) == 0;
}

inline uint32_t SlotOf (uint32_t interpretation)
{
    return (interpretation & 3u) | (cameralayout::IsCombined (interpretation) ? kDeclarableVariants : 0u);
}

inline uint32_t InterpretationOfSlot (uint32_t slot)
{
    return cameralayout::Interpretation (slot & 3u, (slot & kDeclarableVariants) != 0);
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
    const bool combined = cameralayout::IsCombined (interpretation);
    // Combined: Archicad's x, y and w, and OUR depth -- the same line
    // `cameralayout::DecodedDepth` applies to what the census scores.
    char clip[192] = {};
    if (combined)
        _snprintf_s (clip, sizeof (clip), _TRUNCATE,
                     "float4 c = mul (world, Projection); c.z = %.9g * c.w + (%.9g); return c;", cameralayout::kDepthA,
                     cameralayout::kDepthB);
    else
        _snprintf_s (clip, sizeof (clip), _TRUNCATE, "return mul (mul (world, View), Projection);");
    // `ArchicadParallel`: `_44` alone is 1 for a parallel projection; for view x
    // projection the whole w column must also be (0, 0, 0, 1).
    const int written =
        _snprintf_s (out, outBytes, _TRUNCATE,
                     "cbuffer ArchicadView : register (b1)       { %s float4x4 View; };\n"
                     "cbuffer ArchicadProjection : register (b2) { %s float4x4 Projection; };\n"
                     "float4 ArchicadClip (float4 world) { %s }\n"
                     "bool ArchicadParallel () { return %s; }\n"
                     "%s",
                     layouts[interpretation & 1u], layouts[(interpretation >> 1) & 1u], clip,
                     combined ? "all (abs (float3 (Projection._14, Projection._24, Projection._34)) < 1e-4) && "
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
