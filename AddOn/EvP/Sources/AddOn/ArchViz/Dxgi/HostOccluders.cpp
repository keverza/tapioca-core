// ArchViz/Dxgi/HostOccluders -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/HostOccluders.hpp"

#include "ArchViz/Dxgi/CameraShaderSource.hpp"
#include "ArchViz/Dxgi/InjectionCamera.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hostocclusion {

namespace {

constexpr uint32_t kCameraWindowConstants = 16;

// ⚠️ A CEILING, AND OVERFLOW IS COUNTED RATHER THAN WRAPPED. A whole building is
// far more than an overlay needs to be occluded by, and silently dropping the
// far half would hide the overlay in some directions and not others -- which
// looks exactly like a camera fault. Two million vertices is roughly 24 MB of
// position data and covers the models this has been run against.
constexpr uint32_t kMaxVertices = 2u * 1000u * 1000u;
constexpr uint32_t kMaxIndices = 6u * 1000u * 1000u;

// ⚠️ POSITION ONLY. This renders depth and nothing else, so a colour channel
// would be bytes moved for no pixel. The declarations come from
// `CameraShaderSource.hpp` so this reads Archicad's camera through exactly the
// same two lines as every other injected draw.
const char* const kHostShaderBody = "float4 VSHost (float3 position : POSITION) : SV_POSITION\n"
                                    "{\n"
                                    "    float4 p = float4 (position, 1.0);\n"
                                    "    p = mul (p, View);\n"
                                    "    p = mul (p, Projection);\n"
                                    "    return p;\n"
                                    "}\n";

// ⚠️ IMMUTABLE ONCE PUBLISHED. The producer fills one of these and hands it over
// with a single atomic exchange; nothing ever writes to a snapshot the render
// thread can see. That is what removes the mutex from Present.
struct Snapshot {
    std::vector<float> positions; // xyz interleaved, world metres
    std::vector<uint32_t> indices;
};

std::atomic<Snapshot*> g_published { nullptr };
Snapshot* g_building = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11Buffer* g_vertexBuffer = nullptr;
ID3D11Buffer* g_indexBuffer = nullptr;
ID3D11VertexShader* g_vsVariant[camerashader::kDeclarableVariants] = {};
ID3D11InputLayout* g_layout = nullptr;
ID3D11DepthStencilState* g_writeDepth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11BlendState* g_noColour = nullptr;

ID3D11Texture2D* g_depthTexture = nullptr;
ID3D11DepthStencilView* g_depthView = nullptr;
D3D11_TEXTURE2D_DESC g_depthDesc = {};

uint32_t g_uploadedIndices = 0;
bool g_created = false;
bool g_createFailed = false;
Stats g_stats;

// ⚠️ THE CUMULATIVE COUNTERS CANNOT CHECK THE INVARIANT. `opaqueTriangles`
// counts every batch this session; `indices` holds one. The publication rule --
// opaque triangles arrived, therefore indices were added, therefore a publish
// was attempted -- is about ONE batch, so it needs one batch's numbers.
uint64_t g_batchElements = 0;
uint64_t g_batchOpaqueIndices = 0;

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

void PublishFailed (const char* why)
{
    strncpy_s (g_stats.publishFailureReason, sizeof (g_stats.publishFailureReason), why, _TRUNCATE);
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

    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    char source[camerashader::kMaxSource] = {};
    ID3DBlob* first = nullptr;
    ID3DBlob* errors = nullptr;
    bool ok = true;
    for (uint32_t variant = 0; variant < camerashader::kDeclarableVariants && ok; ++variant) {
        if (!camerashader::Compose (variant, kHostShaderBody, source, sizeof (source))) {
            Fail ("the host occluder shader source could not be composed");
            g_createFailed = true;
            return false;
        }
        ID3DBlob* blob = nullptr;
        if (FAILED (D3DCompile (source, strlen (source), "TapiocaHostOccluder", nullptr, nullptr, "VSHost", "vs_5_0",
                                flags, 0, &blob, &errors))) {
            Fail (errors != nullptr ? (const char*) errors->GetBufferPointer ()
                                    : "the host occluder vertex shader would not compile");
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

    const D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
                                               0,          0, D3D11_INPUT_PER_VERTEX_DATA,
                                               0 };
    ok = ok && first != nullptr &&
         SUCCEEDED (
             g_device->CreateInputLayout (&element, 1, first->GetBufferPointer (), first->GetBufferSize (), &g_layout));
    ReleaseAndNull (first);

    // ⚠️ WRITES DEPTH AND NOTHING ELSE. This IS the occluder, so its whole
    // purpose is the depth value; the colour mask is zero so it cannot put a
    // pixel of the building over the frame Archicad already drew.
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    ok = ok && SUCCEEDED (g_device->CreateDepthStencilState (&depthDesc, &g_writeDepth));

    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    // ⚠️ NO CULLING, BECAUSE EXTRACTED WINDING IS NOT GUARANTEED. A wall whose
    // triangles face away would contribute no depth and would stop occluding --
    // which reads as "the overlay ignores that wall" and would be blamed on the
    // classifier rather than on a winding order.
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.ScissorEnable = FALSE;
    ok = ok && SUCCEEDED (g_device->CreateRasterizerState (&rasterDesc, &g_raster));

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
    ok = ok && SUCCEEDED (g_device->CreateBlendState (&blendDesc, &g_noColour));

    D3D11_BUFFER_DESC vertexDesc = {};
    vertexDesc.ByteWidth = UINT (sizeof (float) * 3 * kMaxVertices);
    vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    // ⚠️ ALLOCATED AT THE CEILING RATHER THAN GROWN. Growing would mean
    // destroying a buffer the GPU may still be reading on a path that runs
    // inside Archicad's frame; the memory is the price of never doing that.
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&vertexDesc, nullptr, &g_vertexBuffer));

