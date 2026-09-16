// ArchViz/Dxgi/InjectionRenderer -- see the header. Every rule about this file
// is in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/InjectionRenderer.hpp"

#include "ArchViz/Dxgi/PipelineStateGuard.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/DepthCheckpoints.hpp"
#include "ArchViz/Dxgi/GhostMesh.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"
#include "ArchViz/Dxgi/InjectionOracle.hpp"
#include "ArchViz/Dxgi/InjectionProbes.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

namespace {

// ⚠️ THE TWO HALVES ARE DECLARED WITH DIFFERENT LAYOUTS, AND THAT IS THE
// MEASURED RESULT RATHER THAN A GUESS. The camera census scores eight readings of
// the same 128 bytes against the orbit-target invariant on every moving frame,
// and the selected group returned interpretation 2 -- `p * V * Pt` -- in phase A
// and again in phase B, at 99% inside the clip volume and a median of 0.002 from
// the viewport centre.
//
//     View        row_major      the bytes are the view, as stored
//     Projection  column_major   the bytes are the TRANSPOSE of the projection
//
// ⚠️ `column_major` IS THE FIX, NOT A `transpose ()` CALL AND CERTAINLY NOT A CPU
// COPY. A cbuffer matrix declared `column_major` has its columns in successive
// registers, so HLSL reads exactly the transpose of the same bytes read
// `row_major` -- which is what `p * V * Pt` means. The declaration records WHAT
// ARCHICAD'S BUFFER ACTUALLY CONTAINS; a transpose in the shader body would
// produce identical pixels while writing down the wrong fact, and the next person
// would have to rediscover it.
//
// The multiplication stays row-vector, world on the left, which is the convention
// the view half has been proven in since stage 3.
//
// ⚠️ NO PADDING IS DECLARED BECAUSE THE MATRIX SITS AT OFFSET 0 OF ITS WINDOW.
// Run twenty-one measured `numConstants = 16` for both -- a 256-byte window,
// which is the minimum D3D11.1 granularity -- with the matrix at its start. If a
// future Archicad puts it elsewhere in the window the fix is padding HERE, never
// a CPU copy to make the offset convenient.
// ⚠️ THE TWO LAYOUTS ARE SUBSTITUTED, NOT HARD-CODED, AND THE CENSUS CHOOSES
// THEM. Run thirty-six is why: the shader was pinned to interpretation 2 by a
// constant in this file while the scorer that chose 2 could not tell a correct
// transform from one that collapses the primitive to a point. Compiling all four
// declarations and binding the one the census selected removes the possibility
// of the two disagreeing at all -- there is no longer a number here to be wrong.
//
//     variant bit 0 -- the VIEW is read transposed      (column_major)
//     variant bit 1 -- the PROJECTION is read transposed (column_major)
//
// which is exactly the oracle's variant numbering, so no translation is needed
// between what is measured and what is drawn.
// ⚠️ THE DECLARATIONS ARE IN `CameraShaderSource.hpp`, NOT HERE. The
// ghost mesh reads the same `b1` and `b2` through the same two lines, and a
// second copy of them in a second file is exactly how run thirty-three ended up
// measuring one reading while drawing another. This file owns the BODIES of the
// proof shaders and nothing about the camera contract.
const char* const kProofShaderBody = "float4 VSMain (float3 position : POSITION) : SV_POSITION\n"
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
                                     // ⚠️ PROBE A: NO CAMERA, NO CONSTANT BUFFERS, NO WORLD TRANSFORM. Its
                                     // vertices are already in clip space, so it lands in the same corner of the screen
                                     // on every Present no matter what any matrix says. That is the entire point: it
                                     // separates "the injection or the back buffer is wrong" from "the camera is
                                     // wrong", and those are different investigations. ⚠️ DO NOT DEBUG THE
                                     // CAMERA UNTIL PROBE A IS ROCK SOLID.
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

// ⚠️ THREE STATES, NOT A BOOL. See the header: the caller cannot
// be asked to prove it has a camera before we start looking for one.
std::atomic<uint32_t> g_armState { uint32_t (ArmState::Disabled) };

ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;      // the bound variant
ID3D11VertexShader* g_vsVariant[4] = {}; // one per matrix declaration
uint32_t g_boundVariant = 0;
ID3D11PixelShader* g_ps = nullptr;

ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_vertices = nullptr;
ID3D11VertexShader* g_screenVs = nullptr;
ID3D11PixelShader* g_screenPs = nullptr;
ID3D11Buffer* g_screenVertices = nullptr;

// ⚠️ THE CAMERA ITSELF LIVES IN `InjectionCamera`, NOT HERE. This file answers
// how the primitive is DRAWN; that one answers what it is drawn WITH. Five runs
// of failure were all in the second while the first was never in doubt.

// ⚠️ CLIP SPACE, AND PINNED TOP RIGHT BECAUSE THE HUD OWNS THE TOP LEFT. A probe
// drawn underneath ImGui would be reported as flickering when it was only
// covered, which is the opposite of what this probe is for.
const Vertex kScreenProbe[3] = {
    { 0.98f, 0.98f, 0.0f },
    { 0.80f, 0.98f, 0.0f },
    { 0.98f, 0.80f, 0.0f },
};
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11BlendState* g_blend = nullptr;
bool g_initialised = false;
bool g_initFailed = false;

std::atomic<uint64_t> g_injected { 0 };
std::atomic<uint64_t> g_skipNoDraw { 0 };
std::atomic<uint64_t> g_skipNoCamera { 0 };
std::atomic<uint64_t> g_skipNotReady { 0 };
std::atomic<uint64_t> g_skipPassMismatch { 0 };
std::atomic<uint64_t> g_skipWindowSize { 0 };
std::atomic<uint64_t> g_skipReentrant { 0 };
std::atomic<int> g_point { int (Point::Present) };
std::atomic<uint64_t> g_skipStaleCamera { 0 };
std::atomic<uint64_t> g_backBufferFailures { 0 };
std::atomic<uint64_t> g_skipInterpretation { 0 };

// ⚠️ THE TWO INJECTION POINTS ARE COUNTED SEPARATELY. They have entirely
// different denominators -- one fires per scene departure, the other per Present
// -- and sharing a counter produced "attempted 381, succeeded 944", which reads
// as a broken latch and is only a broken report.
std::atomic<uint64_t> g_injectedPresent { 0 };
std::atomic<uint64_t> g_injectedScenePass { 0 };

// ⚠️ THE LAST ACCEPTED MODEL CAMERA, WHICH OUTLIVES PRESENTS ON PURPOSE. Present
// injection is ephemeral -- it paints the CURRENT back buffer and nothing of it
// survives into the next one -- so the triangle has to be redrawn on every
// Present whether or not Archicad re-rendered the model. Keeping the camera
// until a NEW model scene supersedes it is what makes that possible; requiring a
// camera from this very Present is what made it flicker.
contextstate::SceneDrawState g_acceptedCamera;
uint64_t g_lastInjectedModelGeneration = 0;

std::atomic<uint64_t> g_newScene { 0 };
std::atomic<uint64_t> g_repeatScene { 0 };
std::atomic<uint64_t> g_invalidScene { 0 };
std::atomic<uint64_t> g_invalidNoSnapshot { 0 };
std::atomic<uint64_t> g_invalidNoDrawThisGeneration { 0 };
std::atomic<uint64_t> g_invalidGenerationAdvanced { 0 };
std::atomic<uint64_t> g_invalidGenerationMismatch { 0 };
char g_lastError[192] = {};

void Fail (const char* what)
{
    g_initFailed = true;
    strncpy_s (g_lastError, sizeof (g_lastError), what, _TRUNCATE);
}

template <typename T> void ReleaseAndNull (T*& object)
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

