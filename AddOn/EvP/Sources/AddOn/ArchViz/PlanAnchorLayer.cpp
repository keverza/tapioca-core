#include "ArchViz/PlanAnchorLayer.hpp"

#include "ArchViz/PlanAnchorRibbon.hpp"

#include <windows.h>
#include <d3d11.h>   // Must precede any Diligent D3D11 interop header (Probe 1a).
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

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

namespace {

// ⚠️ THIS LAYOUT MUST MATCH PlanAnchorVertex, FIELD FOR FIELD. A mismatch does
// not fail: it reads the wrong bytes and draws lines that are confidently in
// the wrong place, which on an instrument for checking register is the single
// worst failure available.
struct PlanAnchorConstants {
    float viewProj[16];
    float expand[4];        // xy = half the line width as NDC, zw unused
    float color[4];
};

// One flat colour, one width, no lighting. The push happens HERE rather than in
// the vertex data because a pixel only has a size in clip space — see the
// header for why a world-width ribbon is the wrong instrument.
constexpr const char* kPlanAnchorVS = R"hlsl(
cbuffer PlanAnchorConstants
{
    float4x4 g_viewProj;
    float4   g_expand;
    float4   g_color;
};

struct VSInput
{
    float3 position   : ATTRIB0;
    float2 acrossDir  : ATTRIB1;
    float2 alongDir   : ATTRIB2;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

void main (in VSInput vsIn, out PSInput psIn)
{
    float4 clipPos = mul (g_viewProj, float4 (vsIn.position, 1.0));

    // The push directions are MODEL-space vectors, so they go through the same
    // matrix the position did -- that is what makes the line follow a rotated
    // plan without anything here knowing the rotation. w = 0: a direction is
    // not a point and must not pick up the translation.
    float2 across = mul (g_viewProj, float4 (vsIn.acrossDir, 0.0, 0.0)).xy;
    float2 along  = mul (g_viewProj, float4 (vsIn.alongDir,  0.0, 0.0)).xy;

    // ⚠️ normalize(0) IS A NaN AND A NaN VERTEX DELETES THE TRIANGLE. A
    // direction can project to zero length -- an edge seen exactly end-on --
    // and leaving it unpushed is right: it contributes no width in a direction
    // the screen does not have.
    float acrossLen = length (across);
    if (acrossLen > 1e-6)
        clipPos.xy += (across / acrossLen) * g_expand.xy * clipPos.w;

    float alongLen = length (along);
    if (alongLen > 1e-6)
        clipPos.xy += (along / alongLen) * g_expand.xy * clipPos.w;

    psIn.position = clipPos;
}
)hlsl";

constexpr const char* kPlanAnchorPS = R"hlsl(
cbuffer PlanAnchorConstants
{
    float4x4 g_viewProj;
    float4   g_expand;
    float4   g_color;
};

struct PSInput
{
    float4 position : SV_POSITION;
};

struct PSOutput
{
    float4 color : SV_TARGET;
};

void main (in PSInput psIn, out PSOutput psOut)
{
    psOut.color = g_color;
}
)hlsl";

}   // namespace

struct PlanAnchorLayer::Impl {
    RefCntAutoPtr<Diligent::IPipelineState> pso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    RefCntAutoPtr<Diligent::IBuffer> constants;
    RefCntAutoPtr<Diligent::IBuffer> vertices;
    size_t vertexCount = 0;
    size_t vertexCapacity = 0;
    bool ready = false;
};

PlanAnchorLayer::PlanAnchorLayer () : impl_ (new Impl ()) {}
PlanAnchorLayer::~PlanAnchorLayer () { Shutdown (); }

bool PlanAnchorLayer::IsReady () const { return impl_ != nullptr && impl_->ready; }
size_t PlanAnchorLayer::VertexCount () const { return impl_ != nullptr ? impl_->vertexCount : 0; }

void PlanAnchorLayer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->vertices.Release ();
    impl_->constants.Release ();
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->vertexCount = 0;
    impl_->vertexCapacity = 0;
    impl_->ready = false;
}

