// ArchViz/Dxgi/GhostMesh -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/GhostMesh.hpp"

#include "ArchViz/Dxgi/CameraShaderSource.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace ghost {

namespace {

ID3D11Device* g_device = nullptr;
ID3D11Buffer* g_vertexBuffer = nullptr;
ID3D11Buffer* g_indexBuffer = nullptr;

// ⚠️ THE MESH OWNS ITS OWN PIPELINE. The renderer's proof primitives
// are the regression test for every camera and depth claim under this directory;
// if a mesh bug could break their shaders or their input layout, the instrument
// would go down with the thing it exists to diagnose.
ID3D11VertexShader* g_vsVariant[camerashader::kDeclarableVariants] = {};
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;

bool g_created = false;
bool g_createFailed = false;

std::atomic<bool> g_enabled { false };
std::atomic<bool> g_animated { true };

float g_anchorX = 0.0f;
float g_anchorY = 0.0f;
float g_anchorZ = 0.0f;
float g_anchorSize = 1.0f;

uint32_t g_phase = 0;
Stats g_stats;

Vertex g_vertices[kMaxVertices];
uint32_t g_indices[kMaxIndices];
uint32_t g_vertexCount = 0;
uint32_t g_indexCount = 0;

// ⚠️ THE INDEX BUFFER IS BUILT ONCE AND THE VERTICES EVERY FRAME. The topology
// of this scene never changes -- four boxes, the same 576 indices -- so
// re-uploading it per frame would be work that proves nothing. The `GhostMesh`
// API this becomes will have to re-upload both when an object's triangle count
// changes, and that is a different question from "can vertices move".
bool g_topologyBuilt = false;

// Where each part lives inside the one index buffer.
uint32_t g_partFirstIndex[uint32_t (Part::kCount)] = {};
uint32_t g_partIndexCount[uint32_t (Part::kCount)] = {};

// ⚠️ THE STATES ARE OURS AND THE STYLE PICKS BETWEEN THEM. Three
// depth behaviours and two rasterizer behaviours cover the three overlay kinds;
// anything the style cannot express here is a gap in the policy, not a reason to
// branch on the part.
ID3D11DepthStencilState* g_depthTestWrite = nullptr;
ID3D11DepthStencilState* g_depthTestOnly = nullptr;
ID3D11DepthStencilState* g_depthNone = nullptr;
ID3D11RasterizerState* g_rasterPlain = nullptr;
ID3D11RasterizerState* g_rasterBiased = nullptr;

// ⚠️ OPACITY THROUGH THE BLEND FACTOR, NOT A CONSTANT BUFFER. `b1`
// and `b2` are Archicad's camera windows and nothing of ours may go near them;
// a third cbuffer would be a third thing to bind, restore and get wrong. The
// blend factor is a pipeline value that already exists for exactly this.
ID3D11BlendState* g_blendOpacity = nullptr;

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

// ⚠️ THE DECLARATIONS COME FROM `CameraShaderSource.hpp`, SO THIS
// READS ARCHICAD'S CAMERA THROUGH EXACTLY THE SAME TWO LINES AS THE PROOF
// TRIANGLE. A private copy of them here could drift from the interpretation the
// census selected and nothing in the pipeline would compare the two -- which is
// the failure run thirty-three spent itself on.
//
// The one difference is the colour carried through. The proof pixel shader
// returns a constant, which makes self-occlusion literally invisible: a nearer
// surface hiding a farther one of the same colour looks like no surface at all.
const char* const kGhostShaderBody = "struct GhostOut { float4 position : SV_POSITION; float3 tint : COLOR; };\n"
                                     "GhostOut VSGhost (float3 position : POSITION, float3 tint : COLOR)\n"
                                     "{\n"
                                     "    GhostOut o;\n"
                                     "    float4 p = float4 (position, 1.0);\n"
                                     "    p = mul (p, View);\n"
                                     "    p = mul (p, Projection);\n"
                                     "    o.position = p;\n"
                                     "    o.tint = tint;\n"
                                     "    return o;\n"
                                     "}\n"
                                     "float4 PSGhost (GhostOut input) : SV_TARGET\n"
                                     "{\n"
                                     "    return float4 (input.tint, 1.0);\n"
                                     "}\n";

// ---------------------------------------------------------------------------
// The scene
// ---------------------------------------------------------------------------

// ⚠️ THE OVERLAPS ARE THE TEST, SO THEY ARE CHOSEN AND NOT INCIDENTAL. Two
// cubes offset by less than their width MUST intersect; a bar three anchor-sizes
// long MUST pass through both and out the far side into whatever the model has
// there; an upright bar MUST cross that bar without touching the cubes. Every
// acceptance question has a known right answer before Archicad is even running.
struct Box {
    float centre[3]; // in anchor sizes, relative to the anchor
    float half[3];   // in anchor sizes
    float colour[3];
};

const Box kScene[kBoxCount] = {
    // Two intersecting cubes: 0.9 apart, 0.5 half-extent each, so they overlap
    // by 0.1 in x and share a corner region. The seam between them is what
    // self-occlusion has to render correctly.
    { { -0.55f, 0.00f, 0.00f }, { 0.50f, 0.50f, 0.50f }, { 0.20f, 0.85f, 1.00f } }, // cyan
    { { 0.35f, 0.25f, 0.15f }, { 0.50f, 0.50f, 0.50f }, { 1.00f, 0.20f, 0.85f } },  // magenta

    // A long bar through both of them and far out either side. ⚠️ THIS IS THE
    // ONE THAT TESTS ARCHICAD'S DEPTH: at a two-metre anchor it reaches six
    // metres each way, so it cannot stay inside whatever room the anchor is in,
    // and the building must cut it.
    { { 0.00f, 0.00f, 0.00f }, { 3.00f, 0.12f, 0.12f }, { 1.00f, 0.85f, 0.15f } }, // yellow

    // And an upright bar crossing the yellow one clear of the cubes.
    { { 1.40f, -0.30f, 0.00f }, { 0.12f, 0.12f, 2.00f }, { 1.00f, 0.45f, 0.10f } }, // orange
};

// A fixed light, so every face of every box has a different, stable shade and a
// hidden surface is visibly hidden rather than merely coincident.
void Shade (const float normal[3], const float colour[3], float out[3])
{
    const float lx = 0.40f, ly = 0.52f, lz = 0.76f;
    float lambert = normal[0] * lx + normal[1] * ly + normal[2] * lz;
    if (lambert < 0.0f)
        lambert = -lambert * 0.55f; // back faces lit dimly, never black
    const float shade = 0.35f + 0.65f * lambert;
    out[0] = colour[0] * shade;
    out[1] = colour[1] * shade;
    out[2] = colour[2] * shade;
}

// ⚠️ A DETERMINISTIC DISPLACEMENT, AND SMALL. It exists to prove the buffer can
// change every frame, not to make a pretty effect: at a few percent of the
// anchor size a camera-lock error of even one pixel is still the loudest thing
// on screen. The seed is the vertex index, so the same phase always produces the
// same mesh and two runs can be compared.
float Breath (uint32_t vertex, uint32_t phase)
{
    if (!g_animated.load (std::memory_order_acquire))
        return 0.0f;
    const float t = float (phase) * 0.06f + float (vertex) * 0.35f;
    return 0.06f * g_anchorSize * std::sin (t);
}

void BuildScene (uint32_t phase)
{
    g_vertexCount = 0;
    g_indexCount = 0;

    // The six faces, as an origin corner plus two edge vectors and a normal.
    const int axisU[6] = { 1, 2, 0, 2, 0, 1 };
    const int axisV[6] = { 2, 1, 2, 0, 1, 0 };
    const int axisN[6] = { 0, 0, 1, 1, 2, 2 };
    const float sign[6] = { +1.0f, -1.0f, +1.0f, -1.0f, +1.0f, -1.0f };

    for (uint32_t box = 0; box < kBoxCount; ++box) {
        const Box& shape = kScene[box];
        for (uint32_t face = 0; face < 6; ++face) {
            const int u = axisU[face];
            const int v = axisV[face];
            const int n = axisN[face];

            float normal[3] = { 0.0f, 0.0f, 0.0f };
            normal[n] = sign[face];
            float colour[3];
            Shade (normal, shape.colour, colour);

            const uint32_t base = g_vertexCount;
            // A 3x3 grid of vertices -> a 2x2 grid of quads -> 8 triangles.
            for (uint32_t iv = 0; iv < 3; ++iv) {
                for (uint32_t iu = 0; iu < 3; ++iu) {
                    const float fu = -1.0f + float (iu); // -1, 0, +1
                    const float fv = -1.0f + float (iv);

                    float local[3] = { 0.0f, 0.0f, 0.0f };
                    local[u] = fu * shape.half[u];
                    local[v] = fv * shape.half[v];
                    local[n] = sign[face] * shape.half[n];

                    const float push = Breath (g_vertexCount, phase);
                    Vertex& out = g_vertices[g_vertexCount];
                    out.x = g_anchorX + (shape.centre[0] + local[0]) * g_anchorSize + normal[0] * push;
                    out.y = g_anchorY + (shape.centre[1] + local[1]) * g_anchorSize + normal[1] * push;
                    out.z = g_anchorZ + (shape.centre[2] + local[2]) * g_anchorSize + normal[2] * push;
                    out.r = colour[0];
                    out.g = colour[1];
                    out.b = colour[2];
                    ++g_vertexCount;
                }
            }

            if (g_topologyBuilt)
                continue;
            for (uint32_t qv = 0; qv < 2; ++qv) {
                for (uint32_t qu = 0; qu < 2; ++qu) {
                    const uint32_t a = base + qv * 3 + qu;
                    const uint32_t b = a + 1;
                    const uint32_t c = a + 3;
                    const uint32_t d = c + 1;
                    g_indices[g_indexCount++] = a;
                    g_indices[g_indexCount++] = c;
                    g_indices[g_indexCount++] = b;
                    g_indices[g_indexCount++] = b;
                    g_indices[g_indexCount++] = c;
                    g_indices[g_indexCount++] = d;
                }
            }
        }
    }
    g_partFirstIndex[uint32_t (Part::Solid)] = 0;
    g_partIndexCount[uint32_t (Part::Solid)] = g_topologyBuilt ? kBoxCount * kIndicesPerBox : g_indexCount;

    // ---- the wireframe: the twelve edges of a box around the whole scene ----
    // ⚠️ A LINE LIST, WHICH IS WHY IT CANNOT SHARE THE SOLID'S DRAW.
    // It crosses the cubes and the bars, so it is the part that shows whether an
    // analysis overlay is correctly hidden by ghost geometry AND by the building
    // while still being drawn faded where a policy says it should be.
    {
        const uint32_t base = g_vertexCount;
        const float half = 2.2f * g_anchorSize;
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const float sx = (corner & 1) ? 1.0f : -1.0f;
            const float sy = (corner & 2) ? 1.0f : -1.0f;
            const float sz = (corner & 4) ? 1.0f : -1.0f;
            Vertex& out = g_vertices[g_vertexCount++];
            out.x = g_anchorX + sx * half;
            out.y = g_anchorY + sy * half;
            out.z = g_anchorZ + sz * half * 0.5f;
            out.r = 0.15f;
            out.g = 1.00f;
            out.b = 0.55f;
        }
        if (!g_topologyBuilt) {
            const uint32_t edges[24] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7 };
            g_partFirstIndex[uint32_t (Part::Wireframe)] = g_indexCount;
            for (uint32_t i = 0; i < kWireIndices; ++i)
                g_indices[g_indexCount++] = base + edges[i];
            g_partIndexCount[uint32_t (Part::Wireframe)] = kWireIndices;
        }
    }

    // ---- the heatmap: a subdivided quad where a host surface would be -------
    // ⚠️ COPLANAR ON PURPOSE. It sits on a plane through the anchor,
    // which is the case a heatmap always has and the one that z-fights without a
    // bias. If it stipples, the bias is wrong; if it disappears, the policy is.
    {
        const uint32_t base = g_vertexCount;
        const float extent = 2.0f * g_anchorSize;
        for (uint32_t row = 0; row < kHeatGrid; ++row) {
            for (uint32_t col = 0; col < kHeatGrid; ++col) {
                const float u = float (col) / float (kHeatGrid - 1);
                const float v = float (row) / float (kHeatGrid - 1);
                Vertex& out = g_vertices[g_vertexCount++];
                out.x = g_anchorX + (u * 2.0f - 1.0f) * extent;
                out.y = g_anchorY - 1.8f * g_anchorSize;
                out.z = g_anchorZ + (v * 2.0f - 1.0f) * extent * 0.5f;
                // A blue-to-red ramp, so a wrong orientation is obvious.
                const float t = u * 0.6f + v * 0.4f;
                out.r = t;
                out.g = 0.25f + 0.35f * (1.0f - std::fabs (t - 0.5f) * 2.0f);
                out.b = 1.0f - t;
            }
        }
        if (!g_topologyBuilt) {
            g_partFirstIndex[uint32_t (Part::Heatmap)] = g_indexCount;
            for (uint32_t row = 0; row + 1 < kHeatGrid; ++row) {
                for (uint32_t col = 0; col + 1 < kHeatGrid; ++col) {
                    const uint32_t a = base + row * kHeatGrid + col;
                    const uint32_t b = a + 1;
                    const uint32_t d = a + kHeatGrid;
                    const uint32_t e = d + 1;
                    g_indices[g_indexCount++] = a;
                    g_indices[g_indexCount++] = d;
                    g_indices[g_indexCount++] = b;
                    g_indices[g_indexCount++] = b;
                    g_indices[g_indexCount++] = d;
                    g_indices[g_indexCount++] = e;
                }
            }
            g_partIndexCount[uint32_t (Part::Heatmap)] = kHeatIndices;
        }
    }

    if (!g_topologyBuilt)
        g_topologyBuilt = true;
    else
        g_indexCount = kMaxIndices;
    g_stats.solidIndices = g_partIndexCount[uint32_t (Part::Solid)];
    g_stats.wireIndices = g_partIndexCount[uint32_t (Part::Wireframe)];
    g_stats.heatIndices = g_partIndexCount[uint32_t (Part::Heatmap)];

    ++g_stats.builds;
    g_stats.vertices = g_vertexCount;
    g_stats.indices = g_indexCount;
    g_stats.triangles = g_indexCount / 3;
    g_stats.phase = phase;
}

