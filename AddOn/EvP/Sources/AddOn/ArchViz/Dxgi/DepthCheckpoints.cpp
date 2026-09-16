// ArchViz/Dxgi/DepthCheckpoints -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/DepthCheckpoints.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"
#include "ArchViz/Dxgi/InjectionProbes.hpp"
#include "ArchViz/Dxgi/PipelineStateGuard.hpp"

#include <d3d11_1.h>

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace checkpoints {

namespace {

constexpr uint32_t kCameraWindowConstants = 16;

struct Slot {
    ID3D11Texture2D* texture = nullptr;
    ID3D11DepthStencilView* view = nullptr;
    ID3D11Query* frontQuery = nullptr;
    ID3D11Query* behindQuery = nullptr;
    bool queryPending = false;
    Result result;
};

ID3D11Device* g_device = nullptr;
Slot g_slots[kCapacity];
Stats g_stats;
D3D11_TEXTURE2D_DESC g_desc = {};
bool g_created = false;
bool g_createFailed = false;

std::atomic<bool> g_enabled { false };
std::atomic<uint32_t> g_interval { 12 };

// Per-frame bookkeeping.
uint64_t g_frameGeneration = 0;
uint64_t g_modelFramesSeen = 0;
bool g_instrumentingThisFrame = false;
uint32_t g_capturedThisFrame = 0;
uint32_t g_drawOrdinal = 0;

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

DXGI_FORMAT DepthViewFormat (DXGI_FORMAT resource)
{
    switch (resource) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_D16_UNORM;
        default:
            return resource;
    }
}

void ReleaseSlots ()
{
    for (uint32_t i = 0; i < kCapacity; ++i) {
        ReleaseAndNull (g_slots[i].view);
        ReleaseAndNull (g_slots[i].texture);
    }
    g_created = false;
    g_stats.ready = false;
}

// ⚠️ THE SAME DESCRIPTION AS ARCHICAD'S, BECAUSE `CopyResource` REQUIRES IT.
// Only the bind flags are ours, and they are the minimum that lets the probes
// test against it.
bool EnsureCreated (ID3D11DeviceContext* context, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc (&desc);
    if (g_created && desc.Width == g_desc.Width && desc.Height == g_desc.Height && desc.Format == g_desc.Format &&
        desc.SampleDesc.Count == g_desc.SampleDesc.Count) {
        return true;
    }
    ReleaseSlots ();
    if (g_createFailed)
        return false;
    if (g_device == nullptr) {
        context->GetDevice (&g_device);
        if (g_device == nullptr) {
            g_createFailed = true;
            Fail ("the context has no device");
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC ours = desc;
    ours.Usage = D3D11_USAGE_DEFAULT;
    ours.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ours.CPUAccessFlags = 0;
    ours.MiscFlags = 0;

    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format = DepthViewFormat (desc.Format);
    viewDesc.ViewDimension =
        desc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;

    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_OCCLUSION;

    bool ok = true;
    for (uint32_t i = 0; i < kCapacity && ok; ++i) {
        ok = ok && SUCCEEDED (g_device->CreateTexture2D (&ours, nullptr, &g_slots[i].texture));
        ok = ok && SUCCEEDED (g_device->CreateDepthStencilView (g_slots[i].texture, &viewDesc, &g_slots[i].view));
        if (g_slots[i].frontQuery == nullptr)
            ok = ok && SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_slots[i].frontQuery));
        if (g_slots[i].behindQuery == nullptr)
            ok = ok && SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_slots[i].behindQuery));
    }
    if (!ok) {
        // ⚠️ EIGHT FULL-RESOLUTION DEPTH TEXTURES IS REAL MEMORY, and failing to
        // get it is a plausible outcome rather than a bug. It says so and stands
        // down instead of leaving half a table behind.
        Fail ("the depth checkpoint textures could not be created");
        g_createFailed = true;
        ReleaseSlots ();
        return false;
    }
    g_desc = desc;
    g_created = true;
    g_stats.ready = true;
    g_stats.width = desc.Width;
    g_stats.height = desc.Height;
    return true;
}

