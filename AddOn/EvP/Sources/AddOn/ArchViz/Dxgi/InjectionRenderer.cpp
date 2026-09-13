// ArchViz/Dxgi/InjectionRenderer -- see the header. Every rule about this file
// is in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/InjectionRenderer.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

namespace {

// ⚠️ THE MATRICES ARE DECLARED WHERE ARCHICAD PUTS THEM AND READ WITH AN
// EXPLICIT LAYOUT. `row_major` is stated rather than left to the compiler's
// default, which is column-major, and the multiplication is row-vector -- world
// on the left -- because that is the convention stage 3's classifier proved
// these bytes are in. Getting either wrong produces a matrix that is almost
// right, which is the hardest kind of wrong to see.
//
// ⚠️ NO PADDING IS DECLARED BECAUSE THE MATRIX SITS AT OFFSET 0 OF ITS WINDOW.
// Run twenty-one measured `numConstants = 16` for both -- a 256-byte window,
// which is the minimum D3D11.1 granularity -- with the matrix at its start. If a
// future Archicad puts it elsewhere in the window the fix is padding HERE, never
// a CPU copy to make the offset convenient.
const char* const kShaderSource =
"cbuffer ArchicadView : register (b1)       { row_major float4x4 View; };\n"
"cbuffer ArchicadProjection : register (b2) { row_major float4x4 Projection; };\n"
"float4 VSMain (float3 position : POSITION) : SV_POSITION\n"
"{\n"
"    float4 p = float4 (position, 1.0);\n"
"    p = mul (p, View);\n"
"    p = mul (p, Projection);\n"
"    return p;\n"
"}\n"
"float4 PSMain () : SV_TARGET\n"
"{\n"
"    return float4 (1.0, 0.15, 0.85, 1.0);\n"
"}\n"
// ⚠️ PROBE A: NO CAMERA, NO CONSTANT BUFFERS, NO WORLD TRANSFORM. Its vertices
// are already in clip space, so it lands in the same corner of the screen on
// every Present no matter what any matrix says. That is the entire point: it
// separates "the injection or the back buffer is wrong" from "the camera is
// wrong", and those are different investigations. ⚠️ DO NOT DEBUG THE CAMERA
// UNTIL PROBE A IS ROCK SOLID.
"float4 VSScreen (float3 position : POSITION) : SV_POSITION\n"
"{\n"
"    return float4 (position, 1.0);\n"
"}\n"
"float4 PSScreen () : SV_TARGET\n"
"{\n"
"    return float4 (0.1, 1.0, 0.3, 1.0);\n"
"}\n";

// ⚠️ HARD-CODED WORLD METRES, DELIBERATELY LARGE AND DELIBERATELY ASYMMETRIC.
// The test is whether it stays welded to the model through a fast orbit, so it
// has to be big enough to see and lopsided enough that a mirrored or transposed
// matrix is obvious rather than merely plausible.
struct Vertex {
    float x, y, z;
};
// ⚠️ SMALL AND ANCHORED, NOT BIG AND FLOATING. A large primitive can drift
// several pixels and still look attached; a small one with a vertex on a known
// Archicad point makes a one-pixel error obvious. The anchor is settable so the
// vertex can be put exactly on a corner the user can see.
float g_anchorX = 0.0f;
float g_anchorY = 0.0f;
float g_anchorZ = 0.0f;
float g_anchorSize = 1.0f;

// ⚠️ THE FIRST FRAME THAT WORKS MUST BE IMPOSSIBLE TO MISS, and only then does
// the primitive shrink. Culling is off, depth is off and the pixel shader writes
// a flat opaque colour, so the only ways it can be invisible are: it went into
// the wrong pass, something drew over it afterwards, it was clipped, the
// transform is wrong, or it is genuinely outside the model. Those are five
// different investigations and a subtle primitive cannot tell them apart.

// ⚠️ THE WINDOW SIZE THE CAMERA BINDINGS MUST HAVE. Run twenty-one measured 16
// constants -- 256 bytes, the D3D11.1 minimum granularity -- for both b1 and b2.
// Anything else is a different layout and the predicate refuses rather than
// reading a matrix out of the wrong place.
constexpr UINT kExpectedWindowConstants = 16;

std::atomic<bool> g_enabled {false};

ID3D11Device*            g_device = nullptr;
ID3D11VertexShader*      g_vs = nullptr;
ID3D11PixelShader*       g_ps = nullptr;
ID3D11InputLayout*       g_layout = nullptr;
ID3D11Buffer*            g_vertices = nullptr;
ID3D11VertexShader*      g_screenVs = nullptr;
ID3D11PixelShader*       g_screenPs = nullptr;
ID3D11Buffer*            g_screenVertices = nullptr;

// ⚠️ OUR OWN COPIES OF THE CAMERA, 256 BYTES EACH -- one D3D11.1 window. Present
// binds THESE and never Archicad's ring, so nothing Archicad does to the ring
// after the model draw can change what the injected shader reads.
ID3D11Buffer*            g_viewSnapshot = nullptr;
ID3D11Buffer*            g_projectionSnapshot = nullptr;
uint64_t g_snapshotModelGeneration = 0;
uint64_t g_snapshotDrawSequence = 0;
bool     g_snapshotValid = false;
std::atomic<uint64_t> g_snapshotsTaken {0};

// ⚠️ CLIP SPACE, AND PINNED TOP RIGHT BECAUSE THE HUD OWNS THE TOP LEFT. A probe
// drawn underneath ImGui would be reported as flickering when it was only
// covered, which is the opposite of what this probe is for.
const Vertex kScreenProbe[3] = {
    {  0.98f,  0.98f, 0.0f },
    {  0.80f,  0.98f, 0.0f },
    {  0.98f,  0.80f, 0.0f },
};
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState*   g_raster = nullptr;
ID3D11BlendState*        g_blend = nullptr;
bool g_initialised = false;
bool g_initFailed = false;

std::atomic<uint64_t> g_injected {0};
std::atomic<uint64_t> g_skipNoDraw {0};
std::atomic<uint64_t> g_skipNoCamera {0};
std::atomic<uint64_t> g_skipNotReady {0};
std::atomic<uint64_t> g_skipPassMismatch {0};
std::atomic<uint64_t> g_skipWindowSize {0};
std::atomic<uint64_t> g_skipReentrant {0};
std::atomic<int> g_point {int (Point::Present)};
std::atomic<uint64_t> g_skipStaleCamera {0};
std::atomic<uint64_t> g_backBufferFailures {0};

// ⚠️ THE LAST ACCEPTED MODEL CAMERA, WHICH OUTLIVES PRESENTS ON PURPOSE. Present
// injection is ephemeral -- it paints the CURRENT back buffer and nothing of it
// survives into the next one -- so the triangle has to be redrawn on every
// Present whether or not Archicad re-rendered the model. Keeping the camera
// until a NEW model scene supersedes it is what makes that possible; requiring a
// camera from this very Present is what made it flicker.
contextstate::SceneDrawState g_acceptedCamera;
uint64_t g_lastInjectedModelGeneration = 0;

std::atomic<uint64_t> g_newScene {0};
std::atomic<uint64_t> g_repeatScene {0};
std::atomic<uint64_t> g_invalidScene {0};
char g_lastError[192] = {};

void Fail (const char* what)
{
    g_initFailed = true;
    strncpy_s (g_lastError, sizeof (g_lastError), what, _TRUNCATE);
}

template <typename T>
void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

bool EnsureCreated (ID3D11DeviceContext* context)
{
    if (g_initialised)
        return true;
    if (g_initFailed)
        return false;

    // ⚠️ ARCHICAD'S OWN DEVICE, TAKEN FROM ITS OWN CONTEXT. A second device would
    // put the triangle on a different GPU timeline and none of this would work.
    // ⚠️ THE VERTEX BUFFER IS BUILT FROM THE ANCHOR AT CREATION, so changing the
    // anchor means recreating it -- which `SetAnchor` does by shutting down.
    const Vertex triangle[3] = {
        { g_anchorX, g_anchorY, g_anchorZ },
        { g_anchorX + g_anchorSize, g_anchorY, g_anchorZ },
        { g_anchorX, g_anchorY, g_anchorZ + g_anchorSize },
    };

    context->GetDevice (&g_device);
    if (g_device == nullptr) {
        Fail ("the context has no device");
        return false;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const SIZE_T sourceLength = strlen (kShaderSource);

    if (FAILED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, &vsBlob, &errors))) {
        Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                : "the injection vertex shader would not compile");
        ReleaseAndNull (errors);
        return false;
    }
    ReleaseAndNull (errors);
    if (FAILED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr, nullptr,
                            "PSMain", "ps_5_0", flags, 0, &psBlob, &errors))) {
        Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                : "the injection pixel shader would not compile");
        ReleaseAndNull (errors);
        ReleaseAndNull (vsBlob);
        return false;
    }
    ReleaseAndNull (errors);

    bool ok = SUCCEEDED (g_device->CreateVertexShader (vsBlob->GetBufferPointer (),
            vsBlob->GetBufferSize (), nullptr, &g_vs));
    ok = ok && SUCCEEDED (g_device->CreatePixelShader (psBlob->GetBufferPointer (),
            psBlob->GetBufferSize (), nullptr, &g_ps));

    ID3DBlob* screenVsBlob = nullptr;
    ID3DBlob* screenPsBlob = nullptr;
    if (SUCCEEDED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr,
                               nullptr, "VSScreen", "vs_5_0", flags, 0, &screenVsBlob,
                               &errors)) &&
        SUCCEEDED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr,
                               nullptr, "PSScreen", "ps_5_0", flags, 0, &screenPsBlob,
                               &errors))) {
        ok = ok && SUCCEEDED (g_device->CreateVertexShader (screenVsBlob->GetBufferPointer (),
                screenVsBlob->GetBufferSize (), nullptr, &g_screenVs));
        ok = ok && SUCCEEDED (g_device->CreatePixelShader (screenPsBlob->GetBufferPointer (),
                screenPsBlob->GetBufferSize (), nullptr, &g_screenPs));
    } else {
        ok = false;
    }
    ReleaseAndNull (errors);
    ReleaseAndNull (screenVsBlob);
    ReleaseAndNull (screenPsBlob);

    const D3D11_INPUT_ELEMENT_DESC element = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
    };
    ok = ok && SUCCEEDED (g_device->CreateInputLayout (&element, 1, vsBlob->GetBufferPointer (),
            vsBlob->GetBufferSize (), &g_layout));
    ReleaseAndNull (vsBlob);
    ReleaseAndNull (psBlob);

    D3D11_BUFFER_DESC vertexDesc = {};
    vertexDesc.ByteWidth = sizeof (triangle);
    vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = triangle;
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&vertexDesc, &initial, &g_vertices));

    D3D11_BUFFER_DESC screenDesc = vertexDesc;
    screenDesc.ByteWidth = sizeof (kScreenProbe);
    D3D11_SUBRESOURCE_DATA screenInitial = {};
    screenInitial.pSysMem = kScreenProbe;
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&screenDesc, &screenInitial,
            &g_screenVertices));

    // ⚠️ `DEFAULT` USAGE, NOT `IMMUTABLE` AND NOT `DYNAMIC`. The GPU writes these
    // through `CopySubresourceRegion` and the vertex stage reads them: an
    // immutable buffer could not be copied into, and a dynamic one would invite
    // exactly the CPU round trip this design exists to avoid. 256 bytes is one
    // D3D11.1 window -- the minimum granularity, which is what Archicad uses.
    D3D11_BUFFER_DESC snapshotDesc = {};
    snapshotDesc.ByteWidth = 256;
    snapshotDesc.Usage = D3D11_USAGE_DEFAULT;
    snapshotDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&snapshotDesc, nullptr, &g_viewSnapshot));
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&snapshotDesc, nullptr,
            &g_projectionSnapshot));

    // ⚠️ PROOF A IS TRANSFORM ONLY: DEPTH TEST OFF. Depth WRITES stay off in
    // proof B as well, so this experiment can never affect Archicad geometry
    // drawn after it. Debugging the camera and the depth semantics at the same
    // time is how neither of them gets debugged.
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depthDesc, &g_depthState));

    // No culling: a triangle that vanished from one side would read as a
    // synchronisation failure when it was only a winding order.
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&rasterDesc, &g_raster));

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED (g_device->CreateBlendState (&blendDesc, &g_blend));

    if (!ok) {
        Fail ("an injection device object could not be created");
        return false;
    }
    g_initialised = true;
    return true;
}

}   // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void SetAnchor (float x, float y, float z, float sizeMetres)
{
    g_anchorX = x;
    g_anchorY = y;
    g_anchorZ = z;
    g_anchorSize = (sizeMetres > 0.001f) ? sizeMetres : 1.0f;
    // The vertex buffer is immutable and built from these, so it has to go.
    // Cheap: it is rebuilt on the next injection.
    ReleaseAndNull (g_vertices);
    g_initialised = false;
    g_initFailed = false;
}

