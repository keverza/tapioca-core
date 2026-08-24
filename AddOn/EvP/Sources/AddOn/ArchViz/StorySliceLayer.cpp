#include "ArchViz/StorySliceLayer.hpp"

#include "ArchViz/StorySliceGeometry.hpp"

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

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

namespace {

// ⚠️ THIS LAYOUT MUST MATCH StorySliceVertex, FIELD FOR FIELD. A mismatch does
// not fail: it reads the wrong bytes and draws lines that are confidently in the
// wrong place — which on a section contour is indistinguishable from a genuinely
// odd building.
struct StorySliceConstants {
    float viewProj[16];
    float expand[4]; // xy = the line width as NDC, zw unused
    float color[4];  // the outline colour
    float fillColor[4];
    float dash[4]; // x = period in pixels, y = duty cycle, z = on/off, w unused
};

// The outline. One flat colour, one width, no lighting. The push happens HERE
// rather than in the vertex data because a pixel only has a size in clip space.
constexpr const char* kStorySliceVS = R"hlsl(
cbuffer StorySliceConstants
{
    float4x4 g_viewProj;
    float4   g_expand;
    float4   g_color;
    float4   g_fillColor;
    float4   g_dash;
};

struct VSInput
{
    float3 position  : ATTRIB0;
    float2 acrossDir : ATTRIB1;
    float2 alongDir  : ATTRIB2;
    float  arc       : ATTRIB3;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float  arc      : TEXCOORD0;
};

void main (in VSInput vsIn, out PSInput psIn)
{
    float4 clipPos = mul (g_viewProj, float4 (vsIn.position, 1.0));

    // The push directions are MODEL-space vectors, so they go through the same
    // matrix the position did -- that is what makes the contour follow a rotated
    // building without anything here knowing the rotation. w = 0: a direction is
    // not a point and must not pick up the translation.
    float2 across = mul (g_viewProj, float4 (vsIn.acrossDir, 0.0, 0.0)).xy;
    float2 along  = mul (g_viewProj, float4 (vsIn.alongDir,  0.0, 0.0)).xy;

    // ⚠️ normalize(0) IS A NaN AND A NaN VERTEX DELETES THE TRIANGLE. A direction
    // can project to zero length -- an edge seen exactly end-on -- and leaving it
    // unpushed is right: it contributes no width in a direction the screen does
    // not have.
    float acrossLen = length (across);
    if (acrossLen > 1e-6)
        clipPos.xy += (across / acrossLen) * g_expand.xy * clipPos.w;

    float alongLen = length (along);
    if (alongLen > 1e-6)
        clipPos.xy += (along / alongLen) * g_expand.xy * clipPos.w;

    psIn.position = clipPos;
    psIn.arc      = vsIn.arc;
}
)hlsl";

constexpr const char* kStorySlicePS = R"hlsl(
cbuffer StorySliceConstants
{
    float4x4 g_viewProj;
    float4   g_expand;
    float4   g_color;
    float4   g_fillColor;
    float4   g_dash;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float  arc      : TEXCOORD0;
};

struct PSOutput
{
    float4 color : SV_TARGET;
};

void main (in PSInput psIn, out PSOutput psOut)
{
    if (g_dash.z > 0.5)
    {
        // ⚠️ THE DASH PERIOD IS CONVERTED TO SCREEN SPACE HERE, and fwidth is
        // what does it. `arc` is metres along the contour, so fwidth(arc) is
        // metres-per-pixel at this fragment -- and arc / fwidth(arc) is therefore
        // the position along the contour measured in PIXELS. That is the whole
        // reason the dash stays the same size at every zoom, which is what makes
        // it read as "this part is hidden" rather than as a texture.
        float metresPerPixel = max (fwidth (psIn.arc), 1e-9);
        float pixels         = psIn.arc / metresPerPixel;
        float phase          = frac (pixels / max (g_dash.x, 1.0));
        if (phase > g_dash.y)
            discard;
    }
    psOut.color = g_color;
}
)hlsl";

