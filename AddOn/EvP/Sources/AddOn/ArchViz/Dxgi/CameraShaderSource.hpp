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
//
// so no translation is ever needed between what was measured and what is bound.
// Variants 4 to 7 are reversed multiplication orders, which no declaration can
// express; callers refuse those upstream rather than silently drawing variant 0.
//
// ⚠️ NO PADDING IS DECLARED BECAUSE THE MATRIX SITS AT OFFSET 0 OF ITS WINDOW.
// Run twenty-one measured `numConstants = 16` for both -- a 256-byte window, the
// D3D11.1 minimum granularity -- and a 4x4 fills it exactly.

#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace camerashader {

constexpr size_t kMaxSource = 8192;
constexpr uint32_t kDeclarableVariants = 4;

// Writes the declarations for `variant` followed by `body` into `out`. Returns
// false when the variant has no declaration -- that is, when it names a reversed
// multiplication order rather than a transpose.
inline bool Compose (uint32_t variant, const char* body, char* out, size_t outBytes)
{
    if (variant >= kDeclarableVariants || body == nullptr || out == nullptr)
        return false;
    const char* const layouts[2] = { "row_major", "column_major" };
    const int written = _snprintf_s (out, outBytes, _TRUNCATE,
                                     "cbuffer ArchicadView : register (b1)       { %s float4x4 View; };\n"
                                     "cbuffer ArchicadProjection : register (b2) { %s float4x4 Projection; };\n"
                                     "%s",
                                     layouts[variant & 1u], layouts[(variant >> 1) & 1u], body);
    return written > 0;
}

} // namespace camerashader
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