void Shutdown ()
{
    g_enabled.store (false, std::memory_order_release);
    ReleaseAndNull (g_blend);
    ReleaseAndNull (g_raster);
    ReleaseAndNull (g_depthState);
    ReleaseAndNull (g_projectionSnapshot);
    ReleaseAndNull (g_viewSnapshot);
    g_snapshotValid = false;
    g_snapshotModelGeneration = 0;
    ReleaseAndNull (g_screenVertices);
    ReleaseAndNull (g_screenPs);
    ReleaseAndNull (g_screenVs);
    ReleaseAndNull (g_vertices);
    ReleaseAndNull (g_layout);
    ReleaseAndNull (g_ps);
    ReleaseAndNull (g_vs);
    ReleaseAndNull (g_device);
    g_initialised = false;
    g_initFailed = false;
}

// The draw itself, with every piece of state it changes saved and restored.
// Both injection points go through this so they cannot disagree about what they
// touch. `targetView` is null when the caller has already got the right render
// target bound and only wants the draw.
void DrawWithCamera (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
                     const contextstate::SceneDrawState& draw,
                     ID3D11RenderTargetView* targetView, bool useSnapshot)
{
    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];

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
    if (targetView != nullptr)
        context->OMGetRenderTargets (1, &savedRtv, &savedDsv);

    // ⚠️ THE CAMERA WINDOWS ARE BOUND EXACTLY AS THE LATCHED DRAW HAD THEM --
    // buffer, firstConstant AND numConstants. The legacy setter would rebind the
    // same buffer at offset zero and hand the shader a different region of an
    // 8 MiB ring.
    // ⚠️ OUR SNAPSHOTS, NOT ARCHICAD'S RING. The whole point of the copy is that
    // by the time this runs, the ring window the model draw used may hold some
    // later pass's constants. Our buffers hold exactly the bytes that draw
    // consumed, at offset zero, which is why `firstConstant` is 0 here.
    ID3D11Buffer* const snapshotBuffers[2] = { g_viewSnapshot, g_projectionSnapshot };
    ID3D11Buffer* const liveBuffers[2] = {
        reinterpret_cast<ID3D11Buffer*> (uintptr_t (view.buffer)),
        reinterpret_cast<ID3D11Buffer*> (uintptr_t (projection.buffer)),
    };
    const UINT snapshotFirst[2] = { 0, 0 };
    const UINT snapshotNum[2] = { kExpectedWindowConstants, kExpectedWindowConstants };
    const UINT liveFirst[2] = { view.firstConstant, projection.firstConstant };
    const UINT liveNum[2] = { view.numConstants, projection.numConstants };
    if (useSnapshot)
        context1->VSSetConstantBuffers1 (1, 2, snapshotBuffers, snapshotFirst, snapshotNum);
    else
        context1->VSSetConstantBuffers1 (1, 2, liveBuffers, liveFirst, liveNum);

    if (targetView != nullptr) {
        // Proof A at Present: no depth view at all, which is what depth-off means
        // here and also stops us writing into a buffer we do not own.
        context->OMSetRenderTargets (1, &targetView, nullptr);
    }

    D3D11_VIEWPORT sceneViewport = {};
    sceneViewport.TopLeftX = draw.viewportX;
    sceneViewport.TopLeftY = draw.viewportY;
    sceneViewport.Width = draw.viewportWidth;
    sceneViewport.Height = draw.viewportHeight;
    sceneViewport.MinDepth = 0.0f;
    sceneViewport.MaxDepth = 1.0f;
    if (sceneViewport.Width > 1.0f && sceneViewport.Height > 1.0f)
        context->RSSetViewports (1, &sceneViewport);

    const UINT stride = sizeof (Vertex);
    const UINT offset = 0;

    // ⚠️ PROBE A FIRST, AND IT USES NO CAMERA AT ALL. If the green corner wedge
    // is not rock solid on every Present then the fault is the injection, the
    // back buffer or the state restoration, and looking at the camera would be
    // looking in the wrong place. Only once it is stable does the magenta
    // world-space triangle beside it mean anything.
    context->IASetInputLayout (g_layout);
    context->IASetVertexBuffers (0, 1, &g_screenVertices, &stride, &offset);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (g_screenVs, nullptr, 0);
    context->PSSetShader (g_screenPs, nullptr, 0);
    context->OMSetDepthStencilState (g_depthState, 0);
    context->RSSetState (g_raster);
    const FLOAT probeBlend[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_blend, probeBlend, 0xffffffffu);
    context->Draw (3, 0);

    context->IASetInputLayout (g_layout);
    context->IASetVertexBuffers (0, 1, &g_vertices, &stride, &offset);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (g_vs, nullptr, 0);
    context->PSSetShader (g_ps, nullptr, 0);
    context->OMSetDepthStencilState (g_depthState, 0);
    context->RSSetState (g_raster);
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_blend, blendFactor, 0xffffffffu);
    context->Draw (3, 0);

    // ---- put everything back ------------------------------------------------
    if (targetView != nullptr)
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

    // ⚠️ EVERY `*Get*` RETURNED AN ADDREF'D POINTER. One leaked per frame would
    // keep Archicad's own shaders, buffers and views alive past a resize.
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

