// ArchViz/Dxgi/InjectionProbes -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/InjectionProbes.hpp"

#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace probes {

namespace {

constexpr size_t kQueryRing = 8;
constexpr UINT   kCameraWindowConstants = 16;

// ⚠️ THE ANCHOR IS BAKED INTO THE SHADER AT COMPILE TIME RATHER THAN BOUND IN A
// CONSTANT BUFFER OF OURS. Probe B exists to test the camera constants; giving
// it a second constant buffer would add another thing that could be wrong to the
// very experiment meant to isolate one. The shader is compiled at runtime
// anyway, so the three world vertices are literals by the time the GPU sees them.
//
// ⚠️ `View` IS `row_major` AND `Projection` IS `column_major`, EXACTLY AS THE
// PRODUCTION SHADER DECLARES THEM. That pair IS interpretation 2 -- `p * V * Pt`
// -- and if probe B's declaration drifted from the production one, probe B would
// stop testing the thing it exists to test.
const char* const kProbeSourceFormat =
"cbuffer ArchicadView : register (b1)       { %s float4x4 View; };\n"
"cbuffer ArchicadProjection : register (b2) { %s float4x4 Projection; };\n"
"\n"
"// Probe A: clip space from the vertex id alone. No vertex buffer, no input\n"
"// layout, no constant buffer, no camera -- the render target, the viewport,\n"
"// the rasterizer, the pixel shader and the write mask, and nothing else.\n"
"float4 VSProbeA (uint id : SV_VertexID) : SV_POSITION\n"
"{\n"
"    float2 corners[3] = { float2 (-0.08, -0.08), float2 (0.08, -0.08),\n"
"                          float2 (0.0, 0.10) };\n"
"    return float4 (corners[id], 0.5, 1.0);\n"
"}\n"
"\n"
"// Probe B: the SAME three world points the production vertex buffer holds,\n"
"// generated here instead of fetched, then put through the production camera.\n"
"float4 VSProbeB (uint id : SV_VertexID) : SV_POSITION\n"
"{\n"
"    float3 corners[3] = { float3 (%.6ff, %.6ff, %.6ff),\n"
"                          float3 (%.6ff, %.6ff, %.6ff),\n"
"                          float3 (%.6ff, %.6ff, %.6ff) };\n"
"    float4 p = float4 (corners[id], 1.0);\n"
"    p = mul (p, View);\n"
"    p = mul (p, Projection);\n"
"    return p;\n"
"}\n"
"\n"
"// Proof B's two test primitives, each its OWN shader.\n"
"// \n"
"// THE SELECTOR IS THE SHADER, NOT `StartVertexLocation`. Run forty-one drew\n"
"// these as Draw(3,3) and Draw(3,6) into one nine-vertex array and got BYTE-\n"
"// IDENTICAL sample counts from primitives eight metres apart -- SV_VertexID did\n"
"// not carry the start location, so all three draws rendered the anchor triangle\n"
"// and the depth comparison was run against itself. Three entry points cannot\n"
"// have that bug.\n"
"float4 VSProbeFront (uint id : SV_VertexID) : SV_POSITION\n"
"{\n"
"    float3 corners[3] = { float3 (%.6ff, %.6ff, %.6ff),\n"
"                          float3 (%.6ff, %.6ff, %.6ff),\n"
"                          float3 (%.6ff, %.6ff, %.6ff) };\n"
"    float4 p = float4 (corners[id], 1.0);\n"
"    p = mul (p, View);\n"
"    p = mul (p, Projection);\n"
"    return p;\n"
"}\n"
"float4 VSProbeBehind (uint id : SV_VertexID) : SV_POSITION\n"
"{\n"
"    float3 corners[3] = { float3 (%.6ff, %.6ff, %.6ff),\n"
"                          float3 (%.6ff, %.6ff, %.6ff),\n"
"                          float3 (%.6ff, %.6ff, %.6ff) };\n"
"    float4 p = float4 (corners[id], 1.0);\n"
"    p = mul (p, View);\n"
"    p = mul (p, Projection);\n"
"    return p;\n"
"}\n"
"\n"
"float4 PSProbeA () : SV_TARGET { return float4 (1.0, 0.15, 0.85, 1.0); }\n"
"float4 PSProbeB () : SV_TARGET { return float4 (0.1, 0.8, 1.0, 1.0); }\n"
"float4 PSProbeC () : SV_TARGET { return float4 (1.0, 0.9, 0.1, 1.0); }\n";

float g_anchorX = 0.0f;
float g_anchorY = 0.0f;
float g_anchorZ = 0.0f;
float g_anchorSize = 1.0f;

// ⚠️ AN ASSUMPTION ABOUT THE MODEL, AND MEANT TO BE CHANGED. "In front of" and
// "behind" only mean anything against geometry that is actually there: the
// default lifts one triangle above the anchor and sinks the other below the
// slab, which is right for a building and wrong for something else.
float g_frontOffsetZ = 2.0f;
float g_behindOffsetZ = -6.0f;

ID3D11Device*            g_device = nullptr;
ID3D11VertexShader*      g_vsProbeA = nullptr;
ID3D11VertexShader*      g_vsProbeB = nullptr;        // the bound variant
ID3D11VertexShader*      g_vsProbeBVariant[4] = {};   // one per declaration
uint64_t                 g_probeHash[4] = {};
ID3D11PixelShader*       g_psProbeA = nullptr;
ID3D11PixelShader*       g_psProbeB = nullptr;
ID3D11PixelShader*       g_psProbeC = nullptr;
ID3D11VertexShader*      g_vsFront = nullptr;
ID3D11VertexShader*      g_vsBehind = nullptr;

// ⚠️ THE MODEL'S OWN DEPTH VIEW, HELD ACROSS THE FRAME. Proof B2 asks whether it
// is still usable at Present; the reference is dropped the moment a different
// one arrives and at every teardown, because holding a view of a resource
// Archicad may resize is the mistake the back-buffer path exists to refuse.
ID3D11DepthStencilView*  g_sceneDepthView = nullptr;
ID3D11DepthStencilState* g_depthTestState = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState*   g_raster = nullptr;
ID3D11BlendState*        g_blend = nullptr;

struct QuerySet {
    ID3D11Query* query[kQueryRing] = {};
    bool         busy[kQueryRing] = {};
    size_t       next = 0;
    int          open = -1;
};
QuerySet g_queries[kProbeCount];
Stats    g_stats;

bool g_created = false;
bool g_createFailed = false;

template <typename T>
void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

void Fail (const char* what)
{
    g_createFailed = true;
    strncpy_s (g_stats.lastError, sizeof (g_stats.lastError), what, _TRUNCATE);
}

// FNV-1a over the compiled bytecode. ⚠️ IT IS AN IDENTITY, NOT A CHECKSUM: the
// only question it answers is whether two shaders are the same shader, and
// whether the one running today is the one that ran yesterday.
uint64_t HashBlob (ID3DBlob* blob)
{
    if (blob == nullptr)
        return 0;
    const unsigned char* bytes = (const unsigned char*) blob->GetBufferPointer ();
    const size_t size = blob->GetBufferSize ();
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= uint64_t (bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool EnsureCreated (ID3D11DeviceContext* context)
{
    if (g_created)
        return true;
    if (g_createFailed || context == nullptr)
        return false;

    context->GetDevice (&g_device);
    if (g_device == nullptr) {
        Fail ("the context has no device");
        return false;
    }

    // ⚠️ THE SAME FOUR DECLARATIONS THE PRODUCTION SHADER COMPILES, for the same
    // reason. Probe B exists to test what the compiled camera semantics actually
    // do, so it must be built from the declaration the production draw is bound
    // to -- not from a copy of it that could drift.
    const char* const layouts[2] = { "row_major", "column_major" };
    char sources[4][4096] = {};
    for (uint32_t variant = 0; variant < 4; ++variant) {
        _snprintf_s (sources[variant], sizeof (sources[variant]), _TRUNCATE,
                     kProbeSourceFormat, layouts[variant & 1u],
                     layouts[(variant >> 1) & 1u],
                     // 0..2 the anchor triangle, as before
                     g_anchorX, g_anchorY, g_anchorZ,
                     g_anchorX + g_anchorSize, g_anchorY, g_anchorZ,
                     g_anchorX, g_anchorY + g_anchorSize, g_anchorZ,
                     // 3..5 FRONT: lifted clear of the model
                     g_anchorX, g_anchorY, g_anchorZ + g_frontOffsetZ,
                     g_anchorX + g_anchorSize, g_anchorY, g_anchorZ + g_frontOffsetZ,
                     g_anchorX, g_anchorY + g_anchorSize, g_anchorZ + g_frontOffsetZ,
                     // 6..8 BEHIND: sunk into it
                     g_anchorX, g_anchorY, g_anchorZ + g_behindOffsetZ,
                     g_anchorX + g_anchorSize, g_anchorY, g_anchorZ + g_behindOffsetZ,
                     g_anchorX, g_anchorY + g_anchorSize, g_anchorZ + g_behindOffsetZ);
    }
    const char* const source = sources[0];

    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const SIZE_T length = strlen (source);
    ID3DBlob* blobs[7] = {};
    ID3DBlob* errors = nullptr;
    const char* const entries[7] = { "VSProbeA", "VSProbeB", "PSProbeA", "PSProbeB",
                                     "PSProbeC", "VSProbeFront", "VSProbeBehind" };
    const char* const targets[7] = { "vs_5_0", "vs_5_0", "ps_5_0", "ps_5_0", "ps_5_0",
                                     "vs_5_0", "vs_5_0" };

    bool ok = true;
    for (int i = 0; i < 7 && ok; ++i) {
        if (FAILED (D3DCompile (source, length, "TapiocaProbes", nullptr, nullptr,
                                entries[i], targets[i], flags, 0, &blobs[i], &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                    : "a probe shader would not compile");
            ok = false;
        }
        ReleaseAndNull (errors);
    }
    if (ok) {
        ok = SUCCEEDED (g_device->CreateVertexShader (blobs[0]->GetBufferPointer (),
                     blobs[0]->GetBufferSize (), nullptr, &g_vsProbeA)) &&
             SUCCEEDED (g_device->CreateVertexShader (blobs[1]->GetBufferPointer (),
                     blobs[1]->GetBufferSize (), nullptr, &g_vsProbeBVariant[0])) &&
             SUCCEEDED (g_device->CreatePixelShader (blobs[2]->GetBufferPointer (),
                     blobs[2]->GetBufferSize (), nullptr, &g_psProbeA)) &&
             SUCCEEDED (g_device->CreatePixelShader (blobs[3]->GetBufferPointer (),
                     blobs[3]->GetBufferSize (), nullptr, &g_psProbeB)) &&
             SUCCEEDED (g_device->CreatePixelShader (blobs[4]->GetBufferPointer (),
                     blobs[4]->GetBufferSize (), nullptr, &g_psProbeC)) &&
             SUCCEEDED (g_device->CreateVertexShader (blobs[5]->GetBufferPointer (),
                     blobs[5]->GetBufferSize (), nullptr, &g_vsFront)) &&
             SUCCEEDED (g_device->CreateVertexShader (blobs[6]->GetBufferPointer (),
                     blobs[6]->GetBufferSize (), nullptr, &g_vsBehind));
        g_stats.rasterVsHash = HashBlob (blobs[0]);
        g_probeHash[0] = HashBlob (blobs[1]);
        g_stats.probeVsHash = g_probeHash[0];
        for (uint32_t variant = 1; variant < 4 && ok; ++variant) {
            ID3DBlob* blob = nullptr;
            if (FAILED (D3DCompile (sources[variant], strlen (sources[variant]),
                                    "TapiocaProbes", nullptr, nullptr, "VSProbeB",
                                    "vs_5_0", flags, 0, &blob, &errors))) {
                Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                        : "a probe shader variant would not compile");
                ok = false;
            } else {
                ok = SUCCEEDED (g_device->CreateVertexShader (blob->GetBufferPointer (),
                        blob->GetBufferSize (), nullptr, &g_vsProbeBVariant[variant]));
                g_probeHash[variant] = HashBlob (blob);
            }
            ReleaseAndNull (errors);
            ReleaseAndNull (blob);
        }
        g_vsProbeB = g_vsProbeBVariant[0];
    }
    for (int i = 0; i < 7; ++i)
        ReleaseAndNull (blobs[i]);
    if (!ok) {
        if (g_stats.lastError[0] == 0)
            Fail ("a probe shader object could not be created");
        return false;
    }

    // ⚠️ THE PROBES OWN THEIR STATE OBJECTS RATHER THAN BORROWING THE
    // RENDERER'S. A diagnostic that shares state with the thing it is testing
    // cannot exonerate it. Same settings, separate objects: depth off, cull
    // none, scissor OFF -- a correct transform clipped by a leftover helper
    // scissor produces exactly zero pixels and looks identical to a wrong camera
    // -- no blending, and every colour channel written.
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_OCCLUSION;

    // ⚠️ DEPTH TEST ON, DEPTH WRITES OFF. The experiment may READ Archicad's
    // depth buffer and may never change a pixel of it, so a failed Proof B
    // cannot corrupt the frame it is measured in. `LESS_EQUAL` is the ordinary
    // convention and equal depths are kept, which is what a coincident surface
    // needs.
    D3D11_DEPTH_STENCIL_DESC testDesc = {};
    testDesc.DepthEnable = TRUE;
    testDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    testDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

    ok = SUCCEEDED (g_device->CreateDepthStencilState (&testDesc, &g_depthTestState)) &&
         SUCCEEDED (g_device->CreateDepthStencilState (&depthDesc, &g_depthState)) &&
         SUCCEEDED (g_device->CreateRasterizerState (&rasterDesc, &g_raster)) &&
         SUCCEEDED (g_device->CreateBlendState (&blendDesc, &g_blend));
    for (size_t p = 0; p < kProbeCount && ok; ++p) {
        for (size_t i = 0; i < kQueryRing && ok; ++i)
            ok = SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_queries[p].query[i]));
    }
    if (!ok) {
        Fail ("a probe state object or query could not be created");
        return false;
    }
    g_created = true;
    g_stats.ready = true;
    return true;
}

// Collect whatever has arrived. Never waits: `S_FALSE` simply means "not yet".
void PollQueries (ID3D11DeviceContext* context)
{
    for (size_t p = 0; p < kProbeCount; ++p) {
        QuerySet& set = g_queries[p];
        for (size_t i = 0; i < kQueryRing; ++i) {
            if (!set.busy[i] || int (i) == set.open)
                continue;
            UINT64 samples = 0;
            const HRESULT hr = context->GetData (set.query[i], &samples, sizeof (samples),
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr != S_OK)
                continue;
            set.busy[i] = false;
            ++g_stats.probe[p].queriesResolved;
            g_stats.probe[p].totalSamples += uint64_t (samples);
            if (samples > 0)
                ++g_stats.probe[p].drawsWithSamples;
        }
    }
}

void BeginQuery (ID3D11DeviceContext* context, size_t probe)
{
    QuerySet& set = g_queries[probe];
    set.open = -1;
    for (size_t attempt = 0; attempt < kQueryRing; ++attempt) {
        const size_t slot = (set.next + attempt) % kQueryRing;
        if (set.busy[slot])
            continue;
        set.next = (slot + 1) % kQueryRing;
        set.open = int (slot);
        context->Begin (set.query[slot]);
        return;
    }
    // Every slot is still in flight. The probe is drawn anyway -- an unmeasured
    // draw is better than a skipped one, and the counts say how many.
}

void EndQuery (ID3D11DeviceContext* context, size_t probe)
{
    QuerySet& set = g_queries[probe];
    ++g_stats.probe[probe].draws;
    if (set.open < 0)
        return;
    context->End (set.query[set.open]);
    set.busy[set.open] = true;
    set.open = -1;
    ++g_stats.probe[probe].queriesIssued;
}

}   // namespace