// Everything about the draw that just executed, for the report to explain a
// transition the probes have already located.
void RecordAttributes (ID3D11DeviceContext* context, Attributes& out, uint32_t drawKind, uint32_t indexCount,
                       uint32_t ordinal)
{
    out.used = true;
    out.drawOrdinal = ordinal;
    out.drawKind = drawKind;
    out.indexCount = indexCount;

    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology (&topology);
    out.topology = uint32_t (topology);

    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader (&vs, nullptr, nullptr);
    out.vertexShader = uint64_t (uintptr_t (vs));
    ReleaseAndNull (vs);

    ID3D11PixelShader* ps = nullptr;
    context->PSGetShader (&ps, nullptr, nullptr);
    out.pixelShader = uint64_t (uintptr_t (ps));
    ReleaseAndNull (ps);

    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    context->OMGetBlendState (&blend, factor, &mask);
    out.blendState = uint64_t (uintptr_t (blend));
    if (blend != nullptr) {
        D3D11_BLEND_DESC desc = {};
        blend->GetDesc (&desc);
        out.blendEnable = desc.RenderTarget[0].BlendEnable != FALSE;
        out.srcBlend = uint32_t (desc.RenderTarget[0].SrcBlend);
        out.destBlend = uint32_t (desc.RenderTarget[0].DestBlend);
    }
    ReleaseAndNull (blend);

    ID3D11DepthStencilState* depthState = nullptr;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState (&depthState, &stencilRef);
    out.depthStencilState = uint64_t (uintptr_t (depthState));
    out.depthWrite = true;
    out.depthTest = true;
    out.depthFunc = uint32_t (D3D11_COMPARISON_LESS);
    if (depthState != nullptr) {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        depthState->GetDesc (&desc);
        out.depthTest = desc.DepthEnable != FALSE;
        out.depthWrite = desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        out.depthFunc = uint32_t (desc.DepthFunc);
    }
    ReleaseAndNull (depthState);
}