// The fill. Position only: the colour is one constant for the whole layer, so
// there is nothing per-vertex that could disagree with anything.
constexpr const char* kStorySliceFillVS = R"hlsl(
cbuffer StorySliceConstants
{
    float4x4 g_viewProj;
    float4   g_expand;
    float4   g_color;
    float4   g_fillColor;
    float4   g_dash;
};

struct VSInput  { float3 position : ATTRIB0; };
struct PSInput  { float4 position : SV_POSITION; };

void main (in VSInput vsIn, out PSInput psIn)
{
    psIn.position = mul (g_viewProj, float4 (vsIn.position, 1.0));
}
)hlsl";

constexpr const char* kStorySliceFillPS = R"hlsl(
cbuffer StorySliceConstants
{
    float4x4 g_viewProj;
    float4   g_expand;
    float4   g_color;
    float4   g_fillColor;
    float4   g_dash;
};

struct PSInput  { float4 position : SV_POSITION; };
struct PSOutput { float4 color : SV_TARGET; };

void main (in PSInput psIn, out PSOutput psOut)
{
    psOut.color = g_fillColor;
}
)hlsl";

void UnpackRgba (uint32_t rgba, float out[4])
{
    out[0] = float ((rgba >> 24) & 0xFF) / 255.0f;
    out[1] = float ((rgba >> 16) & 0xFF) / 255.0f;
    out[2] = float ((rgba >> 8) & 0xFF) / 255.0f;
    out[3] = float (rgba & 0xFF) / 255.0f;
}

} // namespace

struct StorySliceLayer::Impl {
    // Two PSOs per drawable, differing ONLY in the depth comparison. A PSO is
    // immutable in Diligent, so "same pipeline, other depth func" is a different
    // object rather than a state change -- the stateless-design difference from
    // bgfx::setState that DiligentSceneImpl.hpp already calls out.
    RefCntAutoPtr<Diligent::IPipelineState> outlineVisiblePso;
    RefCntAutoPtr<Diligent::IPipelineState> outlineOccludedPso;
    RefCntAutoPtr<Diligent::IPipelineState> fillVisiblePso;
    RefCntAutoPtr<Diligent::IPipelineState> fillOccludedPso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> outlineVisibleSrb;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> outlineOccludedSrb;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> fillVisibleSrb;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> fillOccludedSrb;
    RefCntAutoPtr<Diligent::IBuffer> constants;
    RefCntAutoPtr<Diligent::IBuffer> outlineVertices;
    RefCntAutoPtr<Diligent::IBuffer> fillVertices;

    size_t outlineCount = 0;
    size_t outlineCapacity = 0;
    size_t fillCount = 0;
    size_t fillCapacity = 0;
    bool ready = false;
};

StorySliceLayer::StorySliceLayer () : impl_ (new Impl ())
{
}
StorySliceLayer::~StorySliceLayer ()
{
    Shutdown ();
}

bool StorySliceLayer::IsReady () const
{
    return impl_ != nullptr && impl_->ready;
}
size_t StorySliceLayer::OutlineVertexCount () const
{
    return impl_ != nullptr ? impl_->outlineCount : 0;
}
size_t StorySliceLayer::FillVertexCount () const
{
    return impl_ != nullptr ? impl_->fillCount : 0;
}

void StorySliceLayer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->outlineVertices.Release ();
    impl_->fillVertices.Release ();
    impl_->constants.Release ();
    impl_->outlineVisibleSrb.Release ();
    impl_->outlineOccludedSrb.Release ();
    impl_->fillVisibleSrb.Release ();
    impl_->fillOccludedSrb.Release ();
    impl_->outlineVisiblePso.Release ();
    impl_->outlineOccludedPso.Release ();
    impl_->fillVisiblePso.Release ();
    impl_->fillOccludedPso.Release ();
    impl_->outlineCount = 0;
    impl_->outlineCapacity = 0;
    impl_->fillCount = 0;
    impl_->fillCapacity = 0;
    impl_->ready = false;
}

