#include "ArchViz/GhPreviewLayer.hpp"

#include "ArchViz/GhPreviewGeometry.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).
#include <Buffer.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <InputLayout.h>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

namespace {

// ⚠️ THIS STRUCT AND THE cbuffer IN BOTH SHADERS ARE ONE DECLARATION WRITTEN
// TWICE. HLSL packs into 16-byte rows; every member here is already a multiple of
// one, which is why there is no padding and must not silently become some.
struct GhPreviewConstants {
    float viewProj[16];
    // xy = 2 / surface size, the pixels-to-NDC factor. zw = the surface size, so
    // the shader can go the other way without a divide.
    float pixelToNdc[4];
    // x = half the drawn line width in pixels.
    // y = half the WIDENED width: x + 1, which is the quad actually built.
    float params[4];
    // xyz = the world direction the legibility light comes from, w = ambient.
    float light[4];
};

// ⚠️ A LEGIBILITY LIGHT, NOT A SUN, AND DELIBERATELY FIXED IN WORLD SPACE. The
// scene's own sun moves, casts and is shadowed; a preview shaded by it would go
// black whenever the user swung the sun round, and a Grasshopper result that
// disappears at 6pm is not an instrument. abs() makes it two-sided, because a
// preview mesh is very often a single unclosed surface and its back is as much
// of the answer as its front.
constexpr const char* kMeshVS = R"hlsl(
cbuffer GhPreviewConstants
{
    float4x4 g_viewProj;
    float4   g_pixelToNdc;
    float4   g_params;
    float4   g_light;
};

struct VSInput
{
    float3 position : ATTRIB0;
    float3 normal   : ATTRIB1;
    float4 color    : ATTRIB2;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
};

void main (in VSInput vsIn, out PSInput psIn)
{
    psIn.position = mul (g_viewProj, float4 (vsIn.position, 1.0));

    float nlen = length (vsIn.normal);
    // A zero normal would make the shading term undefined. Fully lit is the
    // honest answer: the geometry is there, and only its shading is unknown.
    float lambert = 1.0;
    if (nlen > 1e-6)
        lambert = abs (dot (vsIn.normal / nlen, g_light.xyz));

    float shade = g_light.w + (1.0 - g_light.w) * lambert;

    // ⚠️ .wzyx, AND THIS IS NOT A TYPO. The vertex carries one uint32 written as
    // 0xRRGGBBAA, which on a little-endian machine reaches the input assembler as
    // the bytes AA, BB, GG, RR -- so a normalized UINT8x4 attribute arrives
    // reversed. Getting this wrong does not fail; it swaps red and alpha, and a
    // preview that is invisible because its alpha came from the red channel looks
    // exactly like a preview that was never sent.
    float4 rgba = vsIn.color.wzyx;
    psIn.color = float4 (rgba.rgb * shade, rgba.a);
}
)hlsl";

constexpr const char* kMeshPS = R"hlsl(
struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
};

struct PSOutput
{
    float4 color : SV_TARGET;
};

void main (in PSInput psIn, out PSOutput psOut)
{
    psOut.color = psIn.color;
}
)hlsl";

// The screen-width ribbon. Everything view-dependent happens here, at the camera
// the viewport actually has -- see the header on why the layer holds no camera of
// its own.
constexpr const char* kLineVS = R"hlsl(
cbuffer GhPreviewConstants
{
    float4x4 g_viewProj;
    float4   g_pixelToNdc;
    float4   g_params;
    float4   g_light;
};

struct VSInput
{
    float3 position : ATTRIB0;
    float3 other    : ATTRIB1;
    float  side     : ATTRIB2;
    float4 color    : ATTRIB3;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
    float  side     : TEXCOORD0;
};

