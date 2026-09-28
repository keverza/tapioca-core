// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings and each
// cost at least one. Composition stays at Present, everything the draw touches is put
// back through ScopedPipelineState, and the plan's Present creates nothing but its view.
// ArchViz/Dxgi/PlanOverlayLayer -- see the header.

#include "ArchViz/Dxgi/PlanOverlayLayer.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/OverlayShaderSources.hpp"
#include "ArchViz/Dxgi/PipelineStateGuard.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace planlayer {

namespace {

// The HLSL is in OverlayShaderSources.hpp, where the offline test compiles it.
const char* const kShader = overlayshaders::kPlan;

// A buffer is at most 128 MB in D3D11; half of it is a very large plan.
constexpr size_t kMaxSegments = (64u * 1024u * 1024u) / sizeof (plancontent::Segment);

// ---- read from any chain's thread ------------------------------------------------
std::atomic<bool> g_armed { false };
std::atomic<uint64_t> g_canvas { 0 };
std::atomic<uint32_t> g_presentThread { 0 };
std::atomic<TransformReader> g_reader { nullptr };

std::atomic<uint64_t> g_chain { 0 };
std::atomic<uint64_t> g_canvasPresents { 0 };
std::atomic<uint64_t> g_drawn { 0 };
std::atomic<uint64_t> g_readsFresh { 0 };
std::atomic<uint64_t> g_readsRefused { 0 };
std::atomic<uint64_t> g_readsInvalid { 0 };
std::atomic<uint64_t> g_drawnWithLastRead { 0 };
std::atomic<int32_t> g_lastReadError { 0 };
std::atomic<uint64_t> g_declines[size_t (Decline::Count)];
std::atomic<uint32_t> g_bufferWidth { 0 };
std::atomic<uint32_t> g_bufferHeight { 0 };
std::atomic<uint32_t> g_readUsLast { 0 };
std::atomic<uint32_t> g_readUsMax { 0 };
std::atomic<uint32_t> g_drawUsLast { 0 };
std::atomic<uint32_t> g_drawUsMax { 0 };

// ---- MAIN THREAD only: Prepare, Release, and the plan's Present past its thread test
Style g_style;
ID3D11Device* g_device = nullptr;    // held from the chain's first Present (§12b)
ID3D11Device* g_builtFor = nullptr;  // identity only: the device the pipeline is on
ID3D11Device* g_failedFor = nullptr; // identity only: a build that failed is not retried
bool g_deviceChanged = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11Buffer* g_constants = nullptr;
ID3D11Buffer* g_segments = nullptr;
uint32_t g_segmentCount = 0;
uint64_t g_generation = 0;
bool g_haveGeneration = false;
double g_originX = 0.0;
double g_originY = 0.0;
plancontent::PixelTransform g_last;
bool g_haveLast = false;

void Bump (std::atomic<uint64_t>& counter)
{
    counter.fetch_add (1, std::memory_order_relaxed);
}

void Declined (Decline decline)
{
    Bump (g_declines[size_t (decline)]);
}

int64_t QpcNow ()
{
    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter (&now);
    return now.QuadPart;
}

int64_t QpcFrequency ()
{
    static const int64_t frequency = [] () {
        LARGE_INTEGER value = {};
        ::QueryPerformanceFrequency (&value);
        return value.QuadPart;
    }();
    return frequency;
}

void RecordMicros (int64_t began, std::atomic<uint32_t>& last, std::atomic<uint32_t>& worst)
{
    const int64_t frequency = QpcFrequency ();
    if (frequency <= 0)
        return;
    const uint32_t us = uint32_t ((QpcNow () - began) * 1000000 / frequency);
    last.store (us, std::memory_order_relaxed);
    if (us > worst.load (std::memory_order_relaxed))
        worst.store (us, std::memory_order_relaxed);
}

template <typename T> void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

// A COM reference taken inside one Present and given back on every path out of it.
template <typename T> struct Transient {
    T* p = nullptr;
    Transient () = default;
    Transient (const Transient&) = delete;
    Transient& operator= (const Transient&) = delete;
    ~Transient ()
    {
        if (p != nullptr)
            p->Release ();
    }
};

void ReleasePipeline ()
{
    ReleaseAndNull (g_segments);
    ReleaseAndNull (g_constants);
    ReleaseAndNull (g_depth);
    ReleaseAndNull (g_blend);
    ReleaseAndNull (g_raster);
    ReleaseAndNull (g_layout);
    ReleaseAndNull (g_ps);
    ReleaseAndNull (g_vs);
    g_builtFor = nullptr;
    g_segmentCount = 0;
    g_haveGeneration = false;
}

bool Compile (const char* entry, const char* target, ID3DBlob*& blob, std::string& error)
{
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile (kShader, std::strlen (kShader), "TapiocaPlanStroke", nullptr, nullptr, entry, target,
                                   flags, 0, &blob, &errors);
    if (FAILED (hr) || blob == nullptr) {
        error = std::string ("the plan stroke's ") + entry + " would not compile" +
                (errors != nullptr ? std::string (": ") + (const char*) errors->GetBufferPointer () : std::string ());
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
        return false;
    }
    ReleaseAndNull (errors);
    return true;
}

// MAIN THREAD, outside any Present: the whole pipeline on `g_device`.
bool BuildPipeline (std::string& error)
{
    ID3DBlob* vertex = nullptr;
    ID3DBlob* pixel = nullptr;
    if (!Compile ("VSPlanStroke", "vs_5_0", vertex, error) || !Compile ("PSPlanStroke", "ps_5_0", pixel, error)) {
        ReleaseAndNull (vertex);
        return false;
    }
    bool ok = SUCCEEDED (
        g_device->CreateVertexShader (vertex->GetBufferPointer (), vertex->GetBufferSize (), nullptr, &g_vs));
    ok = ok &&
         SUCCEEDED (g_device->CreatePixelShader (pixel->GetBufferPointer (), pixel->GetBufferSize (), nullptr, &g_ps));
    // ⚠️ PER-INSTANCE, NOT PER-VERTEX. The six corners come from SV_VertexID; the one
    // thing a corner needs from the buffer is its segment, so the segment is the
    // instance and the buffer holds each once.
    const D3D11_INPUT_ELEMENT_DESC elements[2] = {
        { "SEGMENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "SEGMENT", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    ok = ok && SUCCEEDED (g_device->CreateInputLayout (elements, 2, vertex->GetBufferPointer (),
                                                       vertex->GetBufferSize (), &g_layout));
    ReleaseAndNull (vertex);
    ReleaseAndNull (pixel);

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE; // a stroke's two triangles wind whichever way its segment runs
    raster.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&raster, &g_raster));

    // Straight alpha over Archicad's pixels; the buffer's own alpha is left as it was.
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

    // ⚠️ NO DEPTH AND NO STENCIL. The plan's DSV is bound for stencil only and its depth
    // is never cleared (finding 13): 2D is draw order, and the overlay is drawn last.
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.StencilEnable = FALSE;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depth, &g_depth));

    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = UINT (sizeof (plancontent::ViewConstants));
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&constants, nullptr, &g_constants));

    if (!ok) {
        if (error.empty ())
            error = "the plan stroke's shaders, states or constant buffer could not be created on Archicad's device";
        ReleasePipeline ();
        return false;
    }
    g_builtFor = g_device;
    return true;
}