bool StorySliceLayer::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                            std::string& error)
{
    if (device == nullptr) {
        error = "StorySliceLayer::Init got no render device";
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

    RefCntAutoPtr<Diligent::IShader> vs, ps, fillVs, fillPs;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Story slice VS", kStorySliceVS, vs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "Story slice PS", kStorySlicePS, ps))
        return false;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Story slice fill VS", kStorySliceFillVS, fillVs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "Story slice fill PS", kStorySliceFillPS, fillPs))
        return false;

    Diligent::BufferDesc cbd;
    cbd.Name = "Story slice constants";
    cbd.Size = sizeof (StorySliceConstants);
    cbd.Usage = Diligent::USAGE_DYNAMIC;
    cbd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    cbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (cbd, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent CreateBuffer(Story slice constants) failed";
        return false;
    }

    const Diligent::LayoutElement outlineLayout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
        Diligent::LayoutElement { 1, 0, 2, Diligent::VT_FLOAT32, Diligent::False }, // across
        Diligent::LayoutElement { 2, 0, 2, Diligent::VT_FLOAT32, Diligent::False }, // along
        Diligent::LayoutElement { 3, 0, 1, Diligent::VT_FLOAT32, Diligent::False }, // arc
    };
    const Diligent::LayoutElement fillLayout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
    };

    // One builder for all four, so the only thing that can differ between them is
    // what is passed in. Two hand-written copies of a PSO description differing
    // in one field is precisely how the visible and occluded passes would drift
    // into disagreeing about, say, blending.
    auto buildPso = [&] (const char* name, Diligent::IShader* vsIn, Diligent::IShader* psIn,
                         const Diligent::LayoutElement* layout, Diligent::Uint32 layoutCount,
                         Diligent::COMPARISON_FUNCTION depthFunc, RefCntAutoPtr<Diligent::IPipelineState>& psoOut,
                         RefCntAutoPtr<Diligent::IShaderResourceBinding>& srbOut) -> bool {
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = name;
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // ⚠️ NO CULLING. A flat horizontal ribbon and a flat horizontal fill both
        // vanish the moment the camera crosses their plane if either is culled --
        // and on a storey contour that is one scroll away, every time.
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        // ⚠️ TESTS DEPTH, NEVER WRITES IT. See the header: the two passes split
        // the contour into its visible and hidden halves, and a contour that wrote
        // depth would occlude the wall it is annotating and fight the other pass.
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
        gp.DepthStencilDesc.DepthFunc = depthFunc;
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

        pci.pVS = vsIn;
        pci.pPS = psIn;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

        device->CreateGraphicsPipelineState (pci, &psoOut);
        if (psoOut == nullptr) {
            error = std::string ("Diligent CreateGraphicsPipelineState(") + name + ") failed";
            return false;
        }
        if (auto* v = psoOut->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "StorySliceConstants"))
            v->Set (impl_->constants);
        if (auto* v = psoOut->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "StorySliceConstants"))
            v->Set (impl_->constants);
        psoOut->CreateShaderResourceBinding (&srbOut, true);
        if (srbOut == nullptr) {
            error = std::string ("Diligent CreateShaderResourceBinding(") + name + ") failed";
            return false;
        }
        return true;
    };

    // ⚠️ LESS_EQUAL, NOT LESS, for the visible pass. A section contour sits
    // EXACTLY on the surface it cuts wherever the cut plane grazes a floor slab,
    // so equal depth is the common case rather than the edge case -- and under
    // LESS the contour would vanish along precisely those runs.
    if (!buildPso ("Story slice outline (visible) PSO", vs, ps, outlineLayout, _countof (outlineLayout),
                   Diligent::COMPARISON_FUNC_LESS_EQUAL, impl_->outlineVisiblePso, impl_->outlineVisibleSrb))
        return false;
    if (!buildPso ("Story slice outline (occluded) PSO", vs, ps, outlineLayout, _countof (outlineLayout),
                   Diligent::COMPARISON_FUNC_GREATER, impl_->outlineOccludedPso, impl_->outlineOccludedSrb))
        return false;
    if (!buildPso ("Story slice fill (visible) PSO", fillVs, fillPs, fillLayout, _countof (fillLayout),
                   Diligent::COMPARISON_FUNC_LESS_EQUAL, impl_->fillVisiblePso, impl_->fillVisibleSrb))
        return false;
    if (!buildPso ("Story slice fill (occluded) PSO", fillVs, fillPs, fillLayout, _countof (fillLayout),
                   Diligent::COMPARISON_FUNC_GREATER, impl_->fillOccludedPso, impl_->fillOccludedSrb))
        return false;

    impl_->ready = true;
    return true;
}