    // ⚠️ ALL FOUR DECLARATIONS ARE COMPILED AND THE CENSUS PICKS ONE. A constant
    // in this file saying "the shader implements interpretation N" was a claim
    // nobody could check; a shader chosen BY the measurement cannot disagree with
    // it. See `kShaderSourceFormat`.
    char sources[4][camerashader::kMaxSource] = {};
    for (uint32_t variant = 0; variant < 4; ++variant)
        camerashader::Compose (variant, kProofShaderBody, sources[variant], sizeof (sources[variant]));
    const char* const kShaderSource = sources[0];
    const SIZE_T sourceLength = strlen (kShaderSource);

    if (FAILED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr, nullptr, "VSMain", "vs_5_0",
                            flags, 0, &vsBlob, &errors))) {
        Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                : "the injection vertex shader would not compile");
        ReleaseAndNull (errors);
        return false;
    }
    ReleaseAndNull (errors);
    if (FAILED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr, nullptr, "PSMain", "ps_5_0",
                            flags, 0, &psBlob, &errors))) {
        Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                : "the injection pixel shader would not compile");
        ReleaseAndNull (errors);
        ReleaseAndNull (vsBlob);
        return false;
    }
    ReleaseAndNull (errors);

    bool ok = SUCCEEDED (
        g_device->CreateVertexShader (vsBlob->GetBufferPointer (), vsBlob->GetBufferSize (), nullptr, &g_vsVariant[0]));

    // The other three declarations of the same shader. The input signature is
    // identical, so the input layout below is built from variant 0's blob and is
    // valid for all of them.
    for (uint32_t variant = 1; variant < 4 && ok; ++variant) {
        ID3DBlob* blob = nullptr;
        if (FAILED (D3DCompile (sources[variant], strlen (sources[variant]), "TapiocaInjection", nullptr, nullptr,
                                "VSMain", "vs_5_0", flags, 0, &blob, &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                    : "a camera shader variant would not compile");
            ok = false;
        }
        else {
            ok = SUCCEEDED (g_device->CreateVertexShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr,
                                                          &g_vsVariant[variant]));
        }
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
    }
    g_boundVariant = 0;
    g_vs = g_vsVariant[0];

    // ⚠️ THE PRODUCTION VERTEX SHADER'S IDENTITY, SO THE REPORT CAN PRINT IT
    // BESIDE PROBE B'S. "They use the same camera semantics" is an assumption
    // until two hashes are shown side by side -- and if B passes while C fails,
    // the first question is whether they were the shaders we think they were.
    {
        const unsigned char* bytes = (const unsigned char*) vsBlob->GetBufferPointer ();
        const size_t size = vsBlob->GetBufferSize ();
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i) {
            hash ^= uint64_t (bytes[i]);
            hash *= 1099511628211ull;
        }
        probes::SetCameraVsHash (hash);
    }
    ok = ok && SUCCEEDED (
                   g_device->CreatePixelShader (psBlob->GetBufferPointer (), psBlob->GetBufferSize (), nullptr, &g_ps));

    ID3DBlob* screenVsBlob = nullptr;
    ID3DBlob* screenPsBlob = nullptr;
    if (SUCCEEDED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr, nullptr, "VSScreen", "vs_5_0",
                               flags, 0, &screenVsBlob, &errors)) &&
        SUCCEEDED (D3DCompile (kShaderSource, sourceLength, "TapiocaInjection", nullptr, nullptr, "PSScreen", "ps_5_0",
                               flags, 0, &screenPsBlob, &errors))) {
        ok = ok && SUCCEEDED (g_device->CreateVertexShader (screenVsBlob->GetBufferPointer (),
                                                            screenVsBlob->GetBufferSize (), nullptr, &g_screenVs));
        ok = ok && SUCCEEDED (g_device->CreatePixelShader (screenPsBlob->GetBufferPointer (),
                                                           screenPsBlob->GetBufferSize (), nullptr, &g_screenPs));
    }
    else {
        ok = false;
    }
    ReleaseAndNull (errors);
    ReleaseAndNull (screenVsBlob);
    ReleaseAndNull (screenPsBlob);

    const D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
                                               0,          0, D3D11_INPUT_PER_VERTEX_DATA,
                                               0 };
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
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&screenDesc, &screenInitial, &g_screenVertices));

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
    // ⚠️ SCISSOR OFF, STATED RATHER THAN INHERITED. Archicad draws its helpers
    // and UI with scissor rectangles, and a correct transform clipped by a
    // leftover helper rectangle produces exactly zero pixels and looks identical
    // to a wrong camera. Binding our own rasterizer state with `ScissorEnable`
    // false removes that possibility completely -- the rectangles themselves can
    // then stay as Archicad left them, because a disabled scissor ignores them.
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;
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

} // namespace