    D3D11_BUFFER_DESC indexDesc = vertexDesc;
    indexDesc.ByteWidth = UINT (sizeof (uint32_t) * kMaxIndices);
    indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ok = ok && SUCCEEDED (g_device->CreateBuffer (&indexDesc, nullptr, &g_indexBuffer));

    if (!ok) {
        if (g_stats.lastError[0] == 0)
            Fail ("the host occluder pipeline could not be created");
        g_createFailed = true;
        return false;
    }
    g_created = true;
    g_stats.ready = true;
    return true;
}

// The depth target matches Archicad's own, so the overlay's viewport, resolution
// and format are identical to the ones its camera was built for.
bool EnsureDepthTarget (ID3D11DeviceContext* context)
{
    ID3D11DepthStencilView* const sceneView = injection::depth::SceneView ();
    if (sceneView == nullptr)
        return false;
    ID3D11Resource* resource = nullptr;
    sceneView->GetResource (&resource);
    if (resource == nullptr)
        return false;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED (resource->QueryInterface (__uuidof (ID3D11Texture2D), (void**) &texture)) || texture == nullptr) {
        ReleaseAndNull (resource);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc (&desc);
    ReleaseAndNull (texture);
    ReleaseAndNull (resource);

    if (g_depthView != nullptr && desc.Width == g_depthDesc.Width && desc.Height == g_depthDesc.Height &&
        desc.Format == g_depthDesc.Format && desc.SampleDesc.Count == g_depthDesc.SampleDesc.Count) {
        return true;
    }
    ReleaseAndNull (g_depthView);
    ReleaseAndNull (g_depthTexture);

    D3D11_TEXTURE2D_DESC ours = desc;
    ours.Usage = D3D11_USAGE_DEFAULT;
    ours.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ours.CPUAccessFlags = 0;
    ours.MiscFlags = 0;
    if (FAILED (g_device->CreateTexture2D (&ours, nullptr, &g_depthTexture))) {
        Fail ("the host occluder depth texture could not be created");
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
    viewDesc.Format = DepthViewFormat (desc.Format);
    viewDesc.ViewDimension =
        desc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED (g_device->CreateDepthStencilView (g_depthTexture, &viewDesc, &g_depthView))) {
        Fail ("the host occluder depth view could not be created");
        ReleaseAndNull (g_depthTexture);
        return false;
    }
    g_depthDesc = desc;
    g_stats.width = desc.Width;
    g_stats.height = desc.Height;
    return true;
}