namespace {

// Grow-only upload of one vertex array. Shared by the outline and the fill
// because they differ in nothing but their element size, and two copies of a
// map-and-memcpy is two places for a stale count to survive a failed map.
bool UploadBuffer (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, const char* name,
                   const void* data, size_t count, size_t stride, RefCntAutoPtr<Diligent::IBuffer>& buffer,
                   size_t& capacity, size_t& outCount, std::string& error)
{
    outCount = count;
    if (count == 0)
        return true; // an empty storey set is ordinary, not an error

    if (buffer == nullptr || count > capacity) {
        buffer.Release ();
        // Headroom, so a storey list that grows by one does not reallocate.
        const size_t want = std::max<size_t> (count * 2, 4096);
        Diligent::BufferDesc vbd;
        vbd.Name = name;
        vbd.Size = Diligent::Uint64 (want * stride);
        vbd.Usage = Diligent::USAGE_DYNAMIC;
        vbd.BindFlags = Diligent::BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
        device->CreateBuffer (vbd, nullptr, &buffer);
        if (buffer == nullptr) {
            capacity = 0;
            outCount = 0;
            error = std::string ("Diligent CreateBuffer(") + name + ") failed";
            return false;
        }
        capacity = want;
    }

    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr) {
        // ⚠️ THE COUNT IS ZEROED, NOT LEFT ALONE. A failed map leaves the buffer
        // holding the PREVIOUS storey set, and drawing that at the new count is a
        // confident picture of the wrong building.
        outCount = 0;
        error = std::string ("Diligent MapBuffer(") + name + ") failed";
        return false;
    }
    std::memcpy (mapped, data, count * stride);
    context->UnmapBuffer (buffer, Diligent::MAP_WRITE);
    return true;
}

} // namespace

bool StorySliceLayer::Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                              const std::vector<StorySliceVertex>& outline,
                              const std::vector<StorySliceFillVertex>& fill, std::string& error)
{
    if (!impl_->ready) {
        error = "StorySliceLayer::Upload before Init";
        return false;
    }
    const bool a = UploadBuffer (device, context, "Story slice outline vertices", outline.data (), outline.size (),
                                 sizeof (StorySliceVertex), impl_->outlineVertices, impl_->outlineCapacity,
                                 impl_->outlineCount, error);
    // ⚠️ BOTH ARE ATTEMPTED EVEN IF THE FIRST FAILED. They are one picture: an
    // outline uploaded over a stale fill draws a contour around last storey's
    // shaded region, which looks like a registration bug rather than an upload
    // failure. Whichever fails zeroes its own count, so the picture degrades to
    // "half of it is missing", which is legible.
    std::string fillError;
    const bool b = UploadBuffer (device, context, "Story slice fill vertices", fill.data (), fill.size (),
                                 sizeof (StorySliceFillVertex), impl_->fillVertices, impl_->fillCapacity,
                                 impl_->fillCount, fillError);
    if (a && !b)
        error = fillError;
    return a && b;
}