void SetEnabled (bool enabled)
{
    if (!enabled) {
        g_armState.store (uint32_t (ArmState::Disabled), std::memory_order_release);
        return;
    }
    // ⚠️ ARMING NEVER JUMPS STRAIGHT TO `Active`. It says "start
    // looking"; the render thread says whether anything was found. Re-arming
    // while already active is a no-op rather than a demotion, so a mode change
    // mid-run does not throw away a camera that is already working.
    uint32_t expected = uint32_t (ArmState::Disabled);
    g_armState.compare_exchange_strong (expected, uint32_t (ArmState::ArmedPendingCamera), std::memory_order_acq_rel);
}

bool Enabled ()
{
    return g_armState.load (std::memory_order_acquire) != uint32_t (ArmState::Disabled);
}

ArmState GetArmState ()
{
    return ArmState (g_armState.load (std::memory_order_acquire));
}

void NotifyCameraAcquired ()
{
    // ⚠️ ONLY EVER PROMOTES SOMETHING ALREADY ARMED. A stray call
    // while disabled must not start drawing into the user's 3D window.
    uint32_t expected = uint32_t (ArmState::ArmedPendingCamera);
    g_armState.compare_exchange_strong (expected, uint32_t (ArmState::Active), std::memory_order_acq_rel);
}