bool EnsureShaders ()
{
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    char source[camerashader::kMaxSource] = {};
    ID3DBlob* first = nullptr;
    ID3DBlob* errors = nullptr;
    bool ok = true;

    for (uint32_t variant = 0; variant < camerashader::kDeclarableVariants && ok; ++variant) {
        if (!camerashader::Compose (variant, kGhostShaderBody, source, sizeof (source))) {
            Fail ("the ghost shader source could not be composed");
            return false;
        }
        ID3DBlob* blob = nullptr;
        if (FAILED (D3DCompile (source, strlen (source), "TapiocaGhost", nullptr, nullptr, "VSGhost", "vs_5_0", flags,
                                0, &blob, &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                    : "a ghost vertex shader would not compile");
            ok = false;
        }
        else {
            ok = SUCCEEDED (g_device->CreateVertexShader (blob->GetBufferPointer (), blob->GetBufferSize (), nullptr,
                                                          &g_vsVariant[variant]));
            if (ok && variant == 0) {
                first = blob;
                blob = nullptr;
            }
        }
        ReleaseAndNull (errors);
        ReleaseAndNull (blob);
    }

    ID3DBlob* pixel = nullptr;
    if (ok && FAILED (D3DCompile (source, strlen (source), "TapiocaGhost", nullptr, nullptr, "PSGhost", "ps_5_0", flags,
                                  0, &pixel, &errors))) {
        Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                : "the ghost pixel shader would not compile");
        ok = false;
    }
    ReleaseAndNull (errors);
    ok = ok && pixel != nullptr &&
         SUCCEEDED (g_device->CreatePixelShader (pixel->GetBufferPointer (), pixel->GetBufferSize (), nullptr, &g_ps));

    // ⚠️ THE LAYOUT MUST MATCH `Vertex` EXACTLY. A layout that
    // disagrees with the struct reads colour out of the next vertex's position
    // and produces geometry that is subtly, unfalsifiably wrong -- the hardest
    // kind of bug to see in a picture.
    const D3D11_INPUT_ELEMENT_DESC elements[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ok = ok && first != nullptr &&
         SUCCEEDED (
             g_device->CreateInputLayout (elements, 2, first->GetBufferPointer (), first->GetBufferSize (), &g_layout));
    ReleaseAndNull (first);
    ReleaseAndNull (pixel);
    if (!ok && g_stats.lastError[0] == 0)
        Fail ("the ghost pipeline could not be created");
    return ok;
}

bool EnsureCreated (ID3D11DeviceContext* context)
{
    if (g_created)
        return true;
    if (g_createFailed || context == nullptr)
        return false;

    context->GetDevice (&g_device);
    if (g_device == nullptr) {
        g_createFailed = true;
        Fail ("the context has no device");
        return false;
    }

    // ⚠️ DYNAMIC AND CPU-WRITABLE, WHICH IS THE WHOLE DIFFERENCE FROM THE PROOF
    // PRIMITIVE. That one is `IMMUTABLE` because it never had to change; this
    // one is rewritten every injected frame.
    D3D11_BUFFER_DESC vertexDesc = {};
    vertexDesc.ByteWidth = UINT (sizeof (Vertex) * kMaxVertices);
    vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED (g_device->CreateBuffer (&vertexDesc, nullptr, &g_vertexBuffer))) {
        g_createFailed = true;
        Fail ("the ghost vertex buffer could not be created");
        return false;
    }

    D3D11_BUFFER_DESC indexDesc = vertexDesc;
    indexDesc.ByteWidth = UINT (sizeof (uint32_t) * kMaxIndices);
    indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED (g_device->CreateBuffer (&indexDesc, nullptr, &g_indexBuffer))) {
        g_createFailed = true;
        Fail ("the ghost index buffer could not be created");
        ReleaseAndNull (g_vertexBuffer);
        return false;
    }

    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        bool ok = SUCCEEDED (g_device->CreateDepthStencilState (&desc, &g_depthTestWrite));
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&desc, &g_depthTestOnly));
        desc.DepthEnable = FALSE;
        ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&desc, &g_depthNone));

        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = FALSE;
        ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&raster, &g_rasterPlain));
        // ⚠️ THE BIAS IS IN DEPTH-BUFFER UNITS, NOT METRES. For a 24
        // bit buffer one unit is 1/2^24, so a style asking for 0.0005 of NDC is
        // about eight thousand of them. `SlopeScaledDepthBias` handles the
        // grazing angles a flat map on a wall will hit at the edges of view.
        raster.DepthBias = 8000;
        raster.SlopeScaledDepthBias = 1.5f;
        raster.DepthBiasClamp = 0.01f;
        ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&raster, &g_rasterBiased));

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
            g_createFailed = true;
            Fail ("the ghost overlay states could not be created");
            return false;
        }
    }

    if (!EnsureShaders ()) {
        g_createFailed = true;
        ReleaseAndNull (g_indexBuffer);
        ReleaseAndNull (g_vertexBuffer);
        return false;
    }

    g_created = true;
    g_stats.created = true;
    return true;
}