void ResolveQueries (ID3D11DeviceContext* context)
{
    for (uint32_t i = 0; i < kCapacity; ++i) {
        Slot& slot = g_slots[i];
        if (!slot.queryPending)
            continue;
        UINT64 front = 0;
        UINT64 behind = 0;
        // ⚠️ `DONOTFLUSH`, SO A PENDING QUERY COSTS ARCHICAD NOTHING. A result
        // that is not ready is left for a later frame; this is a measurement and
        // it may never stall the render thread to take one.
        const HRESULT a = context->GetData (slot.frontQuery, &front, sizeof (front), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT b = context->GetData (slot.behindQuery, &behind, sizeof (behind), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (a != S_OK || b != S_OK)
            continue;
        slot.queryPending = false;
        g_stats.queriesResolved += 2;
        ++slot.result.frontDraws;
        ++slot.result.behindDraws;
        slot.result.frontSamples += uint64_t (front);
        slot.result.behindSamples += uint64_t (behind);
        if (front > 0)
            ++slot.result.frontSurvived;
        if (behind > 0)
            ++slot.result.behindSurvived;
    }
}

} // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
    g_stats.enabled = enabled;
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void SetInterval (uint32_t interval)
{
    g_interval.store (interval < 1 ? 1u : interval, std::memory_order_release);
    g_stats.interval = interval < 1 ? 1u : interval;
}

void Reset ()
{
    for (uint32_t i = 0; i < kCapacity; ++i) {
        g_slots[i].result = Result {};
        g_slots[i].queryPending = false;
    }
    const uint32_t interval = g_stats.interval;
    const bool ready = g_stats.ready;
    const bool enabled = g_stats.enabled;
    const uint32_t width = g_stats.width;
    const uint32_t height = g_stats.height;
    g_stats = Stats {};
    g_stats.interval = interval;
    g_stats.ready = ready;
    g_stats.enabled = enabled;
    g_stats.width = width;
    g_stats.height = height;
}

void OnDrawCompleted (ID3D11DeviceContext* context, uint32_t drawKind, uint32_t indexCount, uint64_t modelGeneration)
{
    if (!Enabled () || context == nullptr)
        return;

    ID3D11DepthStencilView* const sceneView = depth::SceneView ();
    if (sceneView == nullptr)
        return;
    const contextstate::ContextState live = contextstate::Snapshot ();
    if (live.depthStencil != uint64_t (uintptr_t (sceneView)))
        return;

    if (modelGeneration != g_frameGeneration) {
        g_frameGeneration = modelGeneration;
        ++g_modelFramesSeen;
        g_capturedThisFrame = 0;
        g_drawOrdinal = 0;
        // ⚠️ ONE FRAME IN `interval`, AND THE REST ARE UNTOUCHED. Eight
        // whole-resource depth copies on every frame would change the thing
        // being measured.
        g_instrumentingThisFrame = (g_modelFramesSeen % g_interval.load (std::memory_order_acquire)) == 0;
        if (g_instrumentingThisFrame)
            ++g_stats.framesInstrumented;
    }
    if (!g_instrumentingThisFrame)
        return;

    const uint32_t ordinal = g_drawOrdinal++;
    if (g_capturedThisFrame >= kCapacity) {
        ++g_stats.overflowed;
        return;
    }

    contextstate::ScopedInjectionGuard guard;
    ID3D11Resource* sourceResource = nullptr;
    sceneView->GetResource (&sourceResource);
    if (sourceResource == nullptr)
        return;
    ID3D11Texture2D* source = nullptr;
    if (FAILED (sourceResource->QueryInterface (__uuidof (ID3D11Texture2D), (void**) &source)) || source == nullptr) {
        ReleaseAndNull (sourceResource);
        return;
    }

    if (EnsureCreated (context, source)) {
        Slot& slot = g_slots[g_capturedThisFrame];
        context->CopyResource (slot.texture, source);
        RecordAttributes (context, slot.result.attributes, drawKind, indexCount, ordinal);
        slot.result.attributes.renderTarget = live.renderTarget;
        slot.result.attributes.depthStencil = live.depthStencil;
        ++slot.result.captures;
        ++g_stats.capturesIssued;
        ++g_capturedThisFrame;
        if (g_capturedThisFrame > g_stats.used)
            g_stats.used = g_capturedThisFrame;
    }
    ReleaseAndNull (source);
    ReleaseAndNull (sourceResource);
}

void Evaluate (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, ID3D11RenderTargetView* targetView,
               float viewportX, float viewportY, float viewportWidth, float viewportHeight)
{
    if (!Enabled () || context == nullptr || context1 == nullptr || targetView == nullptr)
        return;
    ResolveQueries (context);
    if (!g_instrumentingThisFrame || g_capturedThisFrame == 0 || !g_created)
        return;

    const probes::WorldProbePipeline pipeline = probes::GetWorldProbePipeline ();
    if (!pipeline.valid)
        return;
    ID3D11Buffer* const viewBuffer = ViewSnapshotBuffer ();
    ID3D11Buffer* const projectionBuffer = ProjectionSnapshotBuffer ();
    if (viewBuffer == nullptr || projectionBuffer == nullptr || !SnapshotValid ())
        return;
    ++g_stats.evaluations;

    // ⚠️ ONE GUARD, NOT SIXTY LINES OF `*Get*` AND `*Set*`. See
    // `PipelineStateGuard.hpp`: three files carried that block and the index
    // binding reached only one of them.
    const ScopedPipelineState saved (context, context1);

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = viewportX;
    viewport.TopLeftY = viewportY;
    viewport.Width = viewportWidth;
    viewport.Height = viewportHeight;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    if (viewport.Width > 1.0f && viewport.Height > 1.0f)
        context->RSSetViewports (1, &viewport);
    context->RSSetState (pipeline.raster);
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (pipeline.blend, blendFactor, 0xffffffffu);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* const noBuffer = nullptr;
    const UINT zero = 0;
    context->IASetInputLayout (nullptr);
    context->IASetVertexBuffers (0, 1, &noBuffer, &zero, &zero);
    ID3D11Buffer* const cameraBuffers[2] = { viewBuffer, projectionBuffer };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);
    context->OMSetDepthStencilState (pipeline.depthTest, 0);
    context->PSSetShader (pipeline.pixel, nullptr, 0);

    // ⚠️ THE SAME TWO PRIMITIVES AGAINST EVERY CHECKPOINT, DIFFERING IN NOTHING
    // BUT THE DEPTH BOUND. That is what makes the table a measurement rather
    // than a set of unrelated readings.
    for (uint32_t i = 0; i < g_capturedThisFrame; ++i) {
        Slot& slot = g_slots[i];
        if (slot.queryPending)
            continue;
        context->OMSetRenderTargets (1, &targetView, slot.view);

        context->VSSetShader (pipeline.front, nullptr, 0);
        context->Begin (slot.frontQuery);
        context->Draw (3, 0);
        context->End (slot.frontQuery);

        context->VSSetShader (pipeline.behind, nullptr, 0);
        context->Begin (slot.behindQuery);
        context->Draw (3, 0);
        context->End (slot.behindQuery);

        slot.queryPending = true;
        g_stats.queriesIssued += 2;
    }

    // The guard restores and releases everything on the way out.
}

ID3D11DepthStencilView* ViewAt (uint32_t index)
{
    return index < kCapacity ? g_slots[index].view : nullptr;
}

Stats GetStats ()
{
    return g_stats;
}

uint32_t CopyResults (Result* out, uint32_t capacity)
{
    if (out == nullptr)
        return 0;
    const uint32_t count = capacity < kCapacity ? capacity : kCapacity;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = g_slots[i].result;
    return count;
}

void Shutdown ()
{
    for (uint32_t i = 0; i < kCapacity; ++i) {
        ReleaseAndNull (g_slots[i].behindQuery);
        ReleaseAndNull (g_slots[i].frontQuery);
        g_slots[i] = Slot {};
    }
    ReleaseSlots ();
    ReleaseAndNull (g_device);
    g_createFailed = false;
    g_desc = D3D11_TEXTURE2D_DESC {};
    g_stats = Stats {};
    g_instrumentingThisFrame = false;
    g_capturedThisFrame = 0;
    g_modelFramesSeen = 0;
}

} // namespace checkpoints
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