void SetAnchor (float x, float y, float z, float sizeMetres)
{
    g_anchorX = x;
    g_anchorY = y;
    g_anchorZ = z;
    g_anchorSize = (sizeMetres > 0.001f) ? sizeMetres : 1.0f;
    // The oracle projects THIS point and nothing else: during an orbit it is the
    // orbit target, which is what makes its pixel a test rather than a reading.
    oracle::SetAnchor (x, y, z, g_anchorSize);
    // ⚠️ ONE ANCHOR FOR BOTH, so the mesh and the proof triangle are
    // never testing different points. A ghost mesh built somewhere else would
    // make "the triangle is welded and the mesh is not" an ambiguous result.
    ghost::SetAnchor (x, y, z, g_anchorSize);
    probes::SetAnchor (x, y, z, g_anchorSize);
    // The vertex buffer is immutable and built from these, so it has to go.
    // Cheap: it is rebuilt on the next injection.
    ReleaseAndNull (g_vertices);
    g_initialised = false;
    g_initFailed = false;
}

void Shutdown ()
{
    g_armState.store (uint32_t (ArmState::Disabled), std::memory_order_release);
    oracle::Shutdown ();
    ReleaseAndNull (g_blend);
    ReleaseAndNull (g_raster);
    ReleaseAndNull (g_depthState);
    ShutdownCamera ();
    probes::Shutdown ();
    depth::Shutdown ();
    ReleaseAndNull (g_screenVertices);
    ReleaseAndNull (g_screenPs);
    ReleaseAndNull (g_screenVs);
    ReleaseAndNull (g_vertices);
    ReleaseAndNull (g_layout);
    ghost::Shutdown ();
    checkpoints::Shutdown ();
    ReleaseAndNull (g_ps);
    for (uint32_t variant = 0; variant < 4; ++variant)
        ReleaseAndNull (g_vsVariant[variant]);
    g_vs = nullptr;
    ReleaseAndNull (g_device);
    g_initialised = false;
    g_initFailed = false;
}