void main (in VSInput vsIn, out PSInput psIn)
{
    float4 clip      = mul (g_viewProj, float4 (vsIn.position, 1.0));
    float4 clipOther = mul (g_viewProj, float4 (vsIn.other,    1.0));

    // ⚠️ THE DIRECTION IS MEASURED AFTER THE PERSPECTIVE DIVIDE, IN PIXELS. A
    // direction transformed as a vector (w = 0) is the world-space tangent
    // projected, which is NOT the direction the line runs on screen once
    // perspective is applied -- the two agree only for an orthographic camera,
    // and the disagreement is worst exactly where a preview is most often
    // inspected: close up.
    float2 ndc      = clip.xy      / max (abs (clip.w),      1e-6);
    float2 ndcOther = clipOther.xy / max (abs (clipOther.w), 1e-6);
    float2 dirPixels = (ndcOther - ndc) * g_pixelToNdc.zw * 0.5;

    float len = length (dirPixels);
    // Seen exactly end-on the segment has no screen direction. An arbitrary one
    // is better than a NaN: normalize(0) is a NaN, and a NaN vertex deletes the
    // whole triangle rather than degrading it.
    float2 alongPixels = len > 1e-6 ? dirPixels / len : float2 (1.0, 0.0);
    float2 acrossPixels = float2 (-alongPixels.y, alongPixels.x);

    // ⚠️ NOT NAMED `half`. That is a RESERVED TYPE in HLSL, and a local of that
    // name is a shader-compile error -- which this build would not catch, because
    // these shaders are compiled at RUNTIME. The whole layer would simply report
    // "unavailable" in archviz.log and draw nothing, on every machine.
    float halfPixels = g_params.y;

    // Across for the width; against `along` for the square cap. The cap needs no
    // flag: `other` is always the far end, so `along` always points inward and
    // the outward push is always its negation.
    float2 offsetPixels = acrossPixels * vsIn.side * halfPixels - alongPixels * halfPixels;

    clip.xy += offsetPixels * g_pixelToNdc.xy * clip.w;

    psIn.position = clip;
    psIn.color = vsIn.color.wzyx;
    psIn.side = vsIn.side;
}
)hlsl";

// ⚠️ THE ANTIALIASING IS ANALYTIC BECAUSE THIS TARGET HAS NO MSAA. The quad is
// built one pixel wider each way and the coverage ramps over that pixel; without
// it a near-horizontal preview curve staircases, and on a Grasshopper result that
// reads as bad geometry from the definition rather than as a missing multisample.
constexpr const char* kLinePS = R"hlsl(
cbuffer GhPreviewConstants
{
    float4x4 g_viewProj;
    float4   g_pixelToNdc;
    float4   g_params;
    float4   g_light;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
    float  side     : TEXCOORD0;
};

struct PSOutput
{
    float4 color : SV_TARGET;
};

void main (in PSInput psIn, out PSOutput psOut)
{
    // `side` was +/-1 at the widened quad's edges, so scaling it back by the
    // widened half-width gives this pixel's distance from the centreline.
    float distPixels = abs (psIn.side) * g_params.y;
    float coverage = saturate (g_params.x + 0.5 - distPixels);

    psOut.color = float4 (psIn.color.rgb, psIn.color.a * coverage);
}
)hlsl";

struct Bucket {
    RefCntAutoPtr<Diligent::IBuffer> meshVertices;
    RefCntAutoPtr<Diligent::IBuffer> meshIndices;
    RefCntAutoPtr<Diligent::IBuffer> lineVertices;
    size_t meshVertexCapacity = 0;
    size_t meshIndexCapacity = 0;
    size_t lineVertexCapacity = 0;
    size_t meshIndexCount = 0;
    size_t lineVertexCount = 0;

    void Release ()
    {
        meshVertices.Release ();
        meshIndices.Release ();
        lineVertices.Release ();
        meshVertexCapacity = 0;
        meshIndexCapacity = 0;
        lineVertexCapacity = 0;
        meshIndexCount = 0;
        lineVertexCount = 0;
    }
};

// Grow-only, so a slider drag that changes a mesh without changing its size
// costs a map rather than an allocation. That is the commonest case on this path
// by a wide margin.
template <typename T>
bool EnsureCapacity (Diligent::IRenderDevice* device, RefCntAutoPtr<Diligent::IBuffer>& buffer, size_t& capacity,
                     size_t needed, Diligent::BIND_FLAGS bind, const char* name, std::string& error)
{
    if (buffer != nullptr && needed <= capacity)
        return true;

    buffer.Release ();
    const size_t grown = (std::max) (needed * 2, static_cast<size_t> (4096));
    Diligent::BufferDesc desc;
    desc.Name = name;
    desc.Size = Diligent::Uint64 (grown * sizeof (T));
    desc.Usage = Diligent::USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (desc, nullptr, &buffer);
    if (buffer == nullptr) {
        capacity = 0;
        error = std::string ("Diligent CreateBuffer(") + name + ") failed";
        return false;
    }
    capacity = grown;
    return true;
}

template <typename T>
bool Fill (Diligent::IDeviceContext* context, Diligent::IBuffer* buffer, const std::vector<T>& data, const char* name,
           std::string& error)
{
    if (data.empty ())
        return true;
    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr) {
        error = std::string ("Diligent MapBuffer(") + name + ") failed";
        return false;
    }
    std::memcpy (mapped, data.data (), data.size () * sizeof (T));
    context->UnmapBuffer (buffer, Diligent::MAP_WRITE);
    return true;
}

} // namespace