// MAIN THREAD, outside any Present: the content, as one immutable buffer.
bool Upload (const plancontent::Content& content, std::string& error)
{
    ReleaseAndNull (g_segments);
    g_segmentCount = 0;
    g_originX = content.originX;
    g_originY = content.originY;
    if (content.segments.empty ())
        return true; // a storey with nothing on it: the Present declines, counted
    // Parenthesised: a D3D header has already brought in windows.h's `min` macro.
    const size_t count = (std::min) (content.segments.size (), kMaxSegments);
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = UINT (count * sizeof (plancontent::Segment));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = content.segments.data ();
    if (FAILED (g_device->CreateBuffer (&desc, &data, &g_segments)) || g_segments == nullptr) {
        error = "the plan overlay's segment buffer (" + std::to_string (count) + " segments) could not be created";
        return false;
    }
    g_segmentCount = uint32_t (count);
    return true;
}

// The plan canvas's chain, on ACAPI's thread, before the frame is forwarded.
void Draw (IDXGISwapChain* swapChain)
{
    Transient<ID3D11Device> device;
    swapChain->GetDevice (__uuidof (ID3D11Device), (void**) &device.p);
    if (device.p == nullptr) {
        Declined (Decline::NoDevice);
        return;
    }
    if (g_device == nullptr) {
        g_device = device.p; // held until Release (§12b); never a buffer or a view
        g_device->AddRef ();
    }
    else if (device.p != g_device) {
        g_deviceChanged = true;
        Declined (Decline::DeviceChanged);
        return;
    }

    // ⚠️ THE READ COMES FIRST, BEFORE ANY REASON NOT TO DRAW. It is what keeps the last
    // transform current while the pipeline is still being built, so the first frame
    // drawn -- the runtime's own redraw, whose read is refused -- is not a stale one.
    plancontent::PixelTransform transform;
    int32_t error = 0;
    const TransformReader reader = g_reader.load (std::memory_order_acquire);
    const int64_t readBegan = QpcNow ();
    const Read read = reader != nullptr ? reader (transform, error) : Read::Refused;
    RecordMicros (readBegan, g_readUsLast, g_readUsMax);
    bool reused = false;
    if (read == Read::Fresh) {
        g_last = transform;
        g_haveLast = true;
        Bump (g_readsFresh);
    }
    else {
        Bump (read == Read::Refused ? g_readsRefused : g_readsInvalid);
        if (read == Read::Invalid)
            g_lastReadError.store (error, std::memory_order_relaxed);
        transform = g_last;
        reused = true;
    }

    if (g_builtFor != g_device || g_vs == nullptr) {
        Declined (Decline::NotPrepared);
        return;
    }
    if (g_segments == nullptr || g_segmentCount == 0) {
        Declined (Decline::NoContent);
        return;
    }
    if (!g_haveLast) {
        Declined (Decline::NoTransform);
        return;
    }

    const int64_t drawBegan = QpcNow ();
    Transient<ID3D11Texture2D> backBuffer;
    if (FAILED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer.p)) ||
        backBuffer.p == nullptr) {
        Declined (Decline::BackBuffer);
        return;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer.p->GetDesc (&desc);
    g_bufferWidth.store (desc.Width, std::memory_order_relaxed);
    g_bufferHeight.store (desc.Height, std::memory_order_relaxed);
    if (desc.SampleDesc.Count != 1) {
        Declined (Decline::Multisampled);
        return;
    }
    plancontent::ViewConstants constants;
    if (!plancontent::MakeViewConstants (transform, g_originX, g_originY, desc.Width, desc.Height, constants)) {
        Declined (Decline::Degenerate);
        return;
    }
    constants.colour[0] = g_style.red;
    constants.colour[1] = g_style.green;
    constants.colour[2] = g_style.blue;
    constants.colour[3] = g_style.alpha;
    constants.stroke[0] = g_style.widthPixels * 0.5f;

    Transient<ID3D11DeviceContext> context;
    device.p->GetImmediateContext (&context.p);
    Transient<ID3D11DeviceContext1> context1;
    if (context.p != nullptr)
        context.p->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1.p);
    // ⚠️ WITHOUT THE D3D11.1 CONTEXT THE GUARD CANNOT PUT ARCHICAD'S CONSTANT BUFFERS
    // BACK WITH THEIR WINDOWS, and this draw binds b0. Not drawn, counted.
    if (context.p == nullptr || context1.p == nullptr) {
        Declined (Decline::NoContext1);
        return;
    }
    Transient<ID3D11RenderTargetView> view;
    if (FAILED (device.p->CreateRenderTargetView (backBuffer.p, nullptr, &view.p)) || view.p == nullptr) {
        Declined (Decline::CreateView);
        return;
    }

    {
        contextstate::ScopedInjectionGuard injection;
        const ScopedPipelineState saved (context.p, context1.p);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED (context.p->Map (g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr) {
            Declined (Decline::MapConstants);
            return;
        }
        std::memcpy (mapped.pData, &constants, sizeof (constants));
        context.p->Unmap (g_constants, 0);

        const UINT stride = UINT (sizeof (plancontent::Segment));
        const UINT offset = 0;
        D3D11_VIEWPORT viewport = {};
        viewport.Width = float (desc.Width);
        viewport.Height = float (desc.Height);
        viewport.MaxDepth = 1.0f;
        context.p->OMSetRenderTargets (1, &view.p, nullptr);
        context.p->RSSetViewports (1, &viewport);
        context.p->RSSetState (g_raster);
        context.p->OMSetBlendState (g_blend, nullptr, 0xffffffffu);
        context.p->OMSetDepthStencilState (g_depth, 0);
        context.p->IASetInputLayout (g_layout);
        context.p->IASetVertexBuffers (0, 1, &g_segments, &stride, &offset);
        context.p->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.p->VSSetShader (g_vs, nullptr, 0);
        context.p->VSSetConstantBuffers (0, 1, &g_constants);
        context.p->PSSetShader (g_ps, nullptr, 0);
        context.p->PSSetConstantBuffers (0, 1, &g_constants);
        context.p->GSSetShader (nullptr, nullptr, 0);
        context.p->HSSetShader (nullptr, nullptr, 0);
        context.p->DSSetShader (nullptr, nullptr, 0);
        context.p->DrawInstanced (6, g_segmentCount, 0, 0);
    }
    Bump (g_drawn);
    if (reused)
        Bump (g_drawnWithLastRead);
    RecordMicros (drawBegan, g_drawUsLast, g_drawUsMax);
}

} // namespace