// ⚠️ `WRITE_DISCARD` ON A BUFFER WE OWN, AND NEVER ANYTHING ELSE. Discard hands
// back fresh storage rather than waiting for the GPU to finish with the last
// frame's, which is what makes a per-frame rewrite free of a stall. Archicad's
// constant ring is still never mapped, here or anywhere.
bool Upload (ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED (context->Map (buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr) {
        ++g_stats.uploadFailures;
        Fail ("a ghost buffer could not be mapped");
        return false;
    }
    std::memcpy (mapped.pData, data, bytes);
    context->Unmap (buffer, 0);
    ++g_stats.uploads;
    return true;
}

} // namespace

uint32_t Prepare (ID3D11DeviceContext* context)
{
    if (!Enabled () || context == nullptr)
        return 0;
    if (!EnsureCreated (context))
        return 0;

    const bool first = !g_topologyBuilt;
    BuildScene (g_phase++);

    if (!Upload (context, g_vertexBuffer, g_vertices, sizeof (Vertex) * g_vertexCount))
        return 0;
    // ⚠️ THE INDICES GO UP ONCE. Their content cannot change while the
    // scene is a fixed set of parts, and uploading them every frame would measure
    // nothing except the upload.
    if (first && !Upload (context, g_indexBuffer, g_indices, sizeof (uint32_t) * g_indexCount))
        return 0;
    return g_indexCount;
}

