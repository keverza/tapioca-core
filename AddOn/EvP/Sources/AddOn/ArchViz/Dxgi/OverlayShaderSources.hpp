#ifndef EVP_ARCHVIZ_DXGI_OVERLAYSHADERSOURCES_HPP
#define EVP_ARCHVIZ_DXGI_OVERLAYSHADERSOURCES_HPP

// ArchViz/Dxgi/OverlayShaderSources -- the HLSL of the floor-plan overlay, in one
// header so an offline test compiles every entry point
// (tests/cpp/test_overlayshaders.cpp). It is compiled at run time, on Archicad's
// device; a typo here used to be found by a live run.

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace overlayshaders {

// ⚠️ THE STROKE IS WIDENED IN PIXELS, NOT IN METRES. A line of Archicad's plan is a
// pixel wide at every zoom; one widened in model units would be a hairline zoomed out
// and a slab zoomed in. Each segment is one instance: its two ends are projected, and
// six corners around them make two triangles, each end carried half the width past
// its point so a ring's corners close.
//
// ⚠️ `precise` IS WHAT MAKES THE SPLIT WORK. Each point arrives as the float nearest it
// and the float remainder; the anchor does too. Differencing high from high and low
// from low, THEN adding, is what keeps a point near the anchor exact -- and an
// optimiser free to reassociate would add the halves first and throw that away.
// PlanOverlayContent's ShaderPixel is this function in C++, and the tests hold it to
// the double projection at extreme close zoom (§14).
inline constexpr const char* kPlan =
    "cbuffer PlanView : register (b0)\n"
    "{\n"
    "    float4 Linear;\n" // physical pixels per metre: xx, xy, yx, yy
    "    float4 View;\n"   // the anchor, relative to the content origin: hi.xy, lo.xy
    "    float4 Screen;\n" // the anchor's physical pixel, then 2 / width, 2 / height
    "    float4 Colour;\n" // straight alpha
    "    float4 Stroke;\n" // x: half the width, physical pixels
    "};\n"
    "float2 Pixel (float2 hi, float2 lo)\n"
    "{\n"
    "    precise float2 d = (hi - View.xy) + (lo - View.zw);\n"
    "    return float2 (Linear.x * d.x + Linear.y * d.y, Linear.z * d.x + Linear.w * d.y) + Screen.xy;\n"
    "}\n"
    "float4 VSPlanStroke (float4 hi : SEGMENT0, float4 lo : SEGMENT1, uint corner : SV_VertexID) : SV_POSITION\n"
    "{\n"
    "    float2 a = Pixel (hi.xy, lo.xy);\n"
    "    float2 b = Pixel (hi.zw, lo.zw);\n"
    "    float2 d = b - a;\n"
    "    float len = length (d);\n"
    "    float2 along = len > 1e-3 ? d / len : float2 (1.0, 0.0);\n"
    "    float2 across = float2 (-along.y, along.x);\n"
    // corners 0..5: (a,-) (b,-) (b,+) | (a,-) (b,+) (a,+)
    "    bool atB = corner == 1 || corner == 2 || corner == 4;\n"
    "    float side = (corner == 2 || corner == 4 || corner == 5) ? 1.0 : -1.0;\n"
    "    float2 p = (atB ? b + along * Stroke.x : a - along * Stroke.x) + across * (side * Stroke.x);\n"
    "    return float4 (p.x * Screen.z - 1.0, 1.0 - p.y * Screen.w, 0.0, 1.0);\n"
    "}\n"
    "float4 PSPlanStroke (float4 position : SV_POSITION) : SV_TARGET\n"
    "{\n"
    "    return Colour;\n"
    "}\n";

} // namespace overlayshaders
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