bool PlanAnchorLayer::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                            uint32_t depthBufferFormat, std::string& error)
{
    if (device == nullptr) {
        error = "PlanAnchorLayer::Init got no render device";
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

    RefCntAutoPtr<Diligent::IShader> vs, ps;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Plan anchor VS", kPlanAnchorVS, vs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "Plan anchor PS", kPlanAnchorPS, ps))
        return false;

    Diligent::BufferDesc cbd;
    cbd.Name = "Plan anchor constants";
    cbd.Size = sizeof (PlanAnchorConstants);
    cbd.Usage = Diligent::USAGE_DYNAMIC;
    cbd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    cbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (cbd, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent CreateBuffer(Plan anchor constants) failed";
        return false;
    }

    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement {0, 0, 3, Diligent::VT_FLOAT32, Diligent::False},  // position
        Diligent::LayoutElement {1, 0, 2, Diligent::VT_FLOAT32, Diligent::False},  // across
        Diligent::LayoutElement {2, 0, 2, Diligent::VT_FLOAT32, Diligent::False},  // along
    };

    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "Plan anchor PSO";
    Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // See the header: an anchor must be visible where it falls, including
    // inside the wall it outlines, and a flat ribbon must not vanish when the
    // camera crosses its plane.
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = Diligent::False;
    gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
    gp.InputLayout.LayoutElements = layout;
    gp.InputLayout.NumElements = _countof (layout);

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
    pci.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

    device->CreateGraphicsPipelineState (pci, &impl_->pso);
    if (impl_->pso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(Plan anchor PSO) failed";
        return false;
    }

    if (auto* v = impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX,
                                                       "PlanAnchorConstants"))
        v->Set (impl_->constants);
    if (auto* v = impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL,
                                                       "PlanAnchorConstants"))
        v->Set (impl_->constants);

    impl_->pso->CreateShaderResourceBinding (&impl_->srb, true);
    if (impl_->srb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(Plan anchor) failed";
        return false;
    }

    impl_->ready = true;
    return true;
}

bool PlanAnchorLayer::Upload (Diligent::IRenderDevice* device,
                              Diligent::IDeviceContext* context,
                              const std::vector<PlanAnchorVertex>& vertices,
                              std::string& error)
{
    if (!impl_->ready) {
        error = "PlanAnchorLayer::Upload before Init";
        return false;
    }

    impl_->vertexCount = vertices.size ();
    if (vertices.empty ())
        return true;                 // an empty anchor set is ordinary: nothing selected

    // Grow only. A storey re-read at the same size costs a map, not an
    // allocation, and the anchors are re-read on every plan change.
    if (impl_->vertices == nullptr || vertices.size () > impl_->vertexCapacity) {
        impl_->vertices.Release ();

        // Headroom, so a slowly growing selection does not reallocate per frame.
        const size_t capacity = std::max<size_t> (vertices.size () * 2, 4096);
        Diligent::BufferDesc vbd;
        vbd.Name = "Plan anchor vertices";
        vbd.Size = Diligent::Uint64 (capacity * sizeof (PlanAnchorVertex));
        vbd.Usage = Diligent::USAGE_DYNAMIC;
        vbd.BindFlags = Diligent::BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
        device->CreateBuffer (vbd, nullptr, &impl_->vertices);
        if (impl_->vertices == nullptr) {
            impl_->vertexCapacity = 0;
            impl_->vertexCount = 0;
            error = "Diligent CreateBuffer(Plan anchor vertices) failed";
            return false;
        }
        impl_->vertexCapacity = capacity;
    }

    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (impl_->vertices, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr) {
        impl_->vertexCount = 0;
        error = "Diligent MapBuffer(Plan anchor vertices) failed";
        return false;
    }
    std::memcpy (mapped, vertices.data (), vertices.size () * sizeof (PlanAnchorVertex));
    context->UnmapBuffer (impl_->vertices, Diligent::MAP_WRITE);
    return true;
}

void PlanAnchorLayer::Draw (Diligent::IDeviceContext* context, const float viewProj[16],
                            uint32_t surfaceWidth, uint32_t surfaceHeight,
                            float widthPixels, uint32_t rgba)
{
    if (!impl_->ready || impl_->vertexCount == 0 || impl_->vertices == nullptr)
        return;
    if (surfaceWidth == 0 || surfaceHeight == 0)
        return;

    PlanAnchorConstants constants = {};
    std::memcpy (constants.viewProj, viewProj, sizeof (constants.viewProj));

    // NDC spans 2 across the surface, and the shader pushes each way, so half
    // the width in pixels becomes (half / size) * 2 == width / size.
    constants.expand[0] = widthPixels / float (surfaceWidth);
    constants.expand[1] = widthPixels / float (surfaceHeight);

    constants.color[0] = float ((rgba >> 24) & 0xFF) / 255.0f;
    constants.color[1] = float ((rgba >> 16) & 0xFF) / 255.0f;
    constants.color[2] = float ((rgba >> 8) & 0xFF) / 255.0f;
    constants.color[3] = float (rgba & 0xFF) / 255.0f;

    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (impl_->constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr)
        return;
    *static_cast<PlanAnchorConstants*> (mapped) = constants;
    context->UnmapBuffer (impl_->constants, Diligent::MAP_WRITE);

    Diligent::IBuffer* vertexBuffers[1] = {impl_->vertices};
    const Diligent::Uint64 offsets[1] = {0};
    context->SetVertexBuffers (0, 1, vertexBuffers, offsets,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetPipelineState (impl_->pso);
    context->CommitShaderResources (impl_->srb,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Diligent::DrawAttribs draw;
    draw.NumVertices = Diligent::Uint32 (impl_->vertexCount);
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->Draw (draw);
}

}   // namespace archviz
}   // namespace geomsrv