void RetainSceneDepthView (ID3D11DepthStencilView* view)
{
    if (g_sceneDepthView == view)
        return;
    ReleaseAndNull (g_sceneDepthView);
    g_sceneDepthView = view;
    if (g_sceneDepthView != nullptr)
        g_sceneDepthView->AddRef ();
}

void DrawPresentDepth (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
                       ID3D11RenderTargetView* targetView, float viewportX,
                       float viewportY, float viewportWidth, float viewportHeight)
{
    if (context == nullptr || context1 == nullptr || targetView == nullptr)
        return;
    if (!g_created)
        return;
    if (g_sceneDepthView == nullptr) {
        ++g_stats.presentDepthNoView;
        return;
    }
    ID3D11Buffer* const view = ViewSnapshotBuffer ();
    ID3D11Buffer* const projection = ProjectionSnapshotBuffer ();
    if (view == nullptr || projection == nullptr || !SnapshotValid ())
        return;
    ++g_stats.presentDepthDraws;

    ID3D11VertexShader* savedVs = nullptr;
    ID3D11PixelShader*  savedPs = nullptr;
    ID3D11InputLayout*  savedLayout = nullptr;
    ID3D11Buffer*       savedVertexBuffer = nullptr;
    UINT savedStride = 0;
    UINT savedOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11DepthStencilState* savedDepth = nullptr;
    UINT savedStencilRef = 0;
    ID3D11RasterizerState* savedRaster = nullptr;
    ID3D11BlendState* savedBlend = nullptr;
    FLOAT savedBlendFactor[4] = {};
    UINT savedSampleMask = 0;
    ID3D11Buffer* savedCb[2] = {};
    UINT savedFirst[2] = {};
    UINT savedNum[2] = {};
    ID3D11RenderTargetView* savedRtv = nullptr;
    ID3D11DepthStencilView* savedDsv = nullptr;
    D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    context->VSGetShader (&savedVs, nullptr, nullptr);
    context->PSGetShader (&savedPs, nullptr, nullptr);
    context->IAGetInputLayout (&savedLayout);
    context->IAGetVertexBuffers (0, 1, &savedVertexBuffer, &savedStride, &savedOffset);
    context->IAGetPrimitiveTopology (&savedTopology);
    context->OMGetDepthStencilState (&savedDepth, &savedStencilRef);
    context->RSGetState (&savedRaster);
    context->OMGetBlendState (&savedBlend, savedBlendFactor, &savedSampleMask);
    context->RSGetViewports (&savedViewportCount, savedViewports);
    context1->VSGetConstantBuffers1 (1, 2, savedCb, savedFirst, savedNum);
    context->OMGetRenderTargets (1, &savedRtv, &savedDsv);

    // ⚠️ THE BACK BUFFER AND THE MODEL'S DEPTH, TOGETHER. This is the whole
    // experiment: if D3D accepts the pair and the depth still holds the model,
    // the overlay can be both persistent and occluded.
    context->OMSetRenderTargets (1, &targetView, g_sceneDepthView);
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = viewportX;
    viewport.TopLeftY = viewportY;
    viewport.Width = viewportWidth;
    viewport.Height = viewportHeight;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    if (viewport.Width > 1.0f && viewport.Height > 1.0f)
        context->RSSetViewports (1, &viewport);
    context->RSSetState (g_raster);
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_blend, blendFactor, 0xffffffffu);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* const noBuffer = nullptr;
    const UINT zero = 0;
    context->IASetInputLayout (nullptr);
    context->IASetVertexBuffers (0, 1, &noBuffer, &zero, &zero);
    ID3D11Buffer* const cameraBuffers[2] = { view, projection };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);
    context->OMSetDepthStencilState (g_depthTestState, 0);

    context->VSSetShader (g_vsFront, nullptr, 0);
    context->PSSetShader (g_psProbeB, nullptr, 0);
    BeginQuery (context, size_t (Probe::PresentFrontDepth));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::PresentFrontDepth));
    context->VSSetShader (g_vsBehind, nullptr, 0);
    context->PSSetShader (g_psProbeC, nullptr, 0);
    BeginQuery (context, size_t (Probe::PresentBehindDepth));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::PresentBehindDepth));

    context->OMSetRenderTargets (1, &savedRtv, savedDsv);
    context1->VSSetConstantBuffers1 (1, 2, savedCb, savedFirst, savedNum);
    context->VSSetShader (savedVs, nullptr, 0);
    context->PSSetShader (savedPs, nullptr, 0);
    context->IASetInputLayout (savedLayout);
    context->IASetVertexBuffers (0, 1, &savedVertexBuffer, &savedStride, &savedOffset);
    context->IASetPrimitiveTopology (savedTopology);
    context->OMSetDepthStencilState (savedDepth, savedStencilRef);
    context->RSSetState (savedRaster);
    context->OMSetBlendState (savedBlend, savedBlendFactor, savedSampleMask);
    if (savedViewportCount > 0)
        context->RSSetViewports (savedViewportCount, savedViewports);

    for (int i = 0; i < 2; ++i)
        ReleaseAndNull (savedCb[i]);
    ReleaseAndNull (savedVs);
    ReleaseAndNull (savedPs);
    ReleaseAndNull (savedLayout);
    ReleaseAndNull (savedVertexBuffer);
    ReleaseAndNull (savedDepth);
    ReleaseAndNull (savedRaster);
    ReleaseAndNull (savedBlend);
    ReleaseAndNull (savedRtv);
    ReleaseAndNull (savedDsv);
}