struct GhPreviewLayer::Impl {
    RefCntAutoPtr<Diligent::IPipelineState> meshPso;
    RefCntAutoPtr<Diligent::IPipelineState> meshXRayPso;
    RefCntAutoPtr<Diligent::IPipelineState> linePso;
    RefCntAutoPtr<Diligent::IPipelineState> lineXRayPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> meshSrb;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> meshXRaySrb;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> lineSrb;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> lineXRaySrb;
    RefCntAutoPtr<Diligent::IBuffer> constants;
    Bucket depthTested;
    Bucket xray;
    size_t labelCount = 0;
    bool ready = false;
};

GhPreviewLayer::GhPreviewLayer () : impl_ (new Impl ())
{
}

GhPreviewLayer::~GhPreviewLayer ()
{
    Shutdown ();
}

bool GhPreviewLayer::IsReady () const
{
    return impl_ != nullptr && impl_->ready;
}

size_t GhPreviewLayer::MeshIndexCount () const
{
    return impl_ == nullptr ? 0 : impl_->depthTested.meshIndexCount + impl_->xray.meshIndexCount;
}

size_t GhPreviewLayer::LineVertexCount () const
{
    return impl_ == nullptr ? 0 : impl_->depthTested.lineVertexCount + impl_->xray.lineVertexCount;
}

size_t GhPreviewLayer::LabelCount () const
{
    return impl_ == nullptr ? 0 : impl_->labelCount;
}

void GhPreviewLayer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->depthTested.Release ();
    impl_->xray.Release ();
    impl_->constants.Release ();
    impl_->meshSrb.Release ();
    impl_->meshXRaySrb.Release ();
    impl_->lineSrb.Release ();
    impl_->lineXRaySrb.Release ();
    impl_->meshPso.Release ();
    impl_->meshXRayPso.Release ();
    impl_->linePso.Release ();
    impl_->lineXRayPso.Release ();
    impl_->labelCount = 0;
    impl_->ready = false;
}