const char* DeclineName (Decline decline)
{
    switch (decline) {
        case Decline::OffThread:
            return "offThread";
        case Decline::NoDevice:
            return "noDevice";
        case Decline::DeviceChanged:
            return "deviceChanged";
        case Decline::NotPrepared:
            return "notPrepared";
        case Decline::NoContent:
            return "noContent";
        case Decline::NoTransform:
            return "noTransform";
        case Decline::Degenerate:
            return "degenerate";
        case Decline::BackBuffer:
            return "backBuffer";
        case Decline::Multisampled:
            return "multisampled";
        case Decline::NoContext1:
            return "noContext1";
        case Decline::CreateView:
            return "createView";
        case Decline::MapConstants:
            return "mapConstants";
        case Decline::Count:
            break;
    }
    return "?";
}

void Arm (uint64_t canvasWindow, uint32_t presentThread, TransformReader reader, const Style& style)
{
    // Every Start resets what every Stop leaves behind (§8).
    g_armed.store (false, std::memory_order_release);
    for (std::atomic<uint64_t>* counter : { &g_chain, &g_canvasPresents, &g_drawn, &g_readsFresh, &g_readsRefused,
                                            &g_readsInvalid, &g_drawnWithLastRead })
        counter->store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& counter : g_declines)
        counter.store (0, std::memory_order_relaxed);
    for (std::atomic<uint32_t>* value :
         { &g_bufferWidth, &g_bufferHeight, &g_readUsLast, &g_readUsMax, &g_drawUsLast, &g_drawUsMax })
        value->store (0, std::memory_order_relaxed);
    g_lastReadError.store (0, std::memory_order_relaxed);
    g_haveLast = false;
    g_last = plancontent::PixelTransform {};
    g_style = style;
    g_reader.store (reader, std::memory_order_relaxed);
    g_presentThread.store (presentThread, std::memory_order_relaxed);
    g_canvas.store (canvasWindow, std::memory_order_relaxed);
    g_armed.store (true, std::memory_order_release);
}