void SetDepthOffsets (float frontOffsetZ, float behindOffsetZ)
{
    g_frontOffsetZ = frontOffsetZ;
    g_behindOffsetZ = behindOffsetZ;
    // The offsets are literals in the shader, so new offsets are a new shader.
    for (uint32_t variant = 0; variant < 4; ++variant)
        ReleaseAndNull (g_vsProbeBVariant[variant]);
    g_vsProbeB = nullptr;
    ReleaseAndNull (g_vsFront);
    ReleaseAndNull (g_vsBehind);
    g_created = false;
    g_createFailed = false;
    g_stats.lastError[0] = 0;
}

void DrawDepthProof (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1)
{
    if (context == nullptr || context1 == nullptr)
        return;
    if (!EnsureCreated (context))
        return;
    ID3D11Buffer* const view = ViewSnapshotBuffer ();
    ID3D11Buffer* const projection = ProjectionSnapshotBuffer ();
    if (view == nullptr || projection == nullptr || !SnapshotValid ())
        return;

    // ⚠️ WHATEVER IS BOUND RIGHT NOW IS ARCHICAD'S OWN, and that is the entire
    // point of injecting here rather than at Present. The depth view is not
    // created, not cleared and not written -- it is read, and put back exactly
    // as it was found.
    ID3D11RenderTargetView* boundRtv = nullptr;
    ID3D11DepthStencilView* boundDsv = nullptr;
    context->OMGetRenderTargets (1, &boundRtv, &boundDsv);
    ++g_stats.depthInjections;
    if (boundDsv == nullptr) {
        ++g_stats.depthNoView;
        ReleaseAndNull (boundRtv);
        return;
    }
    const uint64_t dsvId = uint64_t (uintptr_t (boundDsv));
    if (g_stats.lastDepthView != 0 && g_stats.lastDepthView != dsvId)
        ++g_stats.depthViewChanged;
    g_stats.lastDepthView = dsvId;
    // Proof B2 needs this view at Present; keep a reference while it is current.
    RetainSceneDepthView (boundDsv);
    // ⚠️ AND THE PRODUCTION PATH GETS IT TOO. Proof B2 showed this view is still
    // valid at Present, so the real overlay -- not just the probes -- can test
    // against it there. See InjectionDepth.hpp.
    depth::RetainSceneView (boundDsv);

    // ---- save what this touches -------------------------------------------
    ID3D11VertexShader* savedVs = nullptr;
    ID3D11PixelShader*  savedPs = nullptr;
    ID3D11InputLayout*  savedLayout = nullptr;
    ID3D11Buffer*       savedVertexBuffer = nullptr;
    UINT savedStride = 0;
    UINT savedOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11DepthStencilState* savedDepth = nullptr;
    UINT savedStencilRef = 0;
    ID3D11RasterizerState* savedRaster = nullptr;
    ID3D11BlendState* savedBlend = nullptr;
    FLOAT savedBlendFactor[4] = {};
    UINT savedSampleMask = 0;
    ID3D11Buffer* savedCb[2] = {};
    UINT savedFirst[2] = {};
    UINT savedNum[2] = {};

    context->VSGetShader (&savedVs, nullptr, nullptr);
    context->PSGetShader (&savedPs, nullptr, nullptr);
    context->IAGetInputLayout (&savedLayout);
    context->IAGetVertexBuffers (0, 1, &savedVertexBuffer, &savedStride, &savedOffset);
    context->IAGetPrimitiveTopology (&savedTopology);
    context->OMGetDepthStencilState (&savedDepth, &savedStencilRef);
    context->RSGetState (&savedRaster);
    context->OMGetBlendState (&savedBlend, savedBlendFactor, &savedSampleMask);
    context1->VSGetConstantBuffers1 (1, 2, savedCb, savedFirst, savedNum);

    // ⚠️ THE VIEWPORT IS ARCHICAD'S OWN AND IS NOT TOUCHED. We are inside its
    // scene pass; the viewport that drew the model is the viewport we want.
    ID3D11Buffer* const noBuffer = nullptr;
    const UINT zero = 0;
    context->IASetInputLayout (nullptr);
    context->IASetVertexBuffers (0, 1, &noBuffer, &zero, &zero);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (g_vsProbeB, nullptr, 0);
    context->RSSetState (g_raster);
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_blend, blendFactor, 0xffffffffu);
    ID3D11Buffer* const cameraBuffers[2] = { view, projection };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);

    PollQueries (context);

    // ---- the control: depth OFF, so both must rasterise --------------------
    // ⚠️ WITHOUT THIS PAIR THE DEPTH RESULT MEANS NOTHING. A primitive that is
    // off screen, backfacing or outside the frustum produces no samples with
    // depth on OR off, and would read as "correctly occluded".
    context->OMSetDepthStencilState (g_depthState, 0);
    context->VSSetShader (g_vsFront, nullptr, 0);
    context->PSSetShader (g_psProbeB, nullptr, 0);
    BeginQuery (context, size_t (Probe::FrontDepthOff));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::FrontDepthOff));
    context->VSSetShader (g_vsBehind, nullptr, 0);
    context->PSSetShader (g_psProbeC, nullptr, 0);
    BeginQuery (context, size_t (Probe::BehindDepthOff));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::BehindDepthOff));

    // ---- the experiment: depth ON against Archicad's own buffer ------------
    context->OMSetDepthStencilState (g_depthTestState, 0);
    context->VSSetShader (g_vsFront, nullptr, 0);
    context->PSSetShader (g_psProbeB, nullptr, 0);
    BeginQuery (context, size_t (Probe::FrontDepthOn));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::FrontDepthOn));
    context->VSSetShader (g_vsBehind, nullptr, 0);
    context->PSSetShader (g_psProbeC, nullptr, 0);
    BeginQuery (context, size_t (Probe::BehindDepthOn));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::BehindDepthOn));

    // ---- put everything back ----------------------------------------------
    context1->VSSetConstantBuffers1 (1, 2, savedCb, savedFirst, savedNum);
    context->VSSetShader (savedVs, nullptr, 0);
    context->PSSetShader (savedPs, nullptr, 0);
    context->IASetInputLayout (savedLayout);
    context->IASetVertexBuffers (0, 1, &savedVertexBuffer, &savedStride, &savedOffset);
    context->IASetPrimitiveTopology (savedTopology);
    context->OMSetDepthStencilState (savedDepth, savedStencilRef);
    context->RSSetState (savedRaster);
    context->OMSetBlendState (savedBlend, savedBlendFactor, savedSampleMask);

    for (int i = 0; i < 2; ++i)
        ReleaseAndNull (savedCb[i]);
    ReleaseAndNull (savedVs);
    ReleaseAndNull (savedPs);
    ReleaseAndNull (savedLayout);
    ReleaseAndNull (savedVertexBuffer);
    ReleaseAndNull (savedDepth);
    ReleaseAndNull (savedRaster);
    ReleaseAndNull (savedBlend);
    ReleaseAndNull (boundDsv);
    ReleaseAndNull (boundRtv);
}