void SetAnchor (float x, float y, float z, float sizeMetres)
{
    g_anchorX = x;
    g_anchorY = y;
    g_anchorZ = z;
    g_anchorSize = (sizeMetres > 0.001f) ? sizeMetres : 1.0f;
}

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
    g_stats.enabled = enabled;
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void SetAnimated (bool animated)
{
    g_animated.store (animated, std::memory_order_release);
    g_stats.animated = animated;
}

bool Animated ()
{
    return g_animated.load (std::memory_order_acquire);
}

void Draw (ID3D11DeviceContext* context, uint32_t interpretation, ID3D11DepthStencilState* depthState,
           ID3D11RasterizerState* raster, ID3D11BlendState* blend)
{
    if (Prepare (context) == 0)
        return;
    DrawCurrent (context, interpretation, depthState, raster, blend);
}

uint32_t DrawPart (ID3D11DeviceContext* context, uint32_t interpretation, Part part, const overlay::OverlayStyle& style,
                   ID3D11DepthStencilView* depthView)
{
    const uint32_t index = uint32_t (part);
    if (context == nullptr || index >= uint32_t (Part::kCount) || !g_created)
        return 0;
    const uint32_t count = g_partIndexCount[index];
    if (count == 0 || g_layout == nullptr || g_ps == nullptr)
        return 0;

    const uint32_t variant =
        (interpretation < camerashader::kDeclarableVariants && g_vsVariant[interpretation] != nullptr) ? interpretation
                                                                                                       : 0;
    if (g_vsVariant[variant] == nullptr)
        return 0;

    // ⚠️ THE STYLE CHOOSES THE STATE; THIS FILE DOES NOT KNOW WHAT A
    // WIREFRAME IS. `selfOcclusion` picks whether the depth test runs at all and
    // `depthWrite` whether our own geometry joins the buffer -- which is the
    // whole difference between a solid that hides itself and a wireframe that
    // must not.
    ID3D11DepthStencilState* depthState = g_depthNone;
    if (depthView != nullptr && style.selfOcclusion)
        depthState = style.depthWrite ? g_depthTestWrite : g_depthTestOnly;
    context->OMSetDepthStencilState (depthState, 0);
    context->RSSetState (style.depthBias > 0.0f ? g_rasterBiased : g_rasterPlain);

    const FLOAT factor[4] = { style.opacity, style.opacity, style.opacity, style.opacity };
    context->OMSetBlendState (g_blendOpacity, factor, 0xffffffffu);

    const UINT stride = sizeof (Vertex);
    const UINT offset = 0;
    context->IASetInputLayout (g_layout);
    context->IASetVertexBuffers (0, 1, &g_vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer (g_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    context->IASetPrimitiveTopology (part == Part::Wireframe ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                             : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (g_vsVariant[variant], nullptr, 0);
    context->PSSetShader (g_ps, nullptr, 0);
    context->DrawIndexed (count, g_partFirstIndex[index], 0);
    ++g_stats.draws;
    return count;
}

uint32_t DrawCurrent (ID3D11DeviceContext* context, uint32_t interpretation, ID3D11DepthStencilState* depthState,
                      ID3D11RasterizerState* raster, ID3D11BlendState* blend)
{
    const uint32_t indices = g_indexCount;
    if (context == nullptr || indices == 0 || !g_created || g_layout == nullptr || g_ps == nullptr)
        return 0;

    // ⚠️ VARIANTS 4 TO 7 ARE REVERSED MULTIPLICATION ORDERS, which no
    // cbuffer declaration can express. Falling back to 0 is the same refusal the
    // proof triangle makes: draw the reading that was measured, or the default,
    // and never a third thing nobody chose.
    const uint32_t variant =
        (interpretation < camerashader::kDeclarableVariants && g_vsVariant[interpretation] != nullptr) ? interpretation
                                                                                                       : 0;
    if (g_vsVariant[variant] == nullptr)
        return 0;

    const UINT stride = sizeof (Vertex);
    const UINT offset = 0;
    context->IASetInputLayout (g_layout);
    context->IASetVertexBuffers (0, 1, &g_vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer (g_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (g_vsVariant[variant], nullptr, 0);
    context->PSSetShader (g_ps, nullptr, 0);
    if (depthState != nullptr)
        context->OMSetDepthStencilState (depthState, 0);
    if (raster != nullptr)
        context->RSSetState (raster);
    if (blend != nullptr) {
        const FLOAT factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState (blend, factor, 0xffffffffu);
    }
    context->DrawIndexed (indices, 0, 0);
    ++g_stats.draws;
    return indices;
}

void Shutdown ()
{
    ReleaseAndNull (g_blendOpacity);
    ReleaseAndNull (g_rasterBiased);
    ReleaseAndNull (g_rasterPlain);
    ReleaseAndNull (g_depthNone);
    ReleaseAndNull (g_depthTestOnly);
    ReleaseAndNull (g_depthTestWrite);
    ReleaseAndNull (g_layout);
    ReleaseAndNull (g_ps);
    for (uint32_t variant = 0; variant < camerashader::kDeclarableVariants; ++variant)
        ReleaseAndNull (g_vsVariant[variant]);
    ReleaseAndNull (g_indexBuffer);
    ReleaseAndNull (g_vertexBuffer);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_topologyBuilt = false;
    g_phase = 0;
    g_vertexCount = 0;
    g_indexCount = 0;
    g_stats = Stats {};
}

Stats GetStats ()
{
    return g_stats;
}

} // namespace ghost
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