// The draw itself, with every piece of state it changes saved and restored.
// Both injection points go through this so they cannot disagree about what they
// touch. `targetView` is null when the caller has already got the right render
// target bound and only wants the draw.
// ⚠️ `altColour` LETS THE TWO INJECTION POINTS BE TOLD APART ON SCREEN WITHOUT A
// LINE OF NEW HLSL. The world vertex shader and the clip-space pixel shader are
// a legal pair -- the pixel shader takes no interpolated inputs -- so the
// scene-pass triangle can be green and the Present one magenta using shaders
// that are already compiled. With `point="both"` that turns four different
// failures into four different pictures in a single run.
void DrawWithCamera (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1,
                     const contextstate::SceneDrawState& draw, ID3D11RenderTargetView* targetView, bool useSnapshot,
                     bool altColour)
{
    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];

    // ⚠️ EVERYTHING THIS DRAW DISTURBS, PUT BACK BY A DESTRUCTOR.
    // See `PipelineStateGuard.hpp`. This was sixty hand-written lines here, and
    // the same sixty in `InjectionProbes` and `DepthCheckpoints` -- and when the
    // ghost mesh introduced the first `DrawIndexed` on this path, the index
    // binding was added to ONE of the three. The other two would have handed
    // Archicad back our index buffer.
    const ScopedPipelineState saved (context, context1);
    ID3D11RenderTargetView* const savedRtv = saved.SavedRenderTarget ();
    ID3D11DepthStencilView* const savedDsv = saved.SavedDepthStencil ();

    // ⚠️ THE CAMERA WINDOWS ARE BOUND EXACTLY AS THE LATCHED DRAW HAD THEM --
    // buffer, firstConstant AND numConstants. The legacy setter would rebind the
    // same buffer at offset zero and hand the shader a different region of an
    // 8 MiB ring.
    // ⚠️ OUR SNAPSHOTS, NOT ARCHICAD'S RING. The whole point of the copy is that
    // by the time this runs, the ring window the model draw used may hold some
    // later pass's constants. Our buffers hold exactly the bytes that draw
    // consumed, at offset zero, which is why `firstConstant` is 0 here.
    ID3D11Buffer* const snapshotBuffers[2] = { ViewSnapshotBuffer (), ProjectionSnapshotBuffer () };
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

    // ⚠️ THE PRODUCTION OVERLAY NOW TESTS AGAINST ARCHICAD'S DEPTH, which is
    // what proofs A and B were for. `Off` is the Proof A control and keeps the
    // old behaviour exactly; `SceneReadOnly` is Proof B2's finding, the building
    // occluding our geometry with nothing written back; `PrivateCopy` adds
    // depth WRITES into a texture we own, so ghost surfaces occlude each other
    // too -- and still not one write reaches Archicad's buffer.
    ID3D11DepthStencilView* depthView = nullptr;
    if (targetView != nullptr) {
        depthView = depth::PrepareForInjection (context);
        context->OMSetRenderTargets (1, &targetView, depthView);
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
    // ⚠️ THE VARIANT THE CENSUS SELECTED, RE-BOUND EVERY DRAW. `ExpectedInterpretation`
    // is what the measurement chose; anything at or above 4 is a reversed
    // multiplication order, which no declaration can express, and the injection
    // refuses those upstream rather than silently drawing variant 0.
    const uint32_t wanted = ExpectedInterpretation ();
    if (wanted < 4 && g_vsVariant[wanted] != nullptr) {
        g_boundVariant = wanted;
        g_vs = g_vsVariant[wanted];
        SetShaderInterpretation (wanted);
    }
    context->VSSetShader (g_vs, nullptr, 0);
    context->PSSetShader (altColour ? g_screenPs : g_ps, nullptr, 0);
    // The depth state belongs with the view: writes off for Archicad's own,
    // writes on for our private copy, and neither is a preference.
    ID3D11DepthStencilState* const depthState = depthView != nullptr ? depth::StateForMode (context) : g_depthState;
    context->OMSetDepthStencilState (depthState != nullptr ? depthState : g_depthState, 0);
    context->RSSetState (g_raster);
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_blend, blendFactor, 0xffffffffu);
    // ⚠️ THE QUERY WRAPS THIS DRAW AND NOTHING ELSE. Depth is off and the pixel
    // shader is opaque, so a triangle whose anchor projects inside the frustum
    // MUST produce samples here. Zero samples with a correct projection is a
    // different investigation from a wrong projection, and this is the number
    // that separates them. The clip-space probe is deliberately not measured:
    // it has never been in doubt and counting it would dilute this.
    // ⚠️ PROBE C IS THIS DRAW, NOT A COPY OF IT. A second rendering of the
    // production path through different code would not be the production path,
    // and the whole point of the three probes is that only one thing differs
    // between them.
    oracle::BeginTriangleQuery (context);
    probes::BeginProductionQuery (context);
    context->Draw (3, 0);
    probes::EndProductionQuery (context);
    oracle::EndTriangleQuery (context);

    // ---- the ghost mesh ----------------------------------------------------
    // ⚠️ AFTER THE PROOF PRIMITIVES AND WITH THE SAME CAMERA, THE SAME
    // DEPTH VIEW AND THE SAME DEPTH STATE. Everything that distinguishes this
    // draw from the one above is the geometry itself, so if the triangle
    // composites and the mesh does not, the fault is in the mesh and nowhere
    // else. The pipeline behind it belongs to `GhostMesh`, which is why this is
    // one line: a mesh bug cannot reach the instrument that would diagnose it.
    ghost::Draw (context, wanted, depthState != nullptr ? depthState : g_depthState, g_raster, g_blend);

    // ---- the depth checkpoints ---------------------------------------------
    // ⚠️ THE SAME TWO PRIMITIVES AGAINST EVERY MOMENT OF ARCHICAD'S
    // FRAME. This is where run fifty answers the question run forty-nine could
    // not: not "which draws look transparent" but "after which draw does the
    // depth buffer stop being usable". It draws nothing the user can see -- the
    // probes write to the back buffer through an occlusion query and are
    // overwritten by nothing, because they run last.
    if (targetView != nullptr) {
        checkpoints::Evaluate (context, context1, targetView, sceneViewport.TopLeftX, sceneViewport.TopLeftY,
                               sceneViewport.Width, sceneViewport.Height);
    }

    // ⚠️ NOTHING IS PUT BACK BY HAND ANY MORE. The guard's
    // destructor restores every binding and releases every reference it took,
    // on every path out of this function -- including the early returns a
    // hand-written block keeps forgetting.
}