void Disarm ()
{
    g_armed.store (false, std::memory_order_release);
    g_canvas.store (0, std::memory_order_release);
    g_reader.store (nullptr, std::memory_order_release);
}

void Retarget (uint64_t canvasWindow)
{
    g_canvas.store (canvasWindow, std::memory_order_release);
}

void SeedTransform (const plancontent::PixelTransform& transform)
{
    g_last = transform;
    g_haveLast = true;
}

Prepared Prepare (const plancontent::Content& content, uint64_t generation, bool& changed, std::string& error)
{
    changed = false;
    // A chain that presented with another device: everything built for the old one
    // goes, the old device with it, and the next Present names the new one.
    if (g_deviceChanged) {
        ReleasePipeline ();
        ReleaseAndNull (g_device);
        g_failedFor = nullptr;
        g_deviceChanged = false;
    }
    if (g_device == nullptr)
        return Prepared::WaitingForDevice;
    if (g_builtFor != g_device) {
        if (g_failedFor == g_device) {
            error = "the plan overlay's pipeline could not be built on this device";
            return Prepared::Failed;
        }
        if (!BuildPipeline (error)) {
            g_failedFor = g_device;
            return Prepared::Failed;
        }
        changed = true;
    }
    if (!g_haveGeneration || generation != g_generation) {
        if (!Upload (content, error))
            return Prepared::Failed;
        g_generation = generation;
        g_haveGeneration = true;
        changed = true;
    }
    return Prepared::Ready;
}

