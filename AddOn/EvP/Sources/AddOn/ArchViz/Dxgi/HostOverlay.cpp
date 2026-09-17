// ArchViz/Dxgi/HostOverlay -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/HostOverlay.hpp"

#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hostoverlay {

namespace {

constexpr uint32_t kCameraWindowConstants = 16;

// ⚠️ THE SCALAR RIDES THROUGH THE VERTEX SHADER AND THE RAMP IS IN THE PIXEL
// SHADER, because the ramp is presentation and the scalar is data. Swapping blue
// -> cyan -> yellow -> red for another ramp must not require touching anything
// that knows about Archicad.
//
// ⚠️ AND THE DEPTH BIAS IS APPLIED IN THE VERTEX SHADER, IN CLIP SPACE, NOT
// THROUGH THE RASTERIZER. `DepthBias` is in units of the depth buffer's least
// representable value, which for the D32_FLOAT buffer Archicad binds varies by
// orders of magnitude across the view frustum -- a bias that works at the near
// plane does nothing at the far one. Subtracting a constant from `position.z`
// after projection is uniform in NDC, which is what `OverlayStyle::depthBias`
// says it is.
const char* const kHeatmapBody = "struct VSOut { float4 position : SV_POSITION; float scalar : TEXCOORD0; };\n"
                                 "VSOut VSHeat (float3 position : POSITION, float scalar : TEXCOORD0)\n"
                                 "{\n"
                                 "    VSOut output;\n"
                                 "    float4 p = float4 (position, 1.0);\n"
                                 "    p = mul (p, View);\n"
                                 "    p = mul (p, Projection);\n"
                                 "    p.z -= DepthBias * p.w;\n"
                                 "    output.position = p;\n"
                                 "    output.scalar = scalar;\n"
                                 "    return output;\n"
                                 "}\n"
                                 "float4 PSHeat (VSOut input) : SV_TARGET\n"
                                 "{\n"
                                 "    float t = saturate (input.scalar);\n"
                                 "    float3 cold = float3 (0.10, 0.25, 0.85);\n"
                                 "    float3 cool = float3 (0.10, 0.80, 0.80);\n"
                                 "    float3 warm = float3 (0.95, 0.85, 0.15);\n"
                                 "    float3 hot  = float3 (0.90, 0.20, 0.15);\n"
                                 "    float3 colour = t < 0.33 ? lerp (cold, cool, t / 0.33)\n"
                                 "                  : t < 0.66 ? lerp (cool, warm, (t - 0.33) / 0.33)\n"
                                 "                             : lerp (warm, hot,  (t - 0.66) / 0.34);\n"
                                 "    return float4 (colour, 1.0);\n"
                                 "}\n";

const char* const kWireframeBody = "float4 VSWire (float3 position : POSITION) : SV_POSITION\n"
                                   "{\n"
                                   "    float4 p = float4 (position, 1.0);\n"
                                   "    p = mul (p, View);\n"
                                   "    p = mul (p, Projection);\n"
                                   "    p.z -= DepthBias * p.w;\n"
                                   "    return p;\n"
                                   "}\n"
                                   "float4 PSWire (float4 position : SV_POSITION) : SV_TARGET\n"
                                   "{\n"
                                   "    return float4 (0.35, 0.95, 1.00, 1.0);\n"
                                   "}\n";

// ⚠️ `DepthBias` IS A LITERAL BAKED INTO THE SOURCE, NOT A CONSTANT BUFFER.
// b0, b1 and b2 all belong to Archicad on this path, and taking a fourth slot
// would mean saving and restoring it around a draw that runs inside Archicad's
// frame. A value that changes per BUILD rather than per frame does not justify
// that -- but it is still POLICY, so it is named in `OverlayStyle.hpp` and read
// from there. One number, two uses.

struct Pipeline {
    ID3D11VertexShader* vs[camerashader::kDeclarableVariants] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
};

Pipeline g_heatmap;
Pipeline g_wireframe;

ID3D11Device* g_device = nullptr;
ID3D11RasterizerState* g_solidRaster = nullptr;
ID3D11RasterizerState* g_wireRaster = nullptr;
ID3D11DepthStencilState* g_testNoWrite = nullptr;
ID3D11DepthStencilState* g_testGreaterNoWrite = nullptr;
ID3D11DepthStencilState* g_noTest = nullptr;
ID3D11BlendState* g_blendOpacity = nullptr;

bool g_created = false;
bool g_createFailed = false;
Stats g_stats;

// ⚠️ ATOMICS, BECAUSE THE WRITER IS THE MAIN THREAD AND THE READER
// IS ARCHICAD'S RENDER THREAD INSIDE A DETOUR. A lock on that path could stall
// Archicad's frame, which is the one thing this rung may never do.
std::atomic<bool> g_heatmapOn { true };
std::atomic<bool> g_wireframeOn { true };

template <typename T> void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

void Fail (const char* what)
{
    strncpy_s (g_stats.lastError, sizeof (g_stats.lastError), what, _TRUNCATE);
}

// The camera declarations plus a bias literal, so both bodies read Archicad's
// transform through exactly the same two lines as every other injected draw.
bool ComposeWithBias (uint32_t variant, const char* body, float bias, char* out, size_t outBytes)
{
    char biased[camerashader::kMaxSource] = {};
    const int written =
        _snprintf_s (biased, sizeof (biased), _TRUNCATE, "static const float DepthBias = %.8f;\n%s", bias, body);
    if (written < 0)
        return false;
    return camerashader::Compose (variant, biased, out, outBytes);
}

bool BuildPipeline (Pipeline& pipeline, const char* body, float bias, const char* vsEntry, const char* psEntry,
                    const D3D11_INPUT_ELEMENT_DESC* elements, UINT elementCount, const char* name)
{
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    char source[camerashader::kMaxSource] = {};
    ID3DBlob* first = nullptr;
    ID3DBlob* errors = nullptr;
    bool ok = true;

    for (uint32_t variant = 0; variant < camerashader::kDeclarableVariants && ok; ++variant) {
        if (!ComposeWithBias (variant, body, bias, source, sizeof (source))) {
            Fail ("a host overlay shader source could not be composed");
            return false;
        }
        ID3DBlob* blob = nullptr;
        if (FAILED (D3DCompile (source, strlen (source), name, nullptr, nullptr, vsEntry, "vs_5_0", flags, 0, &blob,
                                &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                    : "a host overlay vertex shader would not compile");
            ok = false;
        }
        else {
            ok = SUCCEEDED (g_device->CreateVertexShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr,
                                                          &pipeline.vs[variant]));
            if (ok && variant == 0) {
                first = blob;
                blob = nullptr;
            }
        }
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
    }

    if (ok && !ComposeWithBias (0, body, bias, source, sizeof (source)))
        ok = false;
    if (ok) {
        ID3DBlob* blob = nullptr;
        if (FAILED (D3DCompile (source, strlen (source), name, nullptr, nullptr, psEntry, "ps_5_0", flags, 0, &blob,
                                &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                    : "a host overlay pixel shader would not compile");
            ok = false;
        }
        else {
            ok = SUCCEEDED (
                g_device->CreatePixelShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr, &pipeline.ps));
        }
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
    }

    ok = ok && first != nullptr &&
         SUCCEEDED (g_device->CreateInputLayout (elements, elementCount, first->GetBufferPointer (),
                                                 first->GetBufferSize (), &pipeline.layout));
    ReleaseAndNull (first);
    return ok;
}

bool EnsurePipeline (ID3D11DeviceContext* context)
{
    if (g_created)
        return true;
    if (g_createFailed || context == nullptr)
        return false;
    if (g_device == nullptr) {
        context->GetDevice (&g_device);
        if (g_device == nullptr) {
            g_createFailed = true;
            Fail ("the context has no device");
            return false;
        }
    }

    // ⚠️ TWO SLOTS, AND THE SCALAR IS THE SECOND. `hostocclusion` keeps the
    // channel out of the position stream so its own depth-only pass over a whole
    // building moves no bytes it cannot use; this is the consumer that pays for
    // the split by declaring it.
    const D3D11_INPUT_ELEMENT_DESC heatmapElements[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    const D3D11_INPUT_ELEMENT_DESC wireElements[1] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    bool ok = BuildPipeline (g_heatmap, kHeatmapBody, overlay::kHostHeatmapDepthBias, "VSHeat", "PSHeat",
                             heatmapElements, 2, "TapiocaHostHeatmap");
    ok = ok && BuildPipeline (g_wireframe, kWireframeBody, overlay::kHostWireframeDepthBias, "VSWire", "PSWire",
                              wireElements, 1, "TapiocaHostWireframe");

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    // ⚠️ NO CULLING, FOR THE SAME REASON THE OCCLUDER DOES NOT CULL: extracted
    // winding is not guaranteed, and a wall whose triangles face away would
    // simply not be coloured -- which reads as the analysis having no data there.
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&raster, &g_solidRaster));

