// ArchViz/Dxgi/DepthCheckpoints -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/DepthCheckpoints.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/GhostMesh.hpp"
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
    ID3D11Query* front = nullptr;
    ID3D11Query* behind = nullptr;
    ID3D11Query* ghost = nullptr;
    bool pending = false;
    Result result;
};

ID3D11Device* g_device = nullptr;
Slot g_slots[kCapacity];
Stats g_stats;

// ⚠️ OUR OWN STATES, AND BOTH MASKS ARE ZERO. Depth writes off so the probe
// cannot change what Archicad is drawing against; colour writes off so it cannot
// put a pixel on screen. The occlusion query still counts the samples that would
// have passed, which is the entire measurement.
ID3D11DepthStencilState* g_testOnly = nullptr;
ID3D11BlendState* g_noColour = nullptr;
bool g_created = false;
bool g_createFailed = false;

std::atomic<bool> g_enabled { false };
std::atomic<uint32_t> g_interval { 10 };

uint64_t g_frameGeneration = 0;
uint64_t g_modelFramesSeen = 0;
bool g_instrumenting = false;
uint32_t g_ordinal = 0;

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

bool EnsureCreated (ID3D11DeviceContext* context)
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

    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    bool ok = SUCCEEDED (g_device->CreateDepthStencilState (&depthDesc, &g_testOnly));

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
    ok = ok && SUCCEEDED (g_device->CreateBlendState (&blendDesc, &g_noColour));

    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_OCCLUSION;
    for (uint32_t i = 0; i < kCapacity && ok; ++i) {
        ok = ok && SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_slots[i].front));
        ok = ok && SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_slots[i].behind));
        ok = ok && SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_slots[i].ghost));
    }
    if (!ok) {
        Fail ("the checkpoint queries could not be created");
        g_createFailed = true;
        return false;
    }
    g_created = true;
    g_stats.ready = true;
    return true;
}

// Everything about the draw that just executed, so the report can name the
// boundary once the samples have located it.
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
    const uint32_t clamped = interval < 1 ? 1u : interval;
    g_interval.store (clamped, std::memory_order_release);
    g_stats.interval = clamped;
}

void Reset ()
{
    for (uint32_t i = 0; i < kCapacity; ++i) {
        g_slots[i].result = Result {};
        g_slots[i].pending = false;
    }
    const Stats keep = g_stats;
    g_stats = Stats {};
    g_stats.interval = keep.interval;
    g_stats.ready = keep.ready;
    g_stats.enabled = keep.enabled;
}