void Release ()
{
    ReleasePipeline ();
    ReleaseAndNull (g_device);
    g_failedFor = nullptr;
    g_deviceChanged = false;
    g_haveLast = false;
}

void OnPresent (IDXGISwapChain* swapChain)
{
    if (!g_armed.load (std::memory_order_acquire) || swapChain == nullptr)
        return;
    // ⚠️ THE CANVAS'S OWN CHAIN, BY THE WINDOW IT PRESENTS INTO, ASKED EVERY FRAME. A
    // chain Archicad recreates -- on a resize, a device change -- is still the chain
    // presenting into the canvas, and is drawn into from its first Present; a chain
    // nominated once would be forgotten with it (§8).
    const uint64_t chain = uint64_t (uintptr_t (swapChain));
    const uint64_t canvas = g_canvas.load (std::memory_order_acquire);
    if (canvas == 0 || SwapChainWindow (chain) != canvas)
        return;
    Bump (g_canvasPresents);
    g_chain.store (chain, std::memory_order_relaxed);
    // ⚠️ ACAPI'S THREAD OR NOTHING: the read inside the draw is ACAPI, and §11 allows it
    // on this thread alone. The plan has presented on it 942 times out of 942.
    if (uint32_t (::GetCurrentThreadId ()) != g_presentThread.load (std::memory_order_acquire)) {
        Declined (Decline::OffThread);
        return;
    }
    Draw (swapChain);
}

Stats GetStats ()
{
    Stats stats;
    stats.armed = g_armed.load (std::memory_order_acquire);
    stats.deviceKnown = g_device != nullptr;
    stats.prepared = g_device != nullptr && g_builtFor == g_device;
    stats.chain = g_chain.load (std::memory_order_relaxed);
    stats.bufferWidth = g_bufferWidth.load (std::memory_order_relaxed);
    stats.bufferHeight = g_bufferHeight.load (std::memory_order_relaxed);
    stats.segments = g_segmentCount;
    stats.generation = g_haveGeneration ? g_generation : 0;
    stats.canvasPresents = g_canvasPresents.load (std::memory_order_relaxed);
    stats.drawn = g_drawn.load (std::memory_order_relaxed);
    stats.readsFresh = g_readsFresh.load (std::memory_order_relaxed);
    stats.readsRefused = g_readsRefused.load (std::memory_order_relaxed);
    stats.readsInvalid = g_readsInvalid.load (std::memory_order_relaxed);
    stats.drawnWithLastRead = g_drawnWithLastRead.load (std::memory_order_relaxed);
    stats.lastReadError = g_lastReadError.load (std::memory_order_relaxed);
    for (size_t i = 0; i < size_t (Decline::Count); ++i)
        stats.declines[i] = g_declines[i].load (std::memory_order_relaxed);
    stats.readUsLast = g_readUsLast.load (std::memory_order_relaxed);
    stats.readUsMax = g_readUsMax.load (std::memory_order_relaxed);
    stats.drawUsLast = g_drawUsLast.load (std::memory_order_relaxed);
    stats.drawUsMax = g_drawUsMax.load (std::memory_order_relaxed);
    return stats;
}

void TakeMaxima ()
{
    g_readUsMax.store (0, std::memory_order_relaxed);
    g_drawUsMax.store (0, std::memory_order_relaxed);
}

} // namespace planlayer
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