bool Upload (ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, size_t bytes)
{
    if (bytes == 0)
        return true;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED (context->Map (buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr) {
        Fail ("a host occluder buffer could not be mapped");
        return false;
    }
    std::memcpy (mapped.pData, data, bytes);
    context->Unmap (buffer, 0);
    return true;
}

// Take a newly published snapshot, if there is one, and put it on the GPU.
bool TakeAndUpload (ID3D11DeviceContext* context)
{
    Snapshot* const snapshot = g_published.exchange (nullptr, std::memory_order_acq_rel);
    if (snapshot == nullptr)
        return g_uploadedIndices > 0;

    const bool ok =
        Upload (context, g_vertexBuffer, snapshot->positions.data (), sizeof (float) * snapshot->positions.size ()) &&
        Upload (context, g_indexBuffer, snapshot->indices.data (), sizeof (uint32_t) * snapshot->indices.size ());
    if (ok) {
        g_uploadedIndices = uint32_t (snapshot->indices.size ());
        g_stats.vertices = uint32_t (snapshot->positions.size () / 3);
        g_stats.indices = g_uploadedIndices;
        g_stats.triangles = g_uploadedIndices / 3;
        ++g_stats.uploads;
        // ⚠️ THIS SAYS THE GPU HAS IT, NOT THAT THE MODEL ARRIVED. See the
        // header: `haveSnapshot` belongs to the producer and is set at
        // `EndBatch`, because this line cannot run until injection is active.
        g_stats.uploaded = true;
    }
    delete snapshot;
    return ok && g_uploadedIndices > 0;
}

} // namespace

void BeginBatch (bool full)
{
    if (g_building == nullptr)
        g_building = new Snapshot ();
    if (full) {
        g_building->positions.clear ();
        g_building->indices.clear ();
    }
    ++g_stats.extractionGeneration;
    ++g_stats.batchBegins;
    g_batchElements = 0;
    g_batchOpaqueIndices = 0;
    g_stats.pendingVertices = uint32_t (g_building->positions.size () / 3);
    g_stats.publishFailureReason[0] = 0;
}

uint32_t AddVertices (const float* xyz, uint32_t vertexCount)
{
    if (xyz == nullptr || vertexCount == 0)
        return kNoBase;
    ++g_stats.elementsReceived;
    ++g_batchElements;
    if (g_building == nullptr)
        g_building = new Snapshot ();

    const uint32_t base = uint32_t (g_building->positions.size () / 3);
    if (base + vertexCount > kMaxVertices) {
        ++g_stats.droppedOverCapacity;
        return kNoBase;
    }
    g_building->positions.insert (g_building->positions.end (), xyz, xyz + size_t (vertexCount) * 3);
    g_stats.opaqueVerticesAdded += vertexCount;
    g_stats.pendingVertices = uint32_t (g_building->positions.size () / 3);
    return base;
}

void NoteTransparent (uint32_t indexCount)
{
    g_stats.transparentTrianglesSkipped += indexCount / 3;
}

void AddOpaqueIndices (uint32_t vertexBase, const uint32_t* indices, uint32_t indexCount)
{
    if (indices == nullptr || indexCount == 0 || vertexBase == kNoBase || g_building == nullptr)
        return;
    g_stats.opaqueTriangles += indexCount / 3;
    if (g_building->indices.size () + indexCount > kMaxIndices) {
        ++g_stats.droppedOverCapacity;
        return;
    }
    g_building->indices.reserve (g_building->indices.size () + indexCount);
    for (uint32_t i = 0; i < indexCount; ++i)
        g_building->indices.push_back (vertexBase + indices[i]);
    g_stats.opaqueIndicesAdded += indexCount;
    g_batchOpaqueIndices += indexCount;
}

void EndBatch ()
{
    ++g_stats.batchEnds;

    // ⚠️ THE INVARIANT, CHECKED WHERE IT CAN STILL NAME THE CONDITION:
    // opaque indices arrived ⇒ a publish is attempted. Every path that does not
    // publish writes down WHY, because "no host geometry" with no reason
    // attached is what sent run fifty-six looking at the opacity classifier
    // while the classifier was working perfectly.
    if (g_building == nullptr) {
        PublishFailed ("EndBatch arrived with no batch open");
        return;
    }
    if (g_building->indices.empty ()) {
        char why[160] = {};
        if (g_batchElements == 0)
            strncpy_s (why, sizeof (why), "the batch carried no elements at all", _TRUNCATE);
        else
            sprintf_s (why, sizeof (why), "%llu elements arrived and every surface classified as transparent",
                       (unsigned long long) g_batchElements);
        PublishFailed (why);
        delete g_building;
        g_building = nullptr;
        return;
    }
    if (g_batchOpaqueIndices == 0) {
        PublishFailed ("indices were held over from a previous batch, and none were added to this one");
    }

    ++g_stats.publishAttempted;
    // ⚠️ ONE EXCHANGE, AND WHAT IT REPLACES IS DESTROYED BY THE PRODUCER. The
    // render thread only ever takes ownership of what it finds; if it never ran,
    // the superseded snapshot is ours to free and freeing it here cannot race
    // with a consumer that has already exchanged it away.
    g_stats.publishedVertices = uint32_t (g_building->positions.size () / 3);
    g_stats.publishedIndices = uint32_t (g_building->indices.size ());
    g_stats.publishedTriangles = g_stats.publishedIndices / 3;
    g_stats.publishedGeneration = g_stats.extractionGeneration;

    Snapshot* const previous = g_published.exchange (g_building, std::memory_order_acq_rel);
    delete previous;
    g_building = nullptr;
    ++g_stats.publishSucceeded;
    g_stats.haveSnapshot = true;
    g_stats.publishFailureReason[0] = 0;
}