bool GhPreviewLayer::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                           std::string& error)
{
    if (device == nullptr) {
        error = "GhPreviewLayer::Init got no render device";
        return false;
    }
    if (impl_->ready)
        return true;

    auto compile = [&] (Diligent::SHADER_TYPE type, const char* name, const char* body,
                        RefCntAutoPtr<Diligent::IShader>& out) -> bool {
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = name;
        sci.Desc.ShaderType = type;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = body;
        sci.SourceLength = std::strlen (body);
        device->CreateShader (sci, &out, nullptr);
        if (out == nullptr) {
            error = std::string ("Diligent CreateShader(") + name +
                    ") failed -- the HLSL compiler's own message is in the debug output";
            return false;
        }
        return true;
    };

    RefCntAutoPtr<Diligent::IShader> meshVs, meshPs, lineVs, linePs;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Gh preview mesh VS", kMeshVS, meshVs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "Gh preview mesh PS", kMeshPS, meshPs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Gh preview line VS", kLineVS, lineVs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "Gh preview line PS", kLinePS, linePs))
        return false;

    Diligent::BufferDesc cbd;
    cbd.Name = "Gh preview constants";
    cbd.Size = sizeof (GhPreviewConstants);
    cbd.Usage = Diligent::USAGE_DYNAMIC;
    cbd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    cbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (cbd, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent CreateBuffer(Gh preview constants) failed";
        return false;
    }

    // ⚠️ THESE LAYOUTS MUST MATCH GhPreviewMeshVertex AND GhPreviewLineVertex,
    // FIELD FOR FIELD. A mismatch does not fail: it reads the wrong bytes and
    // draws confidently wrong geometry, which on an instrument is the worst
    // failure available.
    const Diligent::LayoutElement meshLayout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // normal
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },    // rgba, normalized
    };
    const Diligent::LayoutElement lineLayout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // other end
        Diligent::LayoutElement { 2, 0, 1, Diligent::VT_FLOAT32, Diligent::False }, // side
        Diligent::LayoutElement { 3, 0, 4, Diligent::VT_UINT8, Diligent::True },    // rgba, normalized
    };

    auto buildPso = [&] (const char* name, Diligent::IShader* vs, Diligent::IShader* ps,
                         const Diligent::LayoutElement* layout, Diligent::Uint32 layoutCount, bool depthTest,
                         RefCntAutoPtr<Diligent::IPipelineState>& pso,
                         RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb) -> bool {
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = name;
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // ⚠️ NO CULLING. A preview mesh is very often a single unclosed surface
        // whose winding nobody controlled, and a culled back face is not a
        // subtler picture -- it is half the result missing.
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        gp.DepthStencilDesc.DepthEnable = depthTest ? Diligent::True : Diligent::False;
        // ⚠️ NEVER WRITES DEPTH, IN EITHER VARIANT. Preview is an annotation on
        // the building; letting it occlude the geometry it annotates would hide
        // the very wall being measured, and would make the ribbon fight itself
        // where segments overlap at a corner.
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
        gp.DepthStencilDesc.DepthFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;
        gp.InputLayout.LayoutElements = layout;
        gp.InputLayout.NumElements = layoutCount;

        Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
        rt.BlendEnable = Diligent::True;
        rt.SrcBlend = Diligent::BLEND_FACTOR_SRC_ALPHA;
        rt.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
        rt.BlendOp = Diligent::BLEND_OPERATION_ADD;
        rt.SrcBlendAlpha = Diligent::BLEND_FACTOR_ONE;
        rt.DestBlendAlpha = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
        rt.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;

        pci.pVS = vs;
        pci.pPS = ps;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

        device->CreateGraphicsPipelineState (pci, &pso);
        if (pso == nullptr) {
            error = std::string ("Diligent CreateGraphicsPipelineState(") + name + ") failed";
            return false;
        }
        if (auto* v = pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "GhPreviewConstants"))
            v->Set (impl_->constants);
        if (auto* v = pso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "GhPreviewConstants"))
            v->Set (impl_->constants);
        pso->CreateShaderResourceBinding (&srb, true);
        if (srb == nullptr) {
            error = std::string ("Diligent CreateShaderResourceBinding(") + name + ") failed";
            return false;
        }
        return true;
    };

    if (!buildPso ("Gh preview mesh PSO", meshVs, meshPs, meshLayout, _countof (meshLayout), true, impl_->meshPso,
                   impl_->meshSrb))
        return false;
    if (!buildPso ("Gh preview mesh x-ray PSO", meshVs, meshPs, meshLayout, _countof (meshLayout), false,
                   impl_->meshXRayPso, impl_->meshXRaySrb))
        return false;
    if (!buildPso ("Gh preview line PSO", lineVs, linePs, lineLayout, _countof (lineLayout), true, impl_->linePso,
                   impl_->lineSrb))
        return false;
    if (!buildPso ("Gh preview line x-ray PSO", lineVs, linePs, lineLayout, _countof (lineLayout), false,
                   impl_->lineXRayPso, impl_->lineXRaySrb))
        return false;

    impl_->ready = true;
    return true;
}

bool GhPreviewLayer::Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                             const GhPreviewDrawables& drawables, std::string& error)
{
    if (!impl_->ready) {
        error = "GhPreviewLayer::Upload before Init";
        return false;
    }
    if (device == nullptr || context == nullptr) {
        error = "GhPreviewLayer::Upload got no device or context";
        return false;
    }

    impl_->labelCount = drawables.LabelCount ();

    auto upload = [&] (const GhPreviewBucket& source, Bucket& target, const char* which) -> bool {
        target.meshIndexCount = source.meshIndices.size ();
        target.lineVertexCount = source.lineVertices.size ();

        if (!source.meshIndices.empty ()) {
            if (!EnsureCapacity<GhPreviewMeshVertex> (device, target.meshVertices, target.meshVertexCapacity,
                                                      source.meshVertices.size (), Diligent::BIND_VERTEX_BUFFER,
                                                      "Gh preview mesh vertices", error) ||
                !EnsureCapacity<uint32_t> (device, target.meshIndices, target.meshIndexCapacity,
                                           source.meshIndices.size (), Diligent::BIND_INDEX_BUFFER,
                                           "Gh preview mesh indices", error)) {
                target.meshIndexCount = 0;
                return false;
            }
            if (!Fill (context, target.meshVertices, source.meshVertices, "Gh preview mesh vertices", error) ||
                !Fill (context, target.meshIndices, source.meshIndices, "Gh preview mesh indices", error)) {
                target.meshIndexCount = 0;
                return false;
            }
        }

        if (!source.lineVertices.empty ()) {
            if (!EnsureCapacity<GhPreviewLineVertex> (device, target.lineVertices, target.lineVertexCapacity,
                                                      source.lineVertices.size (), Diligent::BIND_VERTEX_BUFFER,
                                                      "Gh preview line vertices", error)) {
                target.lineVertexCount = 0;
                return false;
            }
            if (!Fill (context, target.lineVertices, source.lineVertices, "Gh preview line vertices", error)) {
                target.lineVertexCount = 0;
                return false;
            }
        }

        (void) which;
        return true;
    };

    // Both buckets are attempted even if the first fails, so one bad upload does
    // not leave the other bucket describing a buffer it no longer matches.
    std::string firstError;
    if (!upload (drawables.depthTested, impl_->depthTested, "depth-tested"))
        firstError = error;
    if (!upload (drawables.xray, impl_->xray, "x-ray") && firstError.empty ())
        firstError = error;

    if (!firstError.empty ()) {
        error = firstError;
        return false;
    }
    return true;
}

