// ArchViz/Dxgi/HostComposite -- see the header. ARCHICAD'S render thread, inside
// the Present detour.

#include "ArchViz/Dxgi/HostComposite.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/PlanCameraMath.hpp"
#include "ArchViz/Dxgi/SharedOverlaySurface.hpp"
#include "ArchViz/ViewportOverlayWindow.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstring>   // memcpy for the warp constants

namespace geomsrv {
namespace archviz {
namespace dxgi {

namespace {

// ⚠️ ONE OVERSIZED TRIANGLE, NOT TWO, and no vertex buffer at all. Three
// vertices generated from SV_VertexID cover the whole target with no seam down
// the diagonal, no input layout to create and nothing of Archicad's vertex state
// to disturb beyond the topology.
const char* kShaderSource = R"hlsl(
Texture2D    overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);

// The blit-time reprojection (PLAT-RE114). `warp` is scale, cos, sin; `offset`
// is in units of the RENDERED frame's half-height; `aspect` converts between uv
// and the half-height space the warp is expressed in. Identity leaves the blit
// exactly as it was.
cbuffer Reprojection : register(b0)
{
    float4 warpScaleCosSinAspect;
    float4 warpOffsetXY;
};

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain (uint vertexId : SV_VertexID)
{
    VSOut output;
    output.uv = float2 ((vertexId << 1) & 2, vertexId & 2);
    output.position = float4 (output.uv * float2 (2.0, -2.0) + float2 (-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain (VSOut input) : SV_TARGET
{
    const float scale  = warpScaleCosSinAspect.x;
    const float cosD   = warpScaleCosSinAspect.y;
    const float sinD   = warpScaleCosSinAspect.z;
    const float aspect = warpScaleCosSinAspect.w;

    // uv -> centred view coordinates in HALF-HEIGHTS, matching the space
    // ComputePlanReprojection works in: x spans +/-aspect, y spans +/-1 and
    // points UP, because D3D's NDC y is up while a texture's v runs down.
    float2 e1 = float2 ((input.uv.x - 0.5) * 2.0 * aspect,
                        (0.5 - input.uv.y) * 2.0);

    float2 e0 = warpOffsetXY.xy + scale * float2 (cosD * e1.x - sinD * e1.y,
                                                  sinD * e1.x + cosD * e1.y);

    float2 uv0 = float2 (0.5 + e0.x / (2.0 * aspect), 0.5 - e0.y * 0.5);

    // ⚠️ OUTSIDE THE RENDERED FRAME IS TRANSPARENT, NOT CLAMPED. A warp that
    // pans the image brings pixels in from beyond its edge; clamping would smear
    // the border row across that band, which reads as the overlay tearing rather
    // than as the honest "this was never drawn". The sampler's border colour is
    // zero, and premultiplied zero is exactly "nothing here".
    return overlayTexture.Sample (overlaySampler, uv0);
}
)hlsl";

std::atomic<bool> g_enabled {false};
std::atomic<uint64_t> g_blits {0};
std::atomic<uint64_t> g_framesConsumed {0};
std::atomic<uint64_t> g_reprojections {0};
std::atomic<uint64_t> g_failures {0};
std::atomic<uint32_t> g_backBufferFormat {0};
std::atomic<uint32_t> g_width {0};
std::atomic<uint32_t> g_height {0};
std::atomic<bool> g_ready {false};

// Archicad's device objects. Built once, on the first frame that runs, and torn
// down when the mode is disarmed. Only ever touched on Archicad's render thread.
struct HostResources {
    ID3D11Device*             device = nullptr;
    ID3D11DeviceContext*      context = nullptr;
    ID3D11VertexShader*       vertexShader = nullptr;
    ID3D11PixelShader*        pixelShader = nullptr;
    ID3D11BlendState*         blend = nullptr;
    ID3D11RasterizerState*    rasterizer = nullptr;
    ID3D11DepthStencilState*  depthStencil = nullptr;
    ID3D11SamplerState*       sampler = nullptr;
    ID3D11Buffer*             warpBuffer = nullptr;
    // Our private copy of the overlay, so a frame can be drawn even when the
    // producer has nothing new -- see the acquire in Composite.
    ID3D11Texture2D*          overlayCopy = nullptr;
    ID3D11ShaderResourceView* overlayView = nullptr;
    // The opened shared surface and its mutex.
    ID3D11Texture2D*          shared = nullptr;
    IDXGIKeyedMutex*          sharedMutex = nullptr;
    uint64_t                  openedHandle = 0;
    uint64_t                  lastGeneration = 0;
    uint32_t                  surfaceWidth = 0;
    uint32_t                  surfaceHeight = 0;

    void ReleaseSurface ()
    {
        if (sharedMutex != nullptr) { sharedMutex->Release (); sharedMutex = nullptr; }
        if (shared != nullptr) { shared->Release (); shared = nullptr; }
        if (overlayView != nullptr) { overlayView->Release (); overlayView = nullptr; }
        if (overlayCopy != nullptr) { overlayCopy->Release (); overlayCopy = nullptr; }
        openedHandle = 0;
        lastGeneration = 0;
        surfaceWidth = 0;
        surfaceHeight = 0;
    }

    void ReleaseAll ()
    {
        ReleaseSurface ();
        if (warpBuffer != nullptr) { warpBuffer->Release (); warpBuffer = nullptr; }
        if (sampler != nullptr) { sampler->Release (); sampler = nullptr; }
        if (depthStencil != nullptr) { depthStencil->Release (); depthStencil = nullptr; }
        if (rasterizer != nullptr) { rasterizer->Release (); rasterizer = nullptr; }
        if (blend != nullptr) { blend->Release (); blend = nullptr; }
        if (pixelShader != nullptr) { pixelShader->Release (); pixelShader = nullptr; }
        if (vertexShader != nullptr) { vertexShader->Release (); vertexShader = nullptr; }
        if (context != nullptr) { context->Release (); context = nullptr; }
        if (device != nullptr) { device->Release (); device = nullptr; }
    }
};
HostResources g_host;

// The reason the last failure gave. An index, not a string: the render thread
// must not allocate, and a torn std::string read across threads is a crash.
enum FailureStep {
    kNoFailure = 0,
    kCompileShaders,
    kCreateStates,
    kOpenShared,
    kCreateCopy,
    kGetBuffer,
    kCreateRtv
};
std::atomic<int> g_lastFailure {kNoFailure};

const char* StepName (int step)
{
    switch (step) {
        case kCompileShaders: return "the blit shaders would not compile on Archicad's device";
        case kCreateStates:   return "a blend/raster/depth/sampler state would not create";
        case kOpenShared:     return "OpenSharedResource1 refused the overlay surface";
        case kCreateCopy:     return "the private copy of the overlay surface would not create";
        case kGetBuffer:      return "IDXGISwapChain::GetBuffer refused the back buffer";
        case kCreateRtv:      return "CreateRenderTargetView refused the back buffer";
        default:              return "";
    }
}

void Fail (FailureStep step)
{
    g_lastFailure.store (step, std::memory_order_relaxed);
    g_failures.fetch_add (1, std::memory_order_relaxed);
}

// ⚠️ EVERY FIELD IN HERE IS ONE ARCHICAD WOULD OTHERWISE LOSE. The list is
// `imgui_impl_dx11.cpp`'s, which is the reference precisely because it was
// arrived at by finding out what breaks. Restoring a subset is worse than not
// drawing at all: the corruption lands in somebody else's frame, intermittently,
// and looks like a graphics-driver bug.
struct D3D11StateBackup {
    ID3D11DeviceContext*      context = nullptr;
    UINT                      scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT                scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RasterizerState*    rasterizer = nullptr;
    ID3D11BlendState*         blend = nullptr;
    FLOAT                     blendFactor[4] = {};
    UINT                      sampleMask = 0;
    ID3D11DepthStencilState*  depthStencil = nullptr;
    UINT                      stencilRef = 0;
    ID3D11ShaderResourceView* pixelShaderResource = nullptr;
    ID3D11SamplerState*       pixelSampler = nullptr;
    ID3D11Buffer*             pixelConstantBuffer = nullptr;
    ID3D11VertexShader*       vertexShader = nullptr;
    ID3D11PixelShader*        pixelShader = nullptr;
    ID3D11GeometryShader*     geometryShader = nullptr;
    UINT                      vertexShaderInstanceCount = 256;
    UINT                      pixelShaderInstanceCount = 256;
    UINT                      geometryShaderInstanceCount = 256;
    ID3D11ClassInstance*      vertexShaderInstances[256] = {};
    ID3D11ClassInstance*      pixelShaderInstances[256] = {};
    ID3D11ClassInstance*      geometryShaderInstances[256] = {};
    D3D11_PRIMITIVE_TOPOLOGY  topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*             indexBuffer = nullptr;
    DXGI_FORMAT               indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT                      indexOffset = 0;
    ID3D11Buffer*             vertexBuffer = nullptr;
    UINT                      vertexStride = 0;
    UINT                      vertexOffset = 0;
    ID3D11InputLayout*        inputLayout = nullptr;
    ID3D11RenderTargetView*   renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView*   depthStencilView = nullptr;

    explicit D3D11StateBackup (ID3D11DeviceContext* ctx) : context (ctx)
    {
        context->RSGetScissorRects (&scissorCount, scissors);
        context->RSGetViewports (&viewportCount, viewports);
        context->RSGetState (&rasterizer);
        context->OMGetBlendState (&blend, blendFactor, &sampleMask);
        context->OMGetDepthStencilState (&depthStencil, &stencilRef);
        context->OMGetRenderTargets (D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets,
                                     &depthStencilView);
        context->PSGetShaderResources (0, 1, &pixelShaderResource);
        context->PSGetSamplers (0, 1, &pixelSampler);
        context->PSGetConstantBuffers (0, 1, &pixelConstantBuffer);
        context->PSGetShader (&pixelShader, pixelShaderInstances, &pixelShaderInstanceCount);
        context->VSGetShader (&vertexShader, vertexShaderInstances, &vertexShaderInstanceCount);
        context->GSGetShader (&geometryShader, geometryShaderInstances,
                              &geometryShaderInstanceCount);
        context->IAGetPrimitiveTopology (&topology);
        context->IAGetIndexBuffer (&indexBuffer, &indexFormat, &indexOffset);
        context->IAGetVertexBuffers (0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        context->IAGetInputLayout (&inputLayout);
    }

    ~D3D11StateBackup ()
    {
        context->RSSetScissorRects (scissorCount, scissors);
        context->RSSetViewports (viewportCount, viewports);
        context->RSSetState (rasterizer);
        if (rasterizer != nullptr) rasterizer->Release ();
        context->OMSetBlendState (blend, blendFactor, sampleMask);
        if (blend != nullptr) blend->Release ();
        context->OMSetDepthStencilState (depthStencil, stencilRef);
        if (depthStencil != nullptr) depthStencil->Release ();
        context->OMSetRenderTargets (D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets,
                                     depthStencilView);
        for (ID3D11RenderTargetView* view : renderTargets)
            if (view != nullptr) view->Release ();
        if (depthStencilView != nullptr) depthStencilView->Release ();
        context->PSSetShaderResources (0, 1, &pixelShaderResource);
        if (pixelShaderResource != nullptr) pixelShaderResource->Release ();
        context->PSSetSamplers (0, 1, &pixelSampler);
        if (pixelSampler != nullptr) pixelSampler->Release ();
        context->PSSetConstantBuffers (0, 1, &pixelConstantBuffer);
        if (pixelConstantBuffer != nullptr) pixelConstantBuffer->Release ();
        context->PSSetShader (pixelShader, pixelShaderInstances, pixelShaderInstanceCount);
        if (pixelShader != nullptr) pixelShader->Release ();
        for (UINT i = 0; i < pixelShaderInstanceCount; ++i)
            if (pixelShaderInstances[i] != nullptr) pixelShaderInstances[i]->Release ();
        context->VSSetShader (vertexShader, vertexShaderInstances, vertexShaderInstanceCount);
        if (vertexShader != nullptr) vertexShader->Release ();
        for (UINT i = 0; i < vertexShaderInstanceCount; ++i)
            if (vertexShaderInstances[i] != nullptr) vertexShaderInstances[i]->Release ();
        context->GSSetShader (geometryShader, geometryShaderInstances,
                              geometryShaderInstanceCount);
        if (geometryShader != nullptr) geometryShader->Release ();
        for (UINT i = 0; i < geometryShaderInstanceCount; ++i)
            if (geometryShaderInstances[i] != nullptr) geometryShaderInstances[i]->Release ();
        context->IASetPrimitiveTopology (topology);
        context->IASetIndexBuffer (indexBuffer, indexFormat, indexOffset);
        if (indexBuffer != nullptr) indexBuffer->Release ();
        context->IASetVertexBuffers (0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        if (vertexBuffer != nullptr) vertexBuffer->Release ();
        context->IASetInputLayout (inputLayout);
        if (inputLayout != nullptr) inputLayout->Release ();
    }
};

bool BuildPipeline (ID3D11Device* device)
{
    if (g_host.vertexShader != nullptr && g_host.device == device)
        return true;
    if (g_host.device != device)
        g_host.ReleaseAll ();
    g_host.device = device;

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile (kShaderSource, strlen (kShaderSource), nullptr, nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &vertexBlob, &errors);
    if (SUCCEEDED (hr)) {
        if (errors != nullptr) { errors->Release (); errors = nullptr; }
        hr = D3DCompile (kShaderSource, strlen (kShaderSource), nullptr, nullptr, nullptr,
                         "PSMain", "ps_4_0", 0, 0, &pixelBlob, &errors);
    }
    if (errors != nullptr) {
        ArchVizLog (std::string ("host composite shader: ") + (const char*) errors->GetBufferPointer ());
        errors->Release ();
    }
    if (SUCCEEDED (hr))
        hr = device->CreateVertexShader (vertexBlob->GetBufferPointer (),
                                         vertexBlob->GetBufferSize (), nullptr,
                                         &g_host.vertexShader);
    if (SUCCEEDED (hr))
        hr = device->CreatePixelShader (pixelBlob->GetBufferPointer (),
                                        pixelBlob->GetBufferSize (), nullptr,
                                        &g_host.pixelShader);
    if (vertexBlob != nullptr) vertexBlob->Release ();
    if (pixelBlob != nullptr) pixelBlob->Release ();
    if (FAILED (hr)) {
        Fail (kCompileShaders);
        g_host.ReleaseAll ();
        return false;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    // Premultiplied -- see the header.
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState (&blendDesc, &g_host.blend);

    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    // ⚠️ SCISSOR OFF. Archicad may have left a scissor rect set for its own
    // drawing, and inheriting it would clip the overlay to whatever region
    // Archicad happened to be working on -- intermittently, and only on some
    // frames, which is the hardest possible thing to diagnose from a screenshot.
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.DepthClipEnable = FALSE;
    if (SUCCEEDED (hr))
        hr = device->CreateRasterizerState (&rasterDesc, &g_host.rasterizer);

    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    // ⚠️ DEPTH TEST AND WRITE BOTH OFF. The overlay is composited over a
    // finished frame; testing against Archicad's depth buffer would hide it
    // behind the very geometry it is meant to sit on top of, and writing to that
    // buffer would corrupt whatever Archicad does with it next.
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.StencilEnable = FALSE;
    if (SUCCEEDED (hr))
        hr = device->CreateDepthStencilState (&depthDesc, &g_host.depthStencil);

    D3D11_SAMPLER_DESC samplerDesc = {};
    // Point sampling: the overlay is rendered at exactly the back buffer's size,
    // so any filtering is a blur applied to a 1:1 copy.
    // ⚠️ LINEAR, NOT POINT, NOW THAT THE IMAGE IS WARPED. Point sampling was
    // right for a 1:1 copy and is wrong the moment the source is sampled off the
    // pixel grid: a reprojected line would crawl between pixels as the view
    // moves, which is far more visible than the half-pixel softness linear
    // costs.
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    // BORDER, not CLAMP -- see the shader.
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    if (SUCCEEDED (hr))
        hr = device->CreateSamplerState (&samplerDesc, &g_host.sampler);

    D3D11_BUFFER_DESC warpDesc = {};
    warpDesc.ByteWidth = 32;            // two float4s
    warpDesc.Usage = D3D11_USAGE_DYNAMIC;
    warpDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    warpDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED (hr))
        hr = device->CreateBuffer (&warpDesc, nullptr, &g_host.warpBuffer);

    if (FAILED (hr)) {
        Fail (kCreateStates);
        g_host.ReleaseAll ();
        return false;
    }
    ArchVizLog ("host composite: blit pipeline built on Archicad's device");
    return true;
}

// Open (or reopen) the producer's surface and make the private copy the blit
// reads from. Returns false having left nothing half-built.
bool EnsureSurface (ID3D11Device* device)
{
    const uint64_t handle = SharedOverlayHandle ();
    if (handle == 0) {
        g_host.ReleaseSurface ();
        return false;
    }
    const uint32_t width = SharedOverlayWidth ();
    const uint32_t height = SharedOverlayHeight ();
    if (g_host.shared != nullptr && g_host.openedHandle == handle &&
        g_host.surfaceWidth == width && g_host.surfaceHeight == height)
        return true;

    g_host.ReleaseSurface ();
    if (width == 0 || height == 0)
        return false;

    ID3D11Device1* device1 = nullptr;
    if (FAILED (device->QueryInterface (__uuidof (ID3D11Device1), (void**) &device1)) ||
        device1 == nullptr) {
        Fail (kOpenShared);
        return false;
    }
    // ⚠️ OpenSharedResource1, NOT OpenSharedResource. The producer created an NT
    // handle; the legacy call takes the legacy handle type and fails on this one
    // with a generic E_INVALIDARG that says nothing about which of the two is
    // wrong.
    HRESULT hr = device1->OpenSharedResource1 ((HANDLE) (uintptr_t) handle,
                                               __uuidof (ID3D11Texture2D),
                                               (void**) &g_host.shared);
    device1->Release ();
    if (FAILED (hr) || g_host.shared == nullptr) {
        Fail (kOpenShared);
        g_host.ReleaseSurface ();
        return false;
    }
    if (FAILED (g_host.shared->QueryInterface (__uuidof (IDXGIKeyedMutex),
                                               (void**) &g_host.sharedMutex))) {
        Fail (kOpenShared);
        g_host.ReleaseSurface ();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device->CreateTexture2D (&desc, nullptr, &g_host.overlayCopy);
    if (SUCCEEDED (hr))
        hr = device->CreateShaderResourceView (g_host.overlayCopy, nullptr, &g_host.overlayView);
    if (FAILED (hr)) {
        Fail (kCreateCopy);
        g_host.ReleaseSurface ();
        return false;
    }

    g_host.openedHandle = handle;
    g_host.surfaceWidth = width;
    g_host.surfaceHeight = height;
    g_host.lastGeneration = 0;
    ArchVizLog ("host composite: opened the shared overlay surface (" + std::to_string (width) +
                "x" + std::to_string (height) + ")");
    return true;
}

}   // namespace

void CompositeOverlayIfTarget (IDXGISwapChain* swapChain)
{
    if (!g_enabled.load (std::memory_order_acquire) || swapChain == nullptr)
        return;
    // The nomination is the marker's -- one identification of Archicad's chain
    // serves both, and having two would let them disagree.
    if (uint64_t (uintptr_t (swapChain)) != MarkerTarget ())
        return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) ||
        backBuffer == nullptr) {
        Fail (kGetBuffer);
        return;
    }

    // ⚠️ THE DEVICE AND CONTEXT ARE CACHED; THE BACK BUFFER AND ITS VIEW ARE NOT.
    // This runs on ARCHICAD'S render thread between two of its own draws, so
    // every avoidable call here is latency added to Archicad -- and the first
    // live run reported exactly that ("feels like it added slight lag"). The
    // device cannot change without the chain changing, so asking for it, its
    // context and the context's interfaces on every one of ~60 frames a second
    // was pure overhead. The back buffer still must be fetched fresh: a
    // flip-model chain rotates it after every Present.
    ID3D11Device* device = g_host.device;
    ID3D11DeviceContext* context = g_host.context;
    if (device == nullptr || context == nullptr) {
        backBuffer->GetDevice (&device);
        if (device != nullptr)
            device->GetImmediateContext (&context);
        if (device == nullptr || context == nullptr) {
            if (context != nullptr) context->Release ();
            if (device != nullptr) device->Release ();
            backBuffer->Release ();
            g_ready.store (false, std::memory_order_release);
            return;
        }
        // Kept, and deliberately never released -- see SetHostCompositeEnabled
        // for why these outlive the mode rather than being freed off-thread.
        g_host.device = device;
        g_host.context = context;
    }

    if (!BuildPipeline (device) || !EnsureSurface (device)) {
        g_ready.store (false, std::memory_order_release);
        backBuffer->Release ();
        return;
    }
    g_ready.store (true, std::memory_order_release);

    D3D11_TEXTURE2D_DESC backDesc = {};
    backBuffer->GetDesc (&backDesc);
    g_backBufferFormat.store (uint32_t (backDesc.Format), std::memory_order_relaxed);
    g_width.store (backDesc.Width, std::memory_order_relaxed);
    g_height.store (backDesc.Height, std::memory_order_relaxed);

    // ---- take the newest overlay frame, if there is one --------------------
    // ⚠️ ZERO TIMEOUT, AND THE PRIVATE COPY IS WHY IT IS SAFE TO MISS. Waiting
    // here would stall ARCHICAD'S render thread on our frame rate, which is a
    // hang in Archicad caused by us. Missing simply redraws the previous
    // overlay frame, which is what the last frame showed anyway.
    const uint64_t generation = SharedOverlayGeneration ();
    if (generation != g_host.lastGeneration &&
        g_host.sharedMutex->AcquireSync (1, 0) == S_OK) {
        context->CopyResource (g_host.overlayCopy, g_host.shared);
        g_host.sharedMutex->ReleaseSync (0);
        g_host.lastGeneration = generation;
        g_framesConsumed.fetch_add (1, std::memory_order_relaxed);
    }

    // ---- the reprojection (PLAT-RE114) -------------------------------------
    // ⚠️ COMPUTED FROM THE POSE THE FRAME WAS DRAWN WITH AGAINST THE NEWEST ONE
    // KNOWN, HERE, at the last possible instant. Everything else in this file
    // fixes WHEN the pixels land; this is the only step that fixes what they
    // contain. Neither pose valid -- a perspective frame, or no camera tick yet
    // -- leaves the identity warp, which is exactly the previous behaviour.
    float warpConstants[8] = {1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    warpConstants[3] = (backDesc.Height > 0)
        ? float (backDesc.Width) / float (backDesc.Height) : 1.0f;
    const SharedOverlayPose rendered = SharedOverlayFramePose ();
    const SharedOverlayPose latest = LatestPlanPose ();
    bool reprojected = false;
    if (rendered.valid && latest.valid) {
        const PlanReprojection warp = ComputePlanReprojection (
            rendered.centreX, rendered.centreY, rendered.halfHeightMetres,
            rendered.rotationRadians, latest.centreX, latest.centreY,
            latest.halfHeightMetres, latest.rotationRadians);
        if (warp.valid) {
            warpConstants[0] = warp.scale;
            warpConstants[1] = warp.cosDelta;
            warpConstants[2] = warp.sinDelta;
            warpConstants[4] = warp.offsetX;
            warpConstants[5] = warp.offsetY;
            reprojected = true;
        }
    }
    if (reprojected)
        g_reprojections.fetch_add (1, std::memory_order_relaxed);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED (context->Map (g_host.warpBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy (mapped.pData, warpConstants, sizeof (warpConstants));
        context->Unmap (g_host.warpBuffer, 0);
    }

    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED (device->CreateRenderTargetView (backBuffer, nullptr, &rtv)) && rtv != nullptr) {
        // Everything below this line is undone by the destructor, on every path.
        D3D11StateBackup backup (context);

        D3D11_VIEWPORT viewport = {};
        viewport.Width = float (backDesc.Width);
        viewport.Height = float (backDesc.Height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports (1, &viewport);
        context->RSSetState (g_host.rasterizer);
        context->OMSetRenderTargets (1, &rtv, nullptr);
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetBlendState (g_host.blend, blendFactor, 0xffffffff);
        context->OMSetDepthStencilState (g_host.depthStencil, 0);
        context->IASetInputLayout (nullptr);
        context->IASetVertexBuffers (0, 0, nullptr, nullptr, nullptr);
        context->IASetIndexBuffer (nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader (g_host.vertexShader, nullptr, 0);
        context->PSSetShader (g_host.pixelShader, nullptr, 0);
        context->GSSetShader (nullptr, nullptr, 0);
        context->PSSetShaderResources (0, 1, &g_host.overlayView);
        context->PSSetSamplers (0, 1, &g_host.sampler);
        context->PSSetConstantBuffers (0, 1, &g_host.warpBuffer);
        context->Draw (3, 0);

        g_blits.fetch_add (1, std::memory_order_relaxed);
        rtv->Release ();
    } else {
        Fail (kCreateRtv);
    }

    backBuffer->Release ();
}

void SetHostCompositeEnabled (bool enabled)
{
    if (enabled) {
        g_blits.store (0, std::memory_order_relaxed);
        g_framesConsumed.store (0, std::memory_order_relaxed);
        g_reprojections.store (0, std::memory_order_relaxed);
        g_failures.store (0, std::memory_order_relaxed);
        g_lastFailure.store (kNoFailure, std::memory_order_relaxed);
    }
    g_enabled.store (enabled, std::memory_order_release);
    if (!enabled) {
        // ⚠️ THE D3D OBJECTS ARE NOT RELEASED HERE. They belong to Archicad's
        // device and may only be touched from its render thread; freeing them
        // from the main thread is a race against a Present already inside the
        // detour. They are released on the next composite call that finds itself
        // disabled -- see the top of CompositeOverlayIfTarget -- and in the worst
        // case they live until Archicad's device does, which is a handful of
        // small objects and no correctness problem.
        g_ready.store (false, std::memory_order_release);
    }
}

bool HostCompositeEnabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

bool HostCompositeReady ()
{
    return g_ready.load (std::memory_order_acquire);
}

void WatchHostComposite ()
{
    static uint64_t lastBlits = 0;
    static uint64_t lastChangeMs = 0;
    static bool     restored = false;

    if (!g_enabled.load (std::memory_order_acquire)) {
        lastBlits = 0;
        lastChangeMs = 0;
        restored = false;
        return;
    }

    const uint64_t blits = g_blits.load (std::memory_order_relaxed);
    const uint64_t nowMs = GetTickCount64 ();
    if (blits != lastBlits || lastChangeMs == 0) {
        lastBlits = blits;
        lastChangeMs = nowMs;
        return;
    }
    // Three seconds of a hidden overlay and a frozen blit count. Long enough
    // that a stalled frame or a minimised window does not trip it, short enough
    // that a user staring at an empty screen gets it back before giving up.
    constexpr uint64_t kStallMs = 3000;
    if (!restored && nowMs - lastChangeMs > kStallMs) {
        restored = true;
        viewportoverlay::SetVisible (true);
        ArchVizLog ("host composite: no blit for 3 s -- the overlay window is being shown "
                    "again so the picture is not lost. hookdraw is still armed; switch to "
                    "another camera sync mode to remove the hook.");
    }
}

HostCompositeStats GetHostCompositeStats ()
{
    HostCompositeStats stats;
    stats.enabled = g_enabled.load (std::memory_order_acquire);
    stats.ready = g_ready.load (std::memory_order_acquire);
    stats.blits = g_blits.load (std::memory_order_relaxed);
    stats.framesConsumed = g_framesConsumed.load (std::memory_order_relaxed);
    stats.reprojections = g_reprojections.load (std::memory_order_relaxed);
    stats.failures = g_failures.load (std::memory_order_relaxed);
    stats.backBufferFormat = g_backBufferFormat.load (std::memory_order_relaxed);
    stats.width = g_width.load (std::memory_order_relaxed);
    stats.height = g_height.load (std::memory_order_relaxed);
    stats.lastError = StepName (g_lastFailure.load (std::memory_order_relaxed));
    return stats;
}

}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
