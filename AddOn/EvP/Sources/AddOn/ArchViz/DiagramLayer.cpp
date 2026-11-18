#include "ArchViz/DiagramLayer.hpp"
#include "ArchViz/DiagramGeometry.hpp"

#include "ArchViz/DiligentNativeInterop.hpp"

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

namespace geomsrv::archviz {
using Diligent::RefCntAutoPtr;

namespace {
// ⚠️ THIS STRUCT AND THE HLSL cbuffer ARE ONE DECLARATION WRITTEN TWICE; every member
// is a whole 16-byte row, so there is no padding to drift.
struct DiagramConstants {
    float viewProj[16];
    float light[4];       // xyz: fixed legibility direction, w: ambient
    float translation[4]; // xyz: the node's display translation, w unused
    float tint[4];        // rgb: highlight colour, a: highlight weight (0 = none)
    float style[4];       // x: white study-model weight (0 = function colours), yzw unused
};

// ⚠️ .wzyx: a 0xRRGGBBAA uint read as normalized UINT8x4 arrives byte-reversed on a
// little-endian machine (GhPreviewLayer's kMeshVS says why at length).
constexpr const char* kVS = R"hlsl(
cbuffer DiagramConstants
{
    float4x4 g_viewProj;
    float4   g_light;
    float4   g_translation;
    float4   g_tint;
    float4   g_style;
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
    psIn.position = mul (g_viewProj, float4 (vsIn.position + g_translation.xyz, 1.0));
    float lambert = abs (dot (normalize (vsIn.normal), g_light.xyz));
    float shade = g_light.w + (1.0 - g_light.w) * lambert;
    float4 rgba = vsIn.color.wzyx;
    float3 colour = lerp (rgba.rgb, float3 (0.93, 0.93, 0.91), g_style.x);
    float3 base = lerp (colour, g_tint.rgb, g_tint.a);
    psIn.color = float4 (base * shade, 1.0);
}
)hlsl";

constexpr const char* kPS = R"hlsl(
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

bool Ensure (Diligent::IRenderDevice* device, RefCntAutoPtr<Diligent::IBuffer>& buffer, size_t& capacity, size_t needed,
             Diligent::BIND_FLAGS bind, const char* name, std::string& error)
{
    if (buffer != nullptr && needed <= capacity)
        return true;
    buffer.Release ();
    capacity = 0;
    const size_t grown = (std::max) (needed + needed / 2, size_t (4096));
    Diligent::BufferDesc desc;
    desc.Name = name;
    desc.Size = Diligent::Uint64 (grown);
    desc.BindFlags = bind;
    PreparePersistentGeometryBuffer (device, desc); // uploaded per publication, drawn every frame
    device->CreateBuffer (desc, nullptr, &buffer);
    if (buffer == nullptr) {
        error = std::string ("Diligent CreateBuffer(") + name + ") failed";
        return false;
    }
    capacity = grown;
    return true;
}
} // namespace

struct DiagramLayer::Impl {
    RefCntAutoPtr<Diligent::IPipelineState> pso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    RefCntAutoPtr<Diligent::IBuffer> constants, vertices, indices;
    size_t vertexCapacity = 0, indexCapacity = 0;
    diagram::DiagramGeometry ranges; // meshes only: which index range is which node
    size_t uploadedBytes = 0, lastDraws = 0;
    uint64_t uploads = 0;
    bool ready = false;
};

DiagramLayer::DiagramLayer () : impl_ (new Impl ())
{
}
DiagramLayer::~DiagramLayer ()
{
    Shutdown ();
}
bool DiagramLayer::IsReady () const
{
    return impl_->ready;
}
uint64_t DiagramLayer::UploadCount () const
{
    return impl_->uploads;
}
size_t DiagramLayer::UploadedBytes () const
{
    return impl_->uploadedBytes;
}
size_t DiagramLayer::DrawCount () const
{
    return impl_->lastDraws;
}

void DiagramLayer::Shutdown ()
{
    if (!impl_)
        return;
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->constants.Release ();
    impl_->vertices.Release ();
    impl_->indices.Release ();
    impl_->vertexCapacity = impl_->indexCapacity = 0;
    impl_->ranges = {};
    impl_->uploadedBytes = impl_->lastDraws = 0;
    impl_->uploads = 0;
    impl_->ready = false;
}