void OnDrawCompleted (ID3D11DeviceContext* context, uint32_t drawKind, uint32_t indexCount, uint64_t modelGeneration)
{
    if (!Enabled () || context == nullptr)
        return;
    // ⚠️ OUR OWN PROBE DRAWS REACH THIS SAME DETOUR. Without the guard's test the
    // measurement would measure itself, recursively.
    if (contextstate::Injecting ())
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
        g_stats.drawsLastFrame = g_ordinal;
        g_ordinal = 0;
        g_instrumenting = (g_modelFramesSeen % g_interval.load (std::memory_order_acquire)) == 0;
        if (g_instrumenting)
            ++g_stats.framesInstrumented;
    }
    if (!g_instrumenting)
        return;

    const uint32_t ordinal = g_ordinal++;
    if (ordinal >= kCapacity) {
        ++g_stats.overflowed;
        return;
    }
    Slot& slot = g_slots[ordinal];
    if (slot.pending)
        return;

    const probes::WorldProbePipeline pipeline = probes::GetWorldProbePipeline ();
    if (!pipeline.valid) {
        ++g_stats.skippedNoPipeline;
        return;
    }
    ID3D11Buffer* const viewBuffer = ViewSnapshotBuffer ();
    ID3D11Buffer* const projectionBuffer = ProjectionSnapshotBuffer ();
    if (viewBuffer == nullptr || projectionBuffer == nullptr || !SnapshotValid ()) {
        ++g_stats.skippedNoCamera;
        return;
    }

    contextstate::ScopedInjectionGuard guard;
    if (!EnsureCreated (context))
        return;

    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) || context1 == nullptr)
        return;

    {
        const ScopedPipelineState saved (context, context1);

        // ⚠️ ARCHICAD'S OWN TARGETS, EXACTLY AS THEY ARE. The probe tests against
        // the live depth buffer at this instant -- that is the whole point -- and
        // writes to neither it nor the colour target.
        const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState (g_noColour, blendFactor, 0xffffffffu);
        context->OMSetDepthStencilState (g_testOnly, 0);
        context->RSSetState (pipeline.raster);
        context->PSSetShader (pipeline.pixel, nullptr, 0);

        ID3D11Buffer* const cameraBuffers[2] = { viewBuffer, projectionBuffer };
        const UINT cameraFirst[2] = { 0, 0 };
        const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
        context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);

        // ---- FRONT and BEHIND: the point test, kept for continuity ----------
        ID3D11Buffer* const noBuffer = nullptr;
        const UINT zero = 0;
        context->IASetInputLayout (nullptr);
        context->IASetVertexBuffers (0, 1, &noBuffer, &zero, &zero);
        context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        context->VSSetShader (pipeline.front, nullptr, 0);
        context->Begin (slot.front);
        context->Draw (3, 0);
        context->End (slot.front);

        context->VSSetShader (pipeline.behind, nullptr, 0);
        context->Begin (slot.behind);
        context->Draw (3, 0);
        context->End (slot.behind);

        // ---- GHOST: the spatial test, and the one that matters --------------
        // ⚠️ THE WHOLE MESH, NOT A PRIMITIVE NEAR THE ANCHOR. Run fifty's point
        // probes reported perfect behaviour at every checkpoint while the overlay
        // was visibly wrong, because a build plane can leave the anchor alone and
        // blank everything around it. This is the same geometry the user is
        // looking at, so its sample count is the same question they are asking.
        context->Begin (slot.ghost);
        ghost::DrawCurrent (context, ExpectedInterpretation (), g_testOnly, pipeline.raster, g_noColour);
        context->End (slot.ghost);

        RecordAttributes (context, slot.result.attributes, drawKind, indexCount, ordinal);
        slot.pending = true;
        g_stats.queriesIssued += 3;
        ++g_stats.roundsIssued;
        if (ordinal + 1 > g_stats.used)
            g_stats.used = ordinal + 1;
    }
    ReleaseAndNull (context1);
}

void Resolve (ID3D11DeviceContext* context)
{
    if (context == nullptr)
        return;
    for (uint32_t i = 0; i < kCapacity; ++i) {
        Slot& slot = g_slots[i];
        if (!slot.pending)
            continue;
        UINT64 front = 0, behind = 0, ghost = 0;
        // ⚠️ `DONOTFLUSH`, SO A PENDING QUERY COSTS ARCHICAD NOTHING. A result
        // that is not ready is left for a later frame; a measurement may never
        // stall the render thread to take one.
        if (context->GetData (slot.front, &front, sizeof (front), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (context->GetData (slot.behind, &behind, sizeof (behind), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (context->GetData (slot.ghost, &ghost, sizeof (ghost), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        slot.pending = false;
        g_stats.queriesResolved += 3;
        ++slot.result.rounds;
        slot.result.frontSamples += uint64_t (front);
        slot.result.behindSamples += uint64_t (behind);
        slot.result.ghostSamples += uint64_t (ghost);
        if (front > 0)
            ++slot.result.frontSurvived;
        if (behind > 0)
            ++slot.result.behindSurvived;
        if (ghost > 0)
            ++slot.result.ghostSurvived;
    }
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
        ReleaseAndNull (g_slots[i].ghost);
        ReleaseAndNull (g_slots[i].behind);
        ReleaseAndNull (g_slots[i].front);
        g_slots[i] = Slot {};
    }
    ReleaseAndNull (g_noColour);
    ReleaseAndNull (g_testOnly);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_stats = Stats {};
    g_instrumenting = false;
    g_ordinal = 0;
    g_modelFramesSeen = 0;
}

} // namespace checkpoints
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