    raster.FillMode = D3D11_FILL_WIREFRAME;
    ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&raster, &g_wireRaster));

    // Tests the buffer the occluder seeded; never writes to it. An analysis
    // overlay that wrote depth would occlude the overlay drawn after it.
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depth, &g_testNoWrite));

    // ⚠️ THE HIDDEN PASS IS THE SAME DRAW WITH THE COMPARISON REVERSED. That
    // is what makes "faded where it is behind something" one extra draw rather
    // than a second representation of the building.
    depth.DepthFunc = D3D11_COMPARISON_GREATER;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depth, &g_testGreaterNoWrite));

    depth.DepthEnable = FALSE;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depth, &g_noTest));

    // Opacity through the blend factor, so a style value costs no constant
    // buffer -- see the note on `DepthBias`.
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED (g_device->CreateBlendState (&blend, &g_blendOpacity));

    if (!ok) {
        if (g_stats.lastError[0] == 0)
            Fail ("the host overlay pipeline could not be created");
        g_createFailed = true;
        return false;
    }
    g_created = true;
    g_stats.ready = true;
    return true;
}

void DrawPass (ID3D11DeviceContext* context, const Pipeline& pipeline, const hostocclusion::HostGeometry& geometry,
               uint32_t variant, bool heatmap, ID3D11DepthStencilState* depthState, ID3D11RasterizerState* raster,
               float opacity)
{
    const UINT positionStride = sizeof (float) * 3;
    const UINT scalarStride = sizeof (float);
    const UINT offset = 0;

    context->IASetInputLayout (pipeline.layout);
    if (heatmap) {
        ID3D11Buffer* const buffers[2] = { geometry.positions, geometry.scalars };
        const UINT strides[2] = { positionStride, scalarStride };
        const UINT offsets[2] = { 0, 0 };
        context->IASetVertexBuffers (0, 2, buffers, strides, offsets);
    }
    else {
        context->IASetVertexBuffers (0, 1, &geometry.positions, &positionStride, &offset);
    }
    context->IASetIndexBuffer (geometry.indices, DXGI_FORMAT_R32_UINT, 0);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (pipeline.vs[variant], nullptr, 0);
    context->PSSetShader (pipeline.ps, nullptr, 0);
    context->OMSetDepthStencilState (depthState, 0);
    context->RSSetState (raster);

    const FLOAT factor[4] = { opacity, opacity, opacity, opacity };
    context->OMSetBlendState (g_blendOpacity, factor, 0xffffffffu);
    context->DrawIndexed (geometry.indexCount, 0, 0);
}

} // namespace