void SnapshotCamera (ID3D11DeviceContext* context)
{
    if (context == nullptr || !g_enabled.load (std::memory_order_acquire))
        return;
    // ⚠️ OUR OWN COPIES ARE NOT ARCHICAD'S WORK. Without the guard these two
    // copies would be counted as Archicad copy operations and could trip the
    // scene-consumer logic that once served as the injection trigger.
    if (contextstate::Injecting ())
        return;

    const contextstate::SceneDrawState draw = contextstate::LastCameraDraw ();
    if (!draw.valid)
        return;
    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];
    if (view.numConstants != kExpectedWindowConstants ||
        projection.numConstants != kExpectedWindowConstants)
        return;

    contextstate::ScopedInjectionGuard guard;
    if (!EnsureCreated (context))
        return;

    // ⚠️ BYTE COORDINATES, BECAUSE THESE ARE BUFFERS AND NOT TEXTURES. `left` and
    // `right` are byte offsets into the ring; `top`/`bottom`/`front`/`back` are
    // the 0..1 a buffer always has. Copying 256 bytes from `firstConstant * 16`
    // lands the window at offset 0 of our own buffer, which is why the shader
    // needs no padding and why Present can bind it with firstConstant 0.
    D3D11_BOX box = {};
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;

    box.left = view.ByteOffset ();
    box.right = box.left + 256;
    context->CopySubresourceRegion (g_viewSnapshot, 0, 0, 0, 0,
            reinterpret_cast<ID3D11Buffer*> (uintptr_t (view.buffer)), 0, &box);

    box.left = projection.ByteOffset ();
    box.right = box.left + 256;
    context->CopySubresourceRegion (g_projectionSnapshot, 0, 0, 0, 0,
            reinterpret_cast<ID3D11Buffer*> (uintptr_t (projection.buffer)), 0, &box);

    // ⚠️ EVERY QUALIFYING DRAW, NOT A GUESS AT THE LAST ONE. Two 256-byte GPU
    // copies are nothing beside a draw, and overwriting on each one means that by
    // Present the snapshot simply holds the most recent camera of the pass --
    // with no prediction about which draw would turn out to be final.
    g_snapshotModelGeneration = draw.modelSceneGeneration;
    g_snapshotDrawSequence = draw.drawSequence;
    g_snapshotValid = true;
    g_snapshotsTaken.fetch_add (1, std::memory_order_relaxed);
}