bool DiagramLayer::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                         std::string& error)
{
    if (device == nullptr) {
        error = "DiagramLayer::Init got no render device";
        return false;
    }
    if (impl_->ready)
        return true;
    auto compile = [&] (Diligent::SHADER_TYPE type, const char* name, const char* body,
                        RefCntAutoPtr<Diligent::IShader>& out) {
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = name;
        sci.Desc.ShaderType = type;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = body;
        sci.SourceLength = std::strlen (body);
        device->CreateShader (sci, &out, nullptr);
        if (out == nullptr)
            error = std::string ("Diligent CreateShader(") + name + ") failed";
        return out != nullptr;
    };
    RefCntAutoPtr<Diligent::IShader> vs, ps;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Diagram VS", kVS, vs) ||
        !compile (Diligent::SHADER_TYPE_PIXEL, "Diagram PS", kPS, ps))
        return false;

    Diligent::BufferDesc cbd;
    cbd.Name = "Diagram constants";
    cbd.Size = sizeof (DiagramConstants);
    cbd.Usage = Diligent::USAGE_DYNAMIC; // mapped with DISCARD before every draw (D3D12 pitfall 5)
    cbd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    cbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (cbd, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent CreateBuffer(Diagram constants) failed";
        return false;
    }

    // ⚠️ FIELD FOR FIELD diagram::DiagramVertex: position, normal, rgba.
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },
    };
    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "Diagram PSO";
    auto& gp = pci.GraphicsPipeline;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Shell winding comes from slice chains nobody oriented; a culled face is a hole.
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    // Pulled toward the camera: at t = 0 a storey is coincident with its source slab.
    gp.RasterizerDesc.DepthBias = -32;
    gp.RasterizerDesc.SlopeScaledDepthBias = -1.0f;
    gp.DepthStencilDesc.DepthEnable = Diligent::True;
    gp.DepthStencilDesc.DepthWriteEnable = Diligent::True; // a body occludes itself
    gp.DepthStencilDesc.DepthFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;
    gp.InputLayout.LayoutElements = layout;
    gp.InputLayout.NumElements = _countof (layout);
    gp.BlendDesc.RenderTargets[0].BlendEnable = Diligent::False;
    pci.pVS = vs;
    pci.pPS = ps;
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    device->CreateGraphicsPipelineState (pci, &impl_->pso);
    if (impl_->pso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(Diagram PSO) failed";
        return false;
    }
    if (auto* v = impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "DiagramConstants"))
        v->Set (impl_->constants);
    impl_->pso->CreateShaderResourceBinding (&impl_->srb, true);
    if (impl_->srb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(Diagram PSO) failed";
        return false;
    }
    impl_->ready = true;
    return true;
}

bool DiagramLayer::Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                           const diagram::DiagramGeometry& geometry, std::string& error)
{
    if (!impl_->ready || device == nullptr || context == nullptr) {
        error = "DiagramLayer::Upload before Init or without a device";
        return false;
    }
    impl_->ranges = {};
    impl_->uploadedBytes = 0;
    ++impl_->uploads;
    if (geometry.indices.empty ())
        return true;
    const size_t vertexBytes = geometry.vertices.size () * sizeof (diagram::DiagramVertex);
    const size_t indexBytes = geometry.indices.size () * sizeof (uint32_t);
    if (!Ensure (device, impl_->vertices, impl_->vertexCapacity, vertexBytes, Diligent::BIND_VERTEX_BUFFER,
                 "Diagram vertices", error) ||
        !Ensure (device, impl_->indices, impl_->indexCapacity, indexBytes, Diligent::BIND_INDEX_BUFFER,
                 "Diagram indices", error))
        return false;
    if (!WritePersistentGeometryBuffer (context, impl_->vertices, geometry.vertices.data (), vertexBytes) ||
        !WritePersistentGeometryBuffer (context, impl_->indices, geometry.indices.data (), indexBytes)) {
        error = "writing the diagram geometry failed";
        return false;
    }
    impl_->ranges.meshes = geometry.meshes;
    impl_->uploadedBytes = vertexBytes + indexBytes;
    return true;
}

void DiagramLayer::Draw (Diligent::IDeviceContext* context, const float viewProj[16],
                         const std::vector<diagram::DiagramDraw>& draws, const DrawParams& params)
{
    impl_->lastDraws = 0;
    if (!impl_->ready || context == nullptr || draws.empty () || impl_->ranges.meshes.empty () ||
        impl_->vertices == nullptr || impl_->indices == nullptr)
        return;
    DiagramConstants constants {};
    std::memcpy (constants.viewProj, viewProj, sizeof (constants.viewProj));
    constants.light[0] = 0.32f;
    constants.light[1] = 0.42f;
    constants.light[2] = 0.85f;
    constants.light[3] = (std::min) ((std::max) (params.ambient, 0.0f), 1.0f);
    const uint32_t sel = params.selectedRgba;
    constants.tint[0] = float ((sel >> 24) & 0xFF) / 255.0f;
    constants.tint[1] = float ((sel >> 16) & 0xFF) / 255.0f;
    constants.tint[2] = float ((sel >> 8) & 0xFF) / 255.0f;
    constants.style[0] = (std::min) ((std::max) (params.monochrome, 0.0f), 1.0f);

    Diligent::IBuffer* buffers[1] = { impl_->vertices };
    const Diligent::Uint64 offsets[1] = { 0 };
    context->SetVertexBuffers (0, 1, buffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer (impl_->indices, 0, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetPipelineState (impl_->pso);
    for (const auto& draw : draws) {
        if (draw.mesh >= impl_->ranges.meshes.size ())
            continue; // a draw list of another upload: skip, never read past the ranges
        const auto& mesh = impl_->ranges.meshes[draw.mesh];
        for (int k = 0; k < 3; ++k)
            constants.translation[k] = draw.translation[size_t (k)];
        constants.tint[3] = draw.selected ? 0.55f : 0.0f;
        Diligent::PVoid mapped = nullptr;
        context->MapBuffer (impl_->constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
        if (mapped == nullptr)
            return;
        *static_cast<DiagramConstants*> (mapped) = constants;
        context->UnmapBuffer (impl_->constants, Diligent::MAP_WRITE);
        context->CommitShaderResources (impl_->srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawIndexedAttribs attribs;
        attribs.NumIndices = mesh.indexCount;
        attribs.FirstIndexLocation = mesh.firstIndex;
        attribs.IndexType = Diligent::VT_UINT32;
        attribs.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->DrawIndexed (attribs);
        ++impl_->lastDraws;
    }
}
} // namespace geomsrv::archviz
