#ifndef EVP_ARCHVIZ_DXGI_SURFACEFOCUSCONTEXT_HPP
#define EVP_ARCHVIZ_DXGI_SURFACEFOCUSCONTEXT_HPP

// OVERLAY-INVARIANTS.md §§2,7,8,11: Present composition, owned resources only,
// explicit draw outcomes, and the caller's ScopedPipelineState restores all state.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace geomsrv::archviz::dxgi::surfacefocuscontext {

inline constexpr const char* kVertexShader = R"hlsl(
float4 VSSurfaceContext (uint id : SV_VERTEXID) : SV_POSITION
{
    float2 p = float2 ((id << 1) & 2, id & 2);
    return float4 (p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
)hlsl";

inline constexpr const char* kPixelShader = R"hlsl(
Texture2D<float4> g_surfaceContext : register (t0);
cbuffer SurfaceContext : register (b0)
{
    float contextOpacity;
    float contextWhiteModel; // 1: fade a lifted, desaturated image toward white
    float2 contextPadding;
};

float4 PSSurfaceContext (float4 position : SV_POSITION) : SV_TARGET
{
    float4 source = g_surfaceContext.Load (int3 (int2 (position.xy), 0));
    float grey = dot (source.rgb, float3 (0.2126, 0.7152, 0.0722));
    if (contextWhiteModel > 0.5)
    {
        // Archicad's colours cannot be re-lit white here, so the shading is kept
        // as a lifted grey -- a white model -- and then faded toward white.
        float white = lerp (0.55, 1.0, grey);
        return float4 (lerp (float3 (1.0, 1.0, 1.0), float3 (white, white, white),
                             saturate (contextOpacity)), source.a);
    }
    float3 context = lerp (float3 (0.60, 0.60, 0.60), float3 (grey, grey, grey),
                           saturate (contextOpacity));
    return float4 (context, source.a);
}
)hlsl";

enum class DrawResult : uint8_t {
    Drawn,
    NoTarget,
    UnsupportedTarget,
    PipelineError,
};

// RENDER THREAD, inside the existing Present pipeline-state guard. The copy and
// its view are overlay-owned; no Archicad texture or view is retained.
DrawResult Draw (ID3D11DeviceContext* context, ID3D11RenderTargetView* target, float opacity, bool whiteModel = false);
void Shutdown ();

} // namespace geomsrv::archviz::dxgi::surfacefocuscontext

#endif