void StorySliceLayer::Draw (Diligent::IDeviceContext* context, const float viewProj[16], uint32_t surfaceWidth,
                            uint32_t surfaceHeight, const DrawParams& params)
{
    if (!impl_->ready || surfaceWidth == 0 || surfaceHeight == 0)
        return;
    const bool haveOutline = impl_->outlineCount > 0 && impl_->outlineVertices != nullptr;
    const bool haveFill = params.drawFill && impl_->fillCount > 0 && impl_->fillVertices != nullptr;
    if (!haveOutline && !haveFill)
        return;

    StorySliceConstants constants = {};
    std::memcpy (constants.viewProj, viewProj, sizeof (constants.viewProj));
    // NDC spans 2 across the surface and the shader pushes each way, so half the
    // width in pixels becomes (half / size) * 2 == width / size.
    constants.expand[0] = params.widthPixels / float (surfaceWidth);
    constants.expand[1] = params.widthPixels / float (surfaceHeight);
    UnpackRgba (params.rgba, constants.color);
    UnpackRgba (params.fillRgba, constants.fillColor);
    constants.dash[0] = params.dashPixels;
    constants.dash[1] = params.dashDuty;

    auto upload = [&] () {
        Diligent::PVoid mapped = nullptr;
        context->MapBuffer (impl_->constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
        if (mapped == nullptr)
            return false;
        *static_cast<StorySliceConstants*> (mapped) = constants;
        context->UnmapBuffer (impl_->constants, Diligent::MAP_WRITE);
        return true;
    };

    auto drawPass = [&] (Diligent::IBuffer* vertices, size_t count, Diligent::Uint32 stride,
                         Diligent::IPipelineState* pso, Diligent::IShaderResourceBinding* srb) {
        Diligent::IBuffer* vertexBuffers[1] = { vertices };
        const Diligent::Uint64 offsets[1] = { 0 };
        context->SetVertexBuffers (0, 1, vertexBuffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                   Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
        context->SetPipelineState (pso);
        context->CommitShaderResources (srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawAttribs draw;
        draw.NumVertices = Diligent::Uint32 (count);
        draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw (draw);
        (void) stride;
    };

    const bool drawOccluded = params.occluded != OccludedStyle::Hidden;

    // ⚠️ THE FILL GOES FIRST, BOTH PASSES, so the outline lands on top of it. A
    // low-opacity fill drawn over the contour washes the contour out, and the
    // contour is the part carrying the information.
    if (haveFill) {
        constants.dash[2] = 0.0f; // a dashed FILL is meaningless
        if (upload ())
            drawPass (impl_->fillVertices, impl_->fillCount, sizeof (StorySliceFillVertex), impl_->fillVisiblePso,
                      impl_->fillVisibleSrb);
        if (drawOccluded && upload ())
            drawPass (impl_->fillVertices, impl_->fillCount, sizeof (StorySliceFillVertex), impl_->fillOccludedPso,
                      impl_->fillOccludedSrb);
    }

    if (haveOutline) {
        constants.dash[2] = 0.0f; // the visible half is always solid
        if (upload ())
            drawPass (impl_->outlineVertices, impl_->outlineCount, sizeof (StorySliceVertex), impl_->outlineVisiblePso,
                      impl_->outlineVisibleSrb);
        if (drawOccluded) {
            constants.dash[2] = (params.occluded == OccludedStyle::Dashed) ? 1.0f : 0.0f;
            if (upload ())
                drawPass (impl_->outlineVertices, impl_->outlineCount, sizeof (StorySliceVertex),
                          impl_->outlineOccludedPso, impl_->outlineOccludedSrb);
        }
    }
}

} // namespace archviz
} // namespace geomsrv
