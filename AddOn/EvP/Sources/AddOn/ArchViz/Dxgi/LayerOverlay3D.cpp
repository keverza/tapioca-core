// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. Composition stays at Present, the camera is the one the census
// selected and the wireframe reads, and nothing here holds an Archicad resource.
// ArchViz/Dxgi/LayerOverlay3D -- see the header.

#include "ArchViz/Dxgi/LayerOverlay3D.hpp"

#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/OverlayShaderSources.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace layers3d {

namespace {

constexpr UINT kCameraWindowConstants = 16;

const char* const kBody = overlayshaders::kLayer3DBody;

enum Set : size_t { OccludedFills = 0, OverFills, OccludedLines, OverLines, SetCount };

// ---- the hand-over ------------------------------------------------------------
std::atomic<overlaylayers::Prepared3D*> g_published { nullptr };

// ---- RENDER THREAD ----------------------------------------------------------
std::unique_ptr<overlaylayers::Prepared3D> g_current;
bool g_uploaded = false;
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs[camerashader::kShaderSlots] = {};
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_testNoWrite = nullptr;
ID3D11DepthStencilState* g_noTest = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11Buffer* g_buffers[SetCount] = {};
UINT g_counts[SetCount] = {};
bool g_created = false;
bool g_createFailed = false;
Stats g_stats;

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

void ReleaseBuffers ()
{
    for (size_t i = 0; i < SetCount; ++i) {
        ReleaseAndNull (g_buffers[i]);
        g_counts[i] = 0;
    }
    g_uploaded = false;
}

bool EnsurePipeline (ID3D11DeviceContext* context)
{
    if (g_created)
        return true;
    if (g_createFailed)
        return false;
    context->GetDevice (&g_device);
    if (g_device == nullptr) {
        g_createFailed = true;
        Fail ("the context has no device");
        return false;
    }
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    char biased[camerashader::kMaxSource] = {};
    _snprintf_s (biased, sizeof (biased), _TRUNCATE, "static const float DepthBias = %.8f;\n%s",
                 overlay::kHostWireframeDepthBias, kBody);
    char source[camerashader::kMaxSource] = {};
    ID3DBlob* first = nullptr;
    bool ok = true;
    for (uint32_t slot = 0; slot < camerashader::kShaderSlots && ok; ++slot) {
        if (!camerashader::Compose (camerashader::InterpretationOfSlot (slot), biased, source, sizeof (source))) {
            ok = false;
            Fail ("a layer shader could not be composed");
            break;
        }
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED (D3DCompile (source, strlen (source), "TapiocaLayer3D", nullptr, nullptr, "VSLayer", "vs_5_0", flags,
                                0, &blob, &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer () : "the layer vertex shader failed");
            ok = false;
        }
        else {
            ok = SUCCEEDED (
                g_device->CreateVertexShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr, &g_vs[slot]));
            if (ok && slot == 0) {
                first = blob;
                blob = nullptr;
            }
        }
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
    }
    if (ok && camerashader::Compose (0, biased, source, sizeof (source))) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        ok =
            SUCCEEDED (D3DCompile (source, strlen (source), "TapiocaLayer3D", nullptr, nullptr, "PSLayer", "ps_5_0",
                                   flags, 0, &blob, &errors)) &&
            SUCCEEDED (g_device->CreatePixelShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr, &g_ps));
        if (!ok)
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer () : "the layer pixel shader failed");
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
    }
    // `overlaylayers::ColourVertex`: world float3, then R8G8B8A8 with red lowest.
    const D3D11_INPUT_ELEMENT_DESC elements[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ok = ok && first != nullptr &&
         SUCCEEDED (
             g_device->CreateInputLayout (elements, 2, first->GetBufferPointer (), first->GetBufferSize (), &g_layout));
    ReleaseAndNull (first);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE; // a caller's mesh winds whichever way it was written
    raster.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&raster, &g_raster));

    // Tests the host occluder's depth, never writes it -- the wireframe's rule: one
    // layer must not hide another, nor the reference edges.
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depth, &g_testNoWrite));
    depth.DepthEnable = FALSE;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depth, &g_noTest));

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED (g_device->CreateBlendState (&blend, &g_blend));

    if (!ok) {
        if (g_stats.lastError[0] == 0)
            Fail ("the 3D layer pipeline could not be created");
        g_createFailed = true;
        return false;
    }
    g_created = true;
    return true;
}