void Clear ()
{
    delete g_published.exchange (nullptr, std::memory_order_acq_rel);
    delete g_building;
    g_building = nullptr;
    g_uploadedIndices = 0;
    g_stats.haveSnapshot = false;
    g_stats.uploaded = false;
    g_stats.vertices = 0;
    g_stats.indices = 0;
    g_stats.triangles = 0;
    g_stats.pendingVertices = 0;
    g_stats.publishedVertices = 0;
    g_stats.publishedIndices = 0;
    g_stats.publishedTriangles = 0;
    g_batchElements = 0;
    g_batchOpaqueIndices = 0;
}

ID3D11DepthStencilView* Prepare (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation)
{
    if (context == nullptr || context1 == nullptr)
        return nullptr;
    if (!EnsurePipeline (context))
        return nullptr;
    if (!TakeAndUpload (context)) {
        ++g_stats.skippedNoGeometry;
        return nullptr;
    }
    if (!EnsureDepthTarget (context))
        return nullptr;

    ID3D11Buffer* const viewBuffer = injection::ViewSnapshotBuffer ();
    ID3D11Buffer* const projectionBuffer = injection::ProjectionSnapshotBuffer ();
    if (viewBuffer == nullptr || projectionBuffer == nullptr || !injection::SnapshotValid ()) {
        ++g_stats.skippedNoCamera;
        return nullptr;
    }

    const uint32_t variant =
        (interpretation < camerashader::kDeclarableVariants && g_vsVariant[interpretation] != nullptr) ? interpretation
                                                                                                       : 0;
    if (g_vsVariant[variant] == nullptr)
        return nullptr;

    // ⚠️ CLEARED TO FAR EVERY FRAME. Whatever the building looked like last
    // frame is not evidence about this one, and a stale occluder hides the
    // overlay along the path the camera used to be on.
    context->ClearDepthStencilView (g_depthView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    ID3D11RenderTargetView* const noTarget = nullptr;
    context->OMSetRenderTargets (1, &noTarget, g_depthView);
    context->OMSetDepthStencilState (g_writeDepth, 0);
    context->RSSetState (g_raster);
    const FLOAT factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState (g_noColour, factor, 0xffffffffu);

    const UINT stride = sizeof (float) * 3;
    const UINT offset = 0;
    context->IASetInputLayout (g_layout);
    context->IASetVertexBuffers (0, 1, &g_vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer (g_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    context->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader (g_vsVariant[variant], nullptr, 0);
    // ⚠️ NO PIXEL SHADER. Depth-only rendering is exactly this: D3D11 runs the
    // depth test and write with no pixel stage at all, which is both correct and
    // the cheapest way to draw a building.
    context->PSSetShader (nullptr, nullptr, 0);

    ID3D11Buffer* const cameraBuffers[2] = { viewBuffer, projectionBuffer };
    const UINT cameraFirst[2] = { 0, 0 };
    const UINT cameraNum[2] = { kCameraWindowConstants, kCameraWindowConstants };
    context1->VSSetConstantBuffers1 (1, 2, cameraBuffers, cameraFirst, cameraNum);

    context->DrawIndexed (g_uploadedIndices, 0, 0);
    ++g_stats.renders;
    return g_depthView;
}

Stats GetStats ()
{
    return g_stats;
}

void Shutdown ()
{
    Clear ();
    ReleaseAndNull (g_depthView);
    ReleaseAndNull (g_depthTexture);
    ReleaseAndNull (g_noColour);
    ReleaseAndNull (g_raster);
    ReleaseAndNull (g_writeDepth);
    ReleaseAndNull (g_layout);
    for (uint32_t variant = 0; variant < camerashader::kDeclarableVariants; ++variant)
        ReleaseAndNull (g_vsVariant[variant]);
    ReleaseAndNull (g_indexBuffer);
    ReleaseAndNull (g_vertexBuffer);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_uploadedIndices = 0;
    g_depthDesc = D3D11_TEXTURE2D_DESC {};
    g_stats = Stats {};
    g_batchElements = 0;
    g_batchOpaqueIndices = 0;
}

} // namespace hostocclusion
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