void SetPoint (Point point)
{
    g_point.store (int (point), std::memory_order_release);
}

Point GetPoint ()
{
    return Point (g_point.load (std::memory_order_acquire));
}

void InjectAtPresent (ID3D11DeviceContext* context, IDXGISwapChain* swapChain, uint64_t modelSceneGeneration)
{
    if (context == nullptr || swapChain == nullptr)
        return;
    // ⚠️ DRAWING REQUIRES A CAMERA, SNAPSHOTTING REQUIRES ONLY THE
    // ARM. `ArmedPendingCamera` deliberately falls through here and draws
    // nothing: there is no camera yet, and drawing without one is what six
    // inconclusive runs were made of.
    if (GetArmState () != ArmState::Active)
        return;
    if (GetPoint () == Point::ScenePass)
        return;
    if (contextstate::Injecting ()) {
        g_skipReentrant.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // ⚠️ THE GUARD IS TAKEN BEFORE THE CLASSIFICATION, NOT AFTER IT, because the
    // oracle's own staging copies and readbacks go out below and they must not be
    // recorded as Archicad's work. They arrive at the same detours on the same
    // context and no pointer filter could separate them.
    contextstate::ScopedInjectionGuard guard;

    // ---- which of the three frame states is this Present? ------------------
    // ⚠️ WHEN A GROUP IS SELECTED THE CAMERA IS `SelectedCameraState` AND NOTHING
    // ELSE. `LastCameraDraw` is the last camera-bearing draw of the LEARNED pass,
    // which is precisely the thing that turned out to be two different cameras;
    // it must not be consulted, not even as a fallback, because a fallback that
    // fires occasionally is indistinguishable from the bug it replaced.
    const CameraSource source = GetCameraSource ();
    if (source == CameraSource::None)
        return;
    // ⚠️ FAIL CLOSED ON A CONVENTION MISMATCH. Drawing with a transform the
    // census did not choose is exactly the situation that cost run thirty-three,
    // and it is invisible in every other counter.
    if (!InterpretationAgrees ()) {
        g_skipInterpretation.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    const contextstate::SceneDrawState fresh =
        source == CameraSource::CensusSelectedGroup ? SnapshotDraw () : contextstate::LastCameraDraw ();
    const bool freshIsUsable = fresh.valid && fresh.vsConstantBuffers[1].numConstants == kExpectedWindowConstants &&
                               fresh.vsConstantBuffers[2].numConstants == kExpectedWindowConstants;

    oracle::FrameState state = oracle::FrameState::InvalidScene;
    bool draw = false;
    // ⚠️ FRAME STATE DOES NOT VETO THE PROBES, AND FOR THIS DIAGNOSTIC IT MUST
    // NOT. The locked camera is numerically valid on 100% of its samples and
    // Present has already produced hundreds of injections; refusing to draw
    // because the NEW/REPEAT/INVALID bookkeeping disagrees would withhold the
    // three sample counts the whole run exists to obtain. The classification is
    // still recorded on every row, so the frame-state semantics can be repaired
    // afterwards on evidence rather than guessed at now.
    const bool snapshotUsable = SnapshotValid ();
    if (!snapshotUsable) {
        g_invalidNoSnapshot.fetch_add (1, std::memory_order_relaxed);
        // ⚠️ NO SNAPSHOT, NO WORLD TRIANGLE. The clip-space probe still draws, so
        // a run with a broken snapshot stays visibly distinguishable from a run
        // with a broken injection.
        g_invalidScene.fetch_add (1, std::memory_order_relaxed);
    }
    else if (freshIsUsable && fresh.modelSceneGeneration != g_lastInjectedModelGeneration) {
        // NEW_SCENE: the model was re-rendered since we last drew, and it came
        // with its own camera. Take it.
        g_acceptedCamera = fresh;
        g_lastInjectedModelGeneration = fresh.modelSceneGeneration;
        g_newScene.fetch_add (1, std::memory_order_relaxed);
        state = oracle::FrameState::NewScene;
        draw = true;
    }
    else if (g_acceptedCamera.valid && modelSceneGeneration == g_lastInjectedModelGeneration) {
        // ⚠️ REPEAT_SCENE, AND IT IS VALID RATHER THAN A COMPROMISE. Archicad
        // presented without re-rendering the model -- a UI repaint, a palette, a
        // cursor -- so the geometry on screen is the geometry this camera drew.
        // Present injection paints the CURRENT back buffer and nothing survives
        // into the next one, so it has to be drawn again every time.
        g_repeatScene.fetch_add (1, std::memory_order_relaxed);
        state = oracle::FrameState::RepeatScene;
        draw = true;
    }
    else {
        // ⚠️ RECORDED, NOT ENFORCED, AND NOW SPLIT BY REASON. The snapshot is
        // valid and the camera is the selected candidate's; the bookkeeping
        // disagreeing about which frame it belongs to is a separate fault, and
        // WHICH disagreement it is decides the fix.
        g_invalidScene.fetch_add (1, std::memory_order_relaxed);
        if (fresh.modelSceneGeneration > modelSceneGeneration) {
            // The snapshot is from a model frame Present has not reached yet.
            g_invalidGenerationMismatch.fetch_add (1, std::memory_order_relaxed);
        }
        else if (fresh.modelSceneGeneration < modelSceneGeneration) {
            // The model advanced and the candidate has not drawn in the new
            // frame yet -- the commonest case, and the one that says the
            // candidate draws before the generation counter increments.
            g_invalidGenerationAdvanced.fetch_add (1, std::memory_order_relaxed);
        }
        else {
            g_invalidNoDrawThisGeneration.fetch_add (1, std::memory_order_relaxed);
        }
        draw = true;
    }

    // ⚠️ THE ROW IS OPENED FOR EVERY TESTED PRESENT, INCLUDING THE REFUSED ONES.
    // A report that only records the Presents that went well cannot explain the
    // ones that did not, and "the triangle disappeared" is a statement about
    // exactly those.
    // ⚠️ EVERY ROW CARRIES WHERE ITS CAMERA CAME FROM, so an accidental fallback
    // to the learner cannot hide inside an aggregate that looks healthy.
    oracle::BeginPresent (renderstate::CurrentPresentGeneration (), state, modelSceneGeneration, uint32_t (source),
                          GetSelectedCamera ().groupId, GetSelectedCamera ().snapshotGeneration,
                          SnapshotDrawSequence (), SnapshotModelGeneration ());

    if (draw) {
        if (!EnsureCreated (context)) {
            g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
        }
        else {
            ID3D11DeviceContext1* context1 = nullptr;
            if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) ||
                context1 == nullptr) {
                g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
            }
            else {
                // ⚠️ THE BACK BUFFER IS FETCHED AND RELEASED HERE, NEVER CACHED.
                // A held view of a swap-chain buffer makes `ResizeBuffers` fail,
                // which would break every window resize in Archicad.
                ID3D11Texture2D* backBuffer = nullptr;
                ID3D11RenderTargetView* targetView = nullptr;
                if (SUCCEEDED (swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &backBuffer)) &&
                    backBuffer != nullptr) {
                    g_device->CreateRenderTargetView (backBuffer, nullptr, &targetView);
                }
                if (targetView != nullptr) {
                    DrawWithCamera (context, context1, g_acceptedCamera, targetView, true, false);
                    // ⚠️ A AND B GO THROUGH THE SAME BACK-BUFFER VIEW AS C, in the
                    // same Present, under the same explicit raster state. Three
                    // sample counts from one frame answer a question that three
                    // separate runs could not.
                    probes::DrawAll (context, context1, targetView, g_acceptedCamera.viewportX,
                                     g_acceptedCamera.viewportY, g_acceptedCamera.viewportWidth,
                                     g_acceptedCamera.viewportHeight);
                    // ⚠️ PROOF B2, IN THE SAME FRAME AS THE CONTROL. Proof B
                    // passed inside the model pass -- which only runs while the
                    // model is redrawn, so its primitives vanish at rest and are
                    // painted over by later draws in the same frame. This asks
                    // whether the model's own depth view is still usable HERE,
                    // where the overlay is always visible. One run, both answers.
                    probes::DrawPresentDepth (context, context1, targetView, g_acceptedCamera.viewportX,
                                              g_acceptedCamera.viewportY, g_acceptedCamera.viewportWidth,
                                              g_acceptedCamera.viewportHeight);
                    g_injected.fetch_add (1, std::memory_order_relaxed);
                    g_injectedPresent.fetch_add (1, std::memory_order_relaxed);
                }
                else {
                    g_backBufferFailures.fetch_add (1, std::memory_order_relaxed);
                }
                ReleaseAndNull (targetView);
                ReleaseAndNull (backBuffer);
                ReleaseAndNull (context1);
            }
        }
    }

    // ⚠️ ALWAYS, EVEN WHEN NOTHING WAS DRAWN. This is what polls the older
    // staging slots and the older occlusion queries, so a Present that skipped
    // its own draw still advances every row that is waiting on the GPU.
    oracle::EndPresent (context);
}