// RENDER THREAD: one immutable buffer per set. §11 keeps allocation out of hot hooks;
// this runs once per published change, as the host occluder's own upload does.
void Upload ()
{
    ReleaseBuffers ();
    const std::vector<overlaylayers::ColourVertex>* sets[SetCount] = { &g_current->occludedFills, &g_current->overFills,
                                                                       &g_current->occludedLines,
                                                                       &g_current->overLines };
    g_stats.lineVertices = 0;
    g_stats.fillVertices = 0;
    for (size_t i = 0; i < SetCount; ++i) {
        const std::vector<overlaylayers::ColourVertex>& vertices = *sets[i];
        if (vertices.empty ())
            continue;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = UINT (vertices.size () * sizeof (overlaylayers::ColourVertex));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = vertices.data ();
        if (FAILED (g_device->CreateBuffer (&desc, &data, &g_buffers[i])) || g_buffers[i] == nullptr) {
            Fail ("a 3D layer buffer could not be created");
            continue;
        }
        g_counts[i] = UINT (vertices.size ());
        (i >= OccludedLines ? g_stats.lineVertices : g_stats.fillVertices) += uint32_t (vertices.size ());
    }
    g_stats.generation = g_current->generation;
    ++g_stats.uploads;
    g_uploaded = true;
}

void DrawSet (ID3D11DeviceContext* context, Set set, D3D11_PRIMITIVE_TOPOLOGY topology,
              ID3D11DepthStencilState* depthState)
{
    if (g_buffers[set] == nullptr || g_counts[set] == 0)
        return;
    const UINT stride = UINT (sizeof (overlaylayers::ColourVertex));
    const UINT offset = 0;
    context->IASetVertexBuffers (0, 1, &g_buffers[set], &stride, &offset);
    context->IASetPrimitiveTopology (topology);
    context->OMSetDepthStencilState (depthState, 0);
    context->Draw (g_counts[set], 0);
}

} // namespace

void Publish (overlaylayers::Prepared3D prepared)
{
    overlaylayers::Prepared3D* const fresh = new overlaylayers::Prepared3D (std::move (prepared));
    // A snapshot the render thread never took is simply superseded.
    delete g_published.exchange (fresh, std::memory_order_acq_rel);
}

void Draw (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation,
           ID3D11DepthStencilView* depthView)
{
    if (context == nullptr || context1 == nullptr)
        return;
    if (overlaylayers::Prepared3D* const fresh = g_published.exchange (nullptr, std::memory_order_acq_rel)) {
        g_current.reset (fresh);
        g_uploaded = false;
    }
    if (g_current == nullptr)
        return;
    if (!EnsurePipeline (context))
        return;
    if (!g_uploaded)
        Upload ();
    bool any = false;
    for (UINT count : g_counts)
        any = any || count > 0;
    if (!any)
        return;

    ID3D11Buffer* const viewBuffer = injection::ViewSnapshotBuffer ();
    ID3D11Buffer* const projectionBuffer = injection::ProjectionSnapshotBuffer ();
    if (viewBuffer == nullptr || projectionBuffer == nullptr || !injection::SnapshotValid ()) {
        ++g_stats.skippedNoCamera;
        return;
    }
    const uint32_t slot = camerashader::SlotToBind (interpretation, g_vs);
    if (g_vs[slot] == nullptr || g_ps == nullptr)
        return;
    ID3D11Buffer* const cameraBuffers[2] = { viewBuffer, projectionBuffer };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);
    context->IASetInputLayout (g_layout);
    context->IASetIndexBuffer (nullptr, DXGI_FORMAT_R32_UINT, 0);
    context->VSSetShader (g_vs[slot], nullptr, 0);
    context->PSSetShader (g_ps, nullptr, 0);
    context->RSSetState (g_raster);
    context->OMSetBlendState (g_blend, nullptr, 0xffffffffu);

    // ⚠️ NO DEPTH VIEW MEANS NO OCCLUDER RAN: the occluded sets then draw untested
    // rather than against a buffer nobody seeded (the wireframe's rule, runs 48-51).
    ID3D11DepthStencilState* const occluded = depthView != nullptr ? g_testNoWrite : g_noTest;
    DrawSet (context, OccludedFills, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, occluded);
    DrawSet (context, OverFills, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, g_noTest);
    DrawSet (context, OccludedLines, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, occluded);
    DrawSet (context, OverLines, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, g_noTest);
    ++g_stats.draws;
}

void ReleaseDeviceObjects ()
{
    ReleaseBuffers ();
    for (ID3D11VertexShader*& shader : g_vs)
        ReleaseAndNull (shader);
    ReleaseAndNull (g_ps);
    ReleaseAndNull (g_layout);
    ReleaseAndNull (g_raster);
    ReleaseAndNull (g_testNoWrite);
    ReleaseAndNull (g_noTest);
    ReleaseAndNull (g_blend);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
}

Stats GetStats ()
{
    return g_stats;
}

} // namespace layers3d
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