void SetPoint (Point point)
{
    g_point.store (int (point), std::memory_order_release);
}

Point GetPoint ()
{
    return Point (g_point.load (std::memory_order_acquire));
}

void InjectAtPresent (ID3D11DeviceContext* context, IDXGISwapChain* swapChain,
                      uint64_t modelSceneGeneration)
{
    if (context == nullptr || swapChain == nullptr)
        return;
    if (!g_enabled.load (std::memory_order_acquire))
        return;
    if (GetPoint () != Point::Present)
        return;
    if (contextstate::Injecting ()) {
        g_skipReentrant.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ---- which of the three frame states is this Present? ------------------
    const contextstate::SceneDrawState fresh = contextstate::LastCameraDraw ();
    const bool freshIsUsable =
            fresh.valid &&
            fresh.vsConstantBuffers[1].numConstants == kExpectedWindowConstants &&
            fresh.vsConstantBuffers[2].numConstants == kExpectedWindowConstants;

    // ⚠️ NO SNAPSHOT, NO WORLD TRIANGLE. The clip-space probe still draws, so a
    // run with a broken snapshot is still visibly distinguishable from a run with
    // a broken injection.
    if (!g_snapshotValid) {
        g_invalidScene.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    if (freshIsUsable && fresh.modelSceneGeneration != g_lastInjectedModelGeneration) {
        // NEW_SCENE: the model was re-rendered since we last drew, and it came
        // with its own camera. Take it.
        g_acceptedCamera = fresh;
        g_lastInjectedModelGeneration = fresh.modelSceneGeneration;
        g_newScene.fetch_add (1, std::memory_order_relaxed);
    } else if (g_acceptedCamera.valid &&
               modelSceneGeneration == g_lastInjectedModelGeneration) {
        // ⚠️ REPEAT_SCENE, AND IT IS VALID RATHER THAN A COMPROMISE. Archicad
        // presented without re-rendering the model -- a UI repaint, a palette, a
        // cursor -- so the geometry on screen is the geometry this camera drew.
        // Present injection paints the CURRENT back buffer and nothing survives
        // into the next one, so it has to be drawn again every time.
        g_repeatScene.fetch_add (1, std::memory_order_relaxed);
    } else {
        // ⚠️ INVALID_SCENE: the model moved on and no camera came with it. New
        // geometry with an old camera is the one combination that is forbidden,
        // and the only one worth skipping a frame for.
        g_invalidScene.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    contextstate::ScopedInjectionGuard guard;
    if (!EnsureCreated (context)) {
        g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) ||
        context1 == nullptr) {
        g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ⚠️ THE BACK BUFFER IS FETCHED AND RELEASED HERE, NEVER CACHED. A held view
    // of a swap-chain buffer makes `ResizeBuffers` fail, which would break every
    // window resize in Archicad.
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* targetView = nullptr;
    if (SUCCEEDED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) &&
        backBuffer != nullptr) {
        g_device->CreateRenderTargetView (backBuffer, nullptr, &targetView);
    }
    if (targetView != nullptr) {
        DrawWithCamera (context, context1, g_acceptedCamera, targetView, true);
        g_injected.fetch_add (1, std::memory_order_relaxed);
    } else {
        g_backBufferFailures.fetch_add (1, std::memory_order_relaxed);
    }
    ReleaseAndNull (targetView);
    ReleaseAndNull (backBuffer);
    ReleaseAndNull (context1);
}

void InjectIfReady (ID3D11DeviceContext* context)
{
    if (GetPoint () != Point::ScenePass)
        return;
    if (context == nullptr || !g_enabled.load (std::memory_order_acquire))
        return;

    // ⚠️ ALREADY INSIDE AN INJECTION. Our own draw calls reach the same detours,
    // so a nested entry would be us, and re-entering would recurse.
    if (contextstate::Injecting ()) {
        g_skipReentrant.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    const contextstate::SceneDrawState draw = contextstate::LastSceneDraw ();
    if (!draw.valid || draw.drawSequence == 0) {
        g_skipNoDraw.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ⚠️ THE LATCHED DRAW MUST BELONG TO THE PASS THAT IS ENDING. Injecting with
    // a camera latched during a DIFFERENT pass -- a shadow map, a thumbnail, the
    // pass before this one -- is the exact splice stage 4 exists to refuse, and
    // it would look like a plausible near-miss rather than an error.
    const renderstate::ScenePass pass = renderstate::CurrentScenePass ();
    if (draw.scenePassGeneration != pass.generation ||
        draw.sceneTargetEpoch != pass.targetEpoch) {
        g_skipPassMismatch.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ⚠️ BOTH HALVES, FROM THAT SAME DRAW, OR NOTHING. This is where stage 4's
    // guarantee is spent: the bindings come from the state latched at the last
    // verified scene draw, not from whatever is live now -- Archicad may legally
    // change shader and constant buffers between that draw and this call.
    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];
    if (!view.IsBound () || !projection.IsBound ()) {
        g_skipNoCamera.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    if (view.numConstants != kExpectedWindowConstants ||
        projection.numConstants != kExpectedWindowConstants) {
        // A different window size means a different layout, and the shader
        // reads the matrix at offset zero of a 256-byte window. Refuse.
        g_skipWindowSize.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ⚠️ EVERYTHING BELOW IS OURS AND MUST NOT BE RECORDED AS ARCHICAD'S. The
    // guard is taken before the first D3D call and held past the last; without
    // it these calls arrive at the same detours, on the same context object, and
    // no pointer filter could separate them.
    // ⚠️ THE GUARD IS RAISED BEFORE THE FIRST D3D CALL AND HELD PAST THE LAST,
    // and the injection is marked as done for this pass BEFORE any of our own
    // calls go out -- not after. A call of ours that re-entered this function
    // would otherwise find the pass still unmarked and inject again.
    contextstate::ScopedInjectionGuard guard;

    if (!EnsureCreated (context)) {
        g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ⚠️ THE D3D11.1 INTERFACE IS REQUIRED, NOT PREFERRED. Archicad binds
    // sub-ranges of an 8 MiB ring; saving with `VSGetConstantBuffers` and
    // restoring with `VSSetConstantBuffers` would rebind the same buffer at
    // offset zero and hand Archicad a different region of its own ring.
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) ||
        context1 == nullptr) {
        g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // In-pass injection happens while the ring window is still current, so it
    // does not need the snapshot -- but it uses it anyway for one reason: two
    // code paths that read the camera differently would eventually disagree.
    DrawWithCamera (context, context1, draw, nullptr, true);
    ReleaseAndNull (context1);

    g_injected.fetch_add (1, std::memory_order_relaxed);
}

InjectionStats GetInjectionStats ()
{
    InjectionStats stats;
    stats.injected = g_injected.load (std::memory_order_relaxed);
    stats.skippedNoSceneDraw = g_skipNoDraw.load (std::memory_order_relaxed);
    stats.skippedNoCamera = g_skipNoCamera.load (std::memory_order_relaxed);
    stats.skippedNotReady = g_skipNotReady.load (std::memory_order_relaxed);
    stats.skippedPassMismatch = g_skipPassMismatch.load (std::memory_order_relaxed);
    stats.skippedWindowSize = g_skipWindowSize.load (std::memory_order_relaxed);
    stats.skippedReentrant = g_skipReentrant.load (std::memory_order_relaxed);
    stats.skippedStaleCamera = g_skipStaleCamera.load (std::memory_order_relaxed);
    stats.backBufferFailures = g_backBufferFailures.load (std::memory_order_relaxed);
    stats.newScene = g_newScene.load (std::memory_order_relaxed);
    stats.repeatScene = g_repeatScene.load (std::memory_order_relaxed);
    stats.invalidScene = g_invalidScene.load (std::memory_order_relaxed);
    stats.snapshotsTaken = g_snapshotsTaken.load (std::memory_order_relaxed);
    stats.snapshotValid = g_snapshotValid;
    stats.initialised = g_initialised;
    strncpy_s (stats.lastError, sizeof (stats.lastError), g_lastError, _TRUNCATE);
    return stats;
}

}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