void Draw (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation, Kind kind,
           const overlay::OverlayStyle& style, ID3D11DepthStencilView* depthView)
{
    if (context == nullptr || context1 == nullptr)
        return;
    if (!EnsurePipeline (context))
        return;

    const hostocclusion::HostGeometry geometry = hostocclusion::GetGeometry ();
    if (!geometry.valid) {
        ++g_stats.skippedNoGeometry;
        return;
    }

    ID3D11Buffer* const viewBuffer = injection::ViewSnapshotBuffer ();
    ID3D11Buffer* const projectionBuffer = injection::ProjectionSnapshotBuffer ();
    if (viewBuffer == nullptr || projectionBuffer == nullptr || !injection::SnapshotValid ()) {
        ++g_stats.skippedNoCamera;
        return;
    }

    const Pipeline& pipeline = kind == Kind::Heatmap ? g_heatmap : g_wireframe;
    const uint32_t variant =
        (interpretation < camerashader::kDeclarableVariants && pipeline.vs[interpretation] != nullptr) ? interpretation
                                                                                                       : 0;
    if (pipeline.vs[variant] == nullptr || pipeline.ps == nullptr)
        return;

    ID3D11Buffer* const cameraBuffers[2] = { viewBuffer, projectionBuffer };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);

    const bool heatmap = kind == Kind::Heatmap;
    ID3D11RasterizerState* const raster = heatmap ? g_solidRaster : g_wireRaster;

    // ⚠️ THE HIDDEN PASS GOES FIRST, AND THE ORDER IS THE POINT. It draws what
    // is BEHIND the building; drawing it afterwards would let it blend over the
    // visible pass wherever the two overlap along an edge, and the overlay would
    // get brighter exactly where it is most ambiguous.
    if (style.hiddenOpacity > 0.0f && depthView != nullptr) {
        DrawPass (context, pipeline, geometry, variant, heatmap, g_testGreaterNoWrite, raster,
                  style.opacity * style.hiddenOpacity);
        ++g_stats.hiddenPassDraws;
    }

    // ⚠️ NO DEPTH VIEW MEANS NO OCCLUDER RAN, NOT "DRAW IT OCCLUDED ANYWAY".
    // Testing against a buffer nobody seeded would hide the whole overlay behind
    // whatever Archicad last left there, which is the failure runs forty-eight
    // to fifty-one spent themselves on.
    const bool occlude = style.hostOcclusion != overlay::HostOcclusionMode::None && depthView != nullptr;
    DrawPass (context, pipeline, geometry, variant, heatmap, occlude ? g_testNoWrite : g_noTest, raster, style.opacity);

    if (heatmap)
        ++g_stats.heatmapDraws;
    else
        ++g_stats.wireframeDraws;
    g_stats.trianglesDrawn = geometry.indexCount / 3;
}

void SetEnabled (Kind kind, bool enabled)
{
    (kind == Kind::Heatmap ? g_heatmapOn : g_wireframeOn).store (enabled, std::memory_order_release);
}

bool Enabled (Kind kind)
{
    return (kind == Kind::Heatmap ? g_heatmapOn : g_wireframeOn).load (std::memory_order_acquire);
}

Stats GetStats ()
{
    return g_stats;
}

void Shutdown ()
{
    Pipeline* const pipelines[2] = { &g_heatmap, &g_wireframe };
    for (Pipeline* pipeline : pipelines) {
        for (uint32_t variant = 0; variant < camerashader::kDeclarableVariants; ++variant)
            ReleaseAndNull (pipeline->vs[variant]);
        ReleaseAndNull (pipeline->ps);
        ReleaseAndNull (pipeline->layout);
    }
    ReleaseAndNull (g_blendOpacity);
    ReleaseAndNull (g_noTest);
    ReleaseAndNull (g_testGreaterNoWrite);
    ReleaseAndNull (g_testNoWrite);
    ReleaseAndNull (g_wireRaster);
    ReleaseAndNull (g_solidRaster);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_stats = Stats {};
}

} // namespace hostoverlay
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