void SetAnchor (float x, float y, float z, float sizeMetres)
{
    g_anchorX = x;
    g_anchorY = y;
    g_anchorZ = z;
    g_anchorSize = (sizeMetres > 0.001f) ? sizeMetres : 1.0f;
    // The vertices are literals in the shader, so a new anchor is a new shader.
    for (uint32_t variant = 0; variant < 4; ++variant)
        ReleaseAndNull (g_vsProbeBVariant[variant]);
    g_vsProbeB = nullptr;
    ReleaseAndNull (g_vsFront);
    ReleaseAndNull (g_vsBehind);
    g_created = false;
    g_createFailed = false;
    g_stats.lastError[0] = 0;
}

void DrawAll (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
              ID3D11RenderTargetView* targetView, float viewportX, float viewportY,
              float viewportWidth, float viewportHeight)
{
    if (context == nullptr || context1 == nullptr || targetView == nullptr)
        return;
    if (!EnsureCreated (context))
        return;

    ID3D11Buffer* const view = ViewSnapshotBuffer ();
    ID3D11Buffer* const projection = ProjectionSnapshotBuffer ();
    if (view == nullptr || projection == nullptr)
        return;

    // ---- save everything this function touches -----------------------------
    ID3D11VertexShader* savedVs = nullptr;
    ID3D11PixelShader*  savedPs = nullptr;
    ID3D11InputLayout*  savedLayout = nullptr;
    ID3D11Buffer*       savedVertexBuffer = nullptr;
    UINT savedStride = 0;
    UINT savedOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11DepthStencilState* savedDepth = nullptr;
    UINT savedStencilRef = 0;
    ID3D11RasterizerState* savedRaster = nullptr;
    ID3D11BlendState* savedBlend = nullptr;
    FLOAT savedBlendFactor[4] = {};
    UINT savedSampleMask = 0;
    ID3D11Buffer* savedCb[2] = {};
    UINT savedFirst[2] = {};
    UINT savedNum[2] = {};
    ID3D11RenderTargetView* savedRtv = nullptr;
    ID3D11DepthStencilView* savedDsv = nullptr;
    D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    context->VSGetShader (&savedVs, nullptr, nullptr);
    context->PSGetShader (&savedPs, nullptr, nullptr);
    context->IAGetInputLayout (&savedLayout);
    context->IAGetVertexBuffers (0, 1, &savedVertexBuffer, &savedStride, &savedOffset);
    context->IAGetPrimitiveTopology (&savedTopology);
    context->OMGetDepthStencilState (&savedDepth, &savedStencilRef);
    context->RSGetState (&savedRaster);
    context->OMGetBlendState (&savedBlend, savedBlendFactor, &savedSampleMask);
    context->RSGetViewports (&savedViewportCount, savedViewports);
    context1->VSGetConstantBuffers1 (1, 2, savedCb, savedFirst, savedNum);
    context->OMGetRenderTargets (1, &savedRtv, &savedDsv);

    // ---- one output path for all three -------------------------------------
    context->OMSetRenderTargets (1, &targetView, nullptr);
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = viewportX;
    viewport.TopLeftY = viewportY;
    viewport.Width = viewportWidth;
    viewport.Height = viewportHeight;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    if (viewport.Width > 1.0f && viewport.Height > 1.0f)
        context->RSSetViewports (1, &viewport);
    context->OMSetDepthStencilState (g_depthState, 0);
    context->RSSetState (g_raster);
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_blend, blendFactor, 0xffffffffu);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    PollQueries (context);

    // ---- A: raster and output, no camera, no input assembly ----------------
    // ⚠️ NO VERTEX BUFFER AND NO INPUT LAYOUT, AND BOTH ARE EXPLICITLY CLEARED.
    // A stale layout left bound with a null vertex buffer is one of the ways a
    // `SV_VertexID` draw silently produces nothing.
    ID3D11Buffer* const noBuffer = nullptr;
    const UINT zero = 0;
    context->IASetInputLayout (nullptr);
    context->IASetVertexBuffers (0, 1, &noBuffer, &zero, &zero);
    context->VSSetShader (g_vsProbeA, nullptr, 0);
    context->PSSetShader (g_psProbeA, nullptr, 0);
    BeginQuery (context, size_t (Probe::RasterOutput));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::RasterOutput));

    // ---- B: the production camera, still no input assembly -----------------
    ID3D11Buffer* const cameraBuffers[2] = { view, projection };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);
    // The declaration the census selected, exactly as the production draw binds.
    const uint32_t wanted = ExpectedInterpretation ();
    if (wanted < 4 && g_vsProbeBVariant[wanted] != nullptr) {
        g_vsProbeB = g_vsProbeBVariant[wanted];
        g_stats.probeVsHash = g_probeHash[wanted];
    }
    context->VSSetShader (g_vsProbeB, nullptr, 0);
    context->PSSetShader (g_psProbeB, nullptr, 0);
    BeginQuery (context, size_t (Probe::CameraShader));
    context->Draw (3, 0);
    EndQuery (context, size_t (Probe::CameraShader));

    // ---- put everything back -----------------------------------------------
    context->OMSetRenderTargets (1, &savedRtv, savedDsv);
    context1->VSSetConstantBuffers1 (1, 2, savedCb, savedFirst, savedNum);
    context->VSSetShader (savedVs, nullptr, 0);
    context->PSSetShader (savedPs, nullptr, 0);
    context->IASetInputLayout (savedLayout);
    context->IASetVertexBuffers (0, 1, &savedVertexBuffer, &savedStride, &savedOffset);
    context->IASetPrimitiveTopology (savedTopology);
    context->OMSetDepthStencilState (savedDepth, savedStencilRef);
    context->RSSetState (savedRaster);
    context->OMSetBlendState (savedBlend, savedBlendFactor, savedSampleMask);
    if (savedViewportCount > 0)
        context->RSSetViewports (savedViewportCount, savedViewports);

    for (int i = 0; i < 2; ++i)
        ReleaseAndNull (savedCb[i]);
    ReleaseAndNull (savedVs);
    ReleaseAndNull (savedPs);
    ReleaseAndNull (savedLayout);
    ReleaseAndNull (savedVertexBuffer);
    ReleaseAndNull (savedDepth);
    ReleaseAndNull (savedRaster);
    ReleaseAndNull (savedBlend);
    ReleaseAndNull (savedRtv);
    ReleaseAndNull (savedDsv);
}