void InjectIfReady (ID3D11DeviceContext* context)
{
    if (GetPoint () == Point::Present)
        return;
    if (context == nullptr || GetArmState () != ArmState::Active)
        return;
    // ⚠️ THE SAME REFUSAL AS THE PRESENT PATH, AND THE SAME SHADER. Both points
    // use `g_vs` and therefore the same camera interpretation; only the pixel
    // shader differs, so that the two can be told apart on screen.
    if (!InterpretationAgrees ()) {
        g_skipInterpretation.fetch_add (1, std::memory_order_relaxed);
        return;
    }

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
    if (draw.scenePassGeneration != pass.generation || draw.sceneTargetEpoch != pass.targetEpoch) {
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
    if (view.numConstants != kExpectedWindowConstants || projection.numConstants != kExpectedWindowConstants) {
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
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) || context1 == nullptr) {
        g_skipNotReady.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // In-pass injection happens while the ring window is still current, so it
    // does not need the snapshot -- but it uses it anyway for one reason: two
    // code paths that read the camera differently would eventually disagree.
    // ⚠️ GREEN AT THE SCENE PASS, MAGENTA AT PRESENT. Seeing one and not the
    // other says which of the two points is at fault without a second run.
    DrawWithCamera (context, context1, draw, nullptr, true, true);
    ReleaseAndNull (context1);

    g_injected.fetch_add (1, std::memory_order_relaxed);
    g_injectedScenePass.fetch_add (1, std::memory_order_relaxed);
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
    stats.invalidNoSnapshot = g_invalidNoSnapshot.load (std::memory_order_relaxed);
    stats.invalidNoDrawThisGeneration = g_invalidNoDrawThisGeneration.load (std::memory_order_relaxed);
    stats.invalidGenerationAdvanced = g_invalidGenerationAdvanced.load (std::memory_order_relaxed);
    stats.invalidGenerationMismatch = g_invalidGenerationMismatch.load (std::memory_order_relaxed);
    const CameraStats camera = GetCameraStats ();
    stats.snapshotsTaken = camera.snapshotsTaken;
    stats.snapshotSequence = camera.snapshotsTaken;
    stats.qualifyingCameraDraws = camera.qualifyingCameraDraws;
    stats.selectedGroupDraws = camera.selectedGroupDraws;
    stats.selectedGroupSnapshots = camera.selectedGroupSnapshots;
    stats.selectedGroupId = camera.selectedGroupId;
    stats.selectedSnapshotGeneration = camera.selectedSnapshotGeneration;
    stats.injectedPresent = g_injectedPresent.load (std::memory_order_relaxed);
    stats.injectedScenePass = g_injectedScenePass.load (std::memory_order_relaxed);
    stats.shaderInterpretation = ShaderInterpretation ();
    stats.expectedInterpretation = ExpectedInterpretation ();
    stats.interpretationAgrees = InterpretationAgrees ();
    stats.skippedInterpretation = g_skipInterpretation.load (std::memory_order_relaxed);
    stats.viewCopies = camera.viewCopies;
    stats.projectionCopies = camera.projectionCopies;
    stats.snapshotValid = camera.snapshotValid;
    stats.occurrenceDraws = camera.occurrenceDraws;
    stats.authoritativeSnapshots = camera.authoritativeSnapshots;
    stats.occurrenceModelFrames = camera.occurrenceModelFrames;
    stats.occurrenceLocked = camera.occurrenceLocked;
    stats.lockedOccurrence = camera.lockedOccurrence;
    stats.initialised = g_initialised;
    strncpy_s (stats.lastError, sizeof (stats.lastError), g_lastError, _TRUNCATE);
    return stats;
}

} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