void GhPreviewLayer::Draw (Diligent::IDeviceContext* context, const float viewProj[16], uint32_t surfaceWidth,
                           uint32_t surfaceHeight, const DrawParams& params)
{
    if (!impl_->ready || context == nullptr)
        return;
    if (surfaceWidth == 0 || surfaceHeight == 0)
        return;
    if (MeshIndexCount () == 0 && LineVertexCount () == 0)
        return;

    GhPreviewConstants constants = {};
    std::memcpy (constants.viewProj, viewProj, sizeof (constants.viewProj));
    constants.pixelToNdc[0] = 2.0f / float (surfaceWidth);
    constants.pixelToNdc[1] = 2.0f / float (surfaceHeight);
    constants.pixelToNdc[2] = float (surfaceWidth);
    constants.pixelToNdc[3] = float (surfaceHeight);

    const float halfWidth = (std::max) (params.lineWidthPixels, 0.5f) * 0.5f;
    constants.params[0] = halfWidth;
    // The quad is built one pixel wider each way; the pixel shader ramps coverage
    // over that pixel. See kLinePS.
    constants.params[1] = halfWidth + 1.0f;

    // A fixed, normalised direction. See kMeshVS: this is legibility, not a sun.
    constants.light[0] = 0.32f;
    constants.light[1] = 0.42f;
    constants.light[2] = 0.85f;
    constants.light[3] = (std::min) ((std::max) (params.ambient, 0.0f), 1.0f);

    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (impl_->constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr)
        return;
    *static_cast<GhPreviewConstants*> (mapped) = constants;
    context->UnmapBuffer (impl_->constants, Diligent::MAP_WRITE);

    auto drawBucket = [&] (Bucket& bucket, Diligent::IPipelineState* meshPso, Diligent::IShaderResourceBinding* meshSrb,
                           Diligent::IPipelineState* linePso, Diligent::IShaderResourceBinding* lineSrb) {
        if (bucket.meshIndexCount > 0 && bucket.meshVertices != nullptr && bucket.meshIndices != nullptr) {
            Diligent::IBuffer* buffers[1] = { bucket.meshVertices };
            const Diligent::Uint64 offsets[1] = { 0 };
            context->SetVertexBuffers (0, 1, buffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                       Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
            context->SetIndexBuffer (bucket.meshIndices, 0, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->SetPipelineState (meshPso);
            context->CommitShaderResources (meshSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            Diligent::DrawIndexedAttribs draw;
            draw.NumIndices = Diligent::Uint32 (bucket.meshIndexCount);
            draw.IndexType = Diligent::VT_UINT32;
            draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
            context->DrawIndexed (draw);
        }

        if (bucket.lineVertexCount > 0 && bucket.lineVertices != nullptr) {
            Diligent::IBuffer* buffers[1] = { bucket.lineVertices };
            const Diligent::Uint64 offsets[1] = { 0 };
            context->SetVertexBuffers (0, 1, buffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                       Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
            context->SetPipelineState (linePso);
            context->CommitShaderResources (lineSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            Diligent::DrawAttribs draw;
            draw.NumVertices = Diligent::Uint32 (bucket.lineVertexCount);
            draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
            context->Draw (draw);
        }
    };

    // ⚠️ DEPTH-TESTED FIRST, X-RAY SECOND. The x-ray pass is the one that must
    // end up on top: it exists to be seen through the building, and drawing it
    // before the depth-tested geometry would let an ordinary preview mesh blend
    // over the very thing the x-ray flag was set to reveal.
    drawBucket (impl_->depthTested, impl_->meshPso, impl_->meshSrb, impl_->linePso, impl_->lineSrb);
    drawBucket (impl_->xray, impl_->meshXRayPso, impl_->meshXRaySrb, impl_->lineXRayPso, impl_->lineXRaySrb);
}

} // namespace archviz
} // namespace geomsrv