// RENDER THREAD. Probe C is the production draw itself, so the renderer wraps
// its own draw rather than this file repeating it -- a second copy of the
// production path would not be the production path.
void BeginProductionQuery (ID3D11DeviceContext* context)
{
    if (context == nullptr || !g_created)
        return;
    BeginQuery (context, size_t (Probe::Production));
}

void EndProductionQuery (ID3D11DeviceContext* context)
{
    if (context == nullptr || !g_created)
        return;
    EndQuery (context, size_t (Probe::Production));
}

void SetCameraVsHash (uint64_t hash)
{
    g_stats.cameraVsHash = hash;
}

void Reset ()
{
    for (size_t p = 0; p < kProbeCount; ++p)
        g_stats.probe[p] = ProbeStats {};
}

void Shutdown ()
{
    for (size_t p = 0; p < kProbeCount; ++p) {
        for (size_t i = 0; i < kQueryRing; ++i)
            ReleaseAndNull (g_queries[p].query[i]);
        g_queries[p] = QuerySet {};
    }
    ReleaseAndNull (g_blend);
    ReleaseAndNull (g_raster);
    ReleaseAndNull (g_depthTestState);
    ReleaseAndNull (g_depthState);
    ReleaseAndNull (g_psProbeC);
    ReleaseAndNull (g_vsBehind);
    ReleaseAndNull (g_vsFront);
    ReleaseAndNull (g_psProbeB);
    ReleaseAndNull (g_psProbeA);
    for (uint32_t variant = 0; variant < 4; ++variant)
        ReleaseAndNull (g_vsProbeBVariant[variant]);
    g_vsProbeB = nullptr;
    ReleaseAndNull (g_vsProbeA);
    ReleaseAndNull (g_vsBehind);
    ReleaseAndNull (g_vsFront);
    ReleaseAndNull (g_sceneDepthView);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_stats = Stats {};
}

Stats GetStats ()
{
    return g_stats;
}

}   // namespace probes
}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
