#include "ArchViz/DiligentPointCloudLayer.hpp"

#include "ArchViz/PointCloudLod.hpp"

#include <windows.h>
#include <d3d11.h>
#include <Buffer.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Shader.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace geomsrv {
namespace archviz {

namespace {

constexpr size_t kPointBudget = 2'000'000;
constexpr float kLodPixelError = 1.25f;
constexpr float kMinimumRadiusMetres = 0.003f;

const char* kPointCloudVS = R"hlsl(
cbuffer PointCloudConstants
{
    float4x4 g_pointViewProj;
    float4 g_pointOrigin;
    float4 g_pointViewport;
    float4 g_pointParams;
};

struct VSInput
{
    float3 position : ATTRIB0;
    float4 colour : ATTRIB1;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 footprint : TEX_COORD0;
    float4 colour : COLOR0;
};

float2 Corner (uint vertexId)
{
    if (vertexId == 0) return float2 (-1.0, -1.0);
    if (vertexId == 1) return float2 ( 1.0, -1.0);
    if (vertexId == 2) return float2 ( 1.0,  1.0);
    if (vertexId == 3) return float2 (-1.0, -1.0);
    if (vertexId == 4) return float2 ( 1.0,  1.0);
    return float2 (-1.0, 1.0);
}

VSOutput main (VSInput input, uint vertexId : SV_VertexID)
{
    VSOutput output;
    float3 world = input.position + g_pointOrigin.xyz;
    float4 centre = mul (g_pointViewProj, float4 (world, 1.0));
    float2 local = Corner (vertexId);

    float radiusPixels = g_pointParams.x;
    if (g_pointParams.y > 0.5)
        radiusPixels /= max (abs (centre.w), 1.0e-4);
    radiusPixels = clamp (radiusPixels, 0.65, 4.0);
    float2 offsetNdc = local * radiusPixels * float2 (2.0 / g_pointViewport.x, -2.0 / g_pointViewport.y);

    output.position = centre + float4 (offsetNdc * centre.w, 0.0, 0.0);
    output.footprint = local;
    output.colour = input.colour;
    return output;
}
)hlsl";

const char* kPointCloudPS = R"hlsl(
struct PSInput
{
    float4 position : SV_POSITION;
    float2 footprint : TEX_COORD0;
    float4 colour : COLOR0;
};

float3 SrgbToLinear (float3 colour)
{
    float3 low = colour / 12.92;
    float3 high = pow ((colour + 0.055) / 1.055, 2.4);
    return lerp (high, low, colour <= 0.04045);
}

float4 main (PSInput input) : SV_TARGET
{
    clip (1.0 - dot (input.footprint, input.footprint));
    return float4 (SrgbToLinear (input.colour.rgb), 1.0);
}
)hlsl";

struct PointConstants {
    float viewProj[16];
    float origin[4] = {};
    float viewport[4] = {};
    float params[4] = {};
};

static_assert ((sizeof (PointConstants) % 16) == 0, "D3D11 constant buffers require 16-byte alignment");

struct PointNode {
    Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
    uint32_t parentId = UINT32_MAX;
    uint32_t pointCount = 0;
    size_t gpuBytes = 0;
    bool active = false;
};

struct PointLayer {
    PointLayerUpload metadata;
    PointCloudHierarchy hierarchy;
    std::vector<PointNode> nodes;
    bool complete = false;
};

} // namespace

struct DiligentPointCloudLayer::Impl {
    Diligent::RefCntAutoPtr<Diligent::IShader> vertexShader;
    Diligent::RefCntAutoPtr<Diligent::IShader> pixelShader;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> constants;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> ldrPso;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> hdrPso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> ldrSrb;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> hdrSrb;
    std::vector<PointLayer> layers;
    DiligentPointCloudStats stats;
    bool ready = false;

    PointLayer* FindLayer (const std::string& id)
    {
        for (PointLayer& layer : layers) {
            if (layer.metadata.layerId == id)
                return &layer;
        }
        return nullptr;
    }
};

DiligentPointCloudLayer::DiligentPointCloudLayer () : impl_ (std::make_unique<Impl> ())
{
}

DiligentPointCloudLayer::~DiligentPointCloudLayer ()
{
    Shutdown ();
}

bool DiligentPointCloudLayer::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                    uint32_t depthBufferFormat, std::string& error)
{
    if (device == nullptr) {
        error = "DiligentPointCloudLayer::Init got no render device";
        return false;
    }
    if (impl_->ready)
        return true;

    const auto compile = [&] (Diligent::SHADER_TYPE type, const char* name, const char* source,
                              Diligent::RefCntAutoPtr<Diligent::IShader>& shader) {
        Diligent::ShaderCreateInfo createInfo;
        createInfo.Desc.Name = name;
        createInfo.Desc.ShaderType = type;
        createInfo.EntryPoint = "main";
        createInfo.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        createInfo.Source = source;
        device->CreateShader (createInfo, &shader, nullptr);
        if (shader != nullptr)
            return true;
        error = std::string ("Diligent CreateShader(") + name + ") failed";
        return false;
    };
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "ArchViz point-cloud VS", kPointCloudVS, impl_->vertexShader) ||
        !compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz point-cloud PS", kPointCloudPS, impl_->pixelShader))
        return false;

    Diligent::BufferDesc constantsDesc;
    constantsDesc.Name = "ArchViz point-cloud constants";
    constantsDesc.Size = sizeof (PointConstants);
    constantsDesc.Usage = Diligent::USAGE_DYNAMIC;
    constantsDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    constantsDesc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (constantsDesc, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent CreateBuffer(ArchViz point-cloud constants) failed";
        return false;
    }

    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False, 0,
                                  uint32_t (sizeof (PointCloudVertex)),
                                  Diligent::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE },
        Diligent::LayoutElement { 1, 0, 4, Diligent::VT_UINT8, Diligent::True, 12, uint32_t (sizeof (PointCloudVertex)),
                                  Diligent::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE },
    };

    const auto makePipeline = [&] (const char* name, Diligent::TEXTURE_FORMAT format,
                                   Diligent::RefCntAutoPtr<Diligent::IPipelineState>& pso,
                                   Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb) {
        Diligent::GraphicsPipelineStateCreateInfo createInfo;
        createInfo.PSODesc.Name = name;
        createInfo.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        Diligent::GraphicsPipelineDesc& graphics = createInfo.GraphicsPipeline;
        graphics.NumRenderTargets = 1;
        graphics.RTVFormats[0] = format;
        graphics.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
        graphics.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        graphics.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        graphics.DepthStencilDesc.DepthEnable = Diligent::True;
        graphics.DepthStencilDesc.DepthWriteEnable = Diligent::True;
        graphics.BlendDesc.RenderTargets[0].BlendEnable = Diligent::False;
        graphics.InputLayout.LayoutElements = layout;
        graphics.InputLayout.NumElements = _countof (layout);
        createInfo.pVS = impl_->vertexShader;
        createInfo.pPS = impl_->pixelShader;
        device->CreateGraphicsPipelineState (createInfo, &pso);
        if (pso == nullptr) {
            error = std::string ("Diligent CreateGraphicsPipelineState(") + name + ") failed";
            return false;
        }
        Diligent::IShaderResourceVariable* constants =
            pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "PointCloudConstants");
        if (constants == nullptr) {
            error = std::string ("Diligent could not bind PointCloudConstants for ") + name;
            return false;
        }
        constants->Set (impl_->constants);
        pso->CreateShaderResourceBinding (&srb, true);
        if (srb != nullptr)
            return true;
        error = std::string ("Diligent CreateShaderResourceBinding(") + name + ") failed";
        return false;
    };

    if (!makePipeline ("ArchViz point-cloud LDR PSO", static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat),
                       impl_->ldrPso, impl_->ldrSrb) ||
        !makePipeline ("ArchViz point-cloud HDR PSO", Diligent::TEX_FORMAT_RGBA16_FLOAT, impl_->hdrPso, impl_->hdrSrb))
        return false;

    impl_->ready = true;
    return true;
}

void DiligentPointCloudLayer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->layers.clear ();
    impl_->ldrSrb.Release ();
    impl_->hdrSrb.Release ();
    impl_->ldrPso.Release ();
    impl_->hdrPso.Release ();
    impl_->constants.Release ();
    impl_->pixelShader.Release ();
    impl_->vertexShader.Release ();
    impl_->stats = {};
    impl_->ready = false;
}

void DiligentPointCloudLayer::BeginLayer (const PointLayerUpload& upload)
{
    ClearLayer (upload.layerId);
    PointLayer layer;
    layer.metadata = upload;
    impl_->layers.push_back (std::move (layer));
}

void DiligentPointCloudLayer::ClearLayer (const std::string& layerId)
{
    for (size_t i = 0; i < impl_->layers.size (); ++i) {
        if (impl_->layers[i].metadata.layerId != layerId)
            continue;
        impl_->layers[i] = std::move (impl_->layers.back ());
        impl_->layers.pop_back ();
        return;
    }
}

bool DiligentPointCloudLayer::UpsertNode (Diligent::IRenderDevice* device, const PointNodeUpload& upload)
{
    PointLayer* layer = impl_->FindLayer (upload.layerId);
    if (device == nullptr || layer == nullptr || upload.vertices.empty ())
        return false;

    Diligent::BufferDesc desc;
    desc.Name = "ArchViz point-cloud node";
    desc.Size = upload.vertices.size () * sizeof (PointCloudVertex);
    desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    const Diligent::BufferData data { upload.vertices.data (), desc.Size };
    Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
    device->CreateBuffer (desc, &data, &buffer);
    if (buffer == nullptr)
        return false;

    const size_t required = size_t (upload.nodeId) + 1;
    if (layer->nodes.size () < required) {
        layer->nodes.resize (required);
        layer->hierarchy.nodes.resize (required);
    }

    PointNode& node = layer->nodes[upload.nodeId];
    if (node.active && node.parentId < layer->hierarchy.nodes.size ()) {
        PointCloudHierarchyNode& oldParent = layer->hierarchy.nodes[node.parentId];
        for (uint32_t& child : oldParent.children) {
            if (child == upload.nodeId)
                child = UINT32_MAX;
        }
    }
    node.buffer = std::move (buffer);
    node.parentId = upload.parentId;
    node.pointCount = uint32_t (upload.vertices.size ());
    node.gpuBytes = desc.Size;
    node.active = true;

    PointCloudHierarchyNode& hierarchyNode = layer->hierarchy.nodes[upload.nodeId];
    hierarchyNode.id = upload.nodeId;
    hierarchyNode.level = upload.level;
    hierarchyNode.geometricError = upload.geometricError;
    std::copy (upload.boundsMin, upload.boundsMin + 3, hierarchyNode.boundsMin);
    std::copy (upload.boundsMax, upload.boundsMax + 3, hierarchyNode.boundsMax);
    hierarchyNode.pointIndices.resize (upload.vertices.size ());

    // PointCloudHierarchy guarantees parent-before-child upload order. Updating
    // only this link keeps progressive upload linear in the number of nodes.
    if (node.parentId < layer->hierarchy.nodes.size ()) {
        PointCloudHierarchyNode& parent = layer->hierarchy.nodes[node.parentId];
        if (parent.children[0] == UINT32_MAX || parent.children[0] == upload.nodeId)
            parent.children[0] = upload.nodeId;
        else if (parent.children[1] == UINT32_MAX || parent.children[1] == upload.nodeId)
            parent.children[1] = upload.nodeId;
    }
    layer->complete = false;
    return true;
}

void DiligentPointCloudLayer::EndLayer (const std::string& layerId)
{
    if (PointLayer* layer = impl_->FindLayer (layerId))
        layer->complete = true;
}

size_t DiligentPointCloudLayer::Draw (Diligent::IDeviceContext* context, const float viewProj[16],
                                      const float projection[16], const float eye[3], uint32_t viewportWidth,
                                      uint32_t viewportHeight, bool hdr, uint32_t frameIndex)
{
    (void) frameIndex;
    impl_->stats.drawCalls = 0;
    impl_->stats.visiblePoints = 0;
    if (!impl_->ready || context == nullptr || viewportWidth == 0 || viewportHeight == 0)
        return 0;

    context->SetPipelineState (hdr ? impl_->hdrPso : impl_->ldrPso);
    context->CommitShaderResources (hdr ? impl_->hdrSrb : impl_->ldrSrb,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    PointCloudLodCamera cameraTemplate;
    cameraTemplate.viewportHeightPixels = float (viewportHeight);
    cameraTemplate.orthographic = std::abs (projection[15]) > 0.5f;
    if (cameraTemplate.orthographic)
        cameraTemplate.orthographicHeight = 2.0f / (std::max) (std::abs (projection[5]), 1.0e-6f);
    else
        cameraTemplate.verticalFieldOfViewRadians =
            2.0f * std::atan (1.0f / (std::max) (std::abs (projection[5]), 1.0e-6f));

    size_t remainingBudget = kPointBudget;
    for (PointLayer& layer : impl_->layers) {
        if (remainingBudget == 0 || layer.nodes.empty () || !layer.nodes[0].active)
            continue;
        PointCloudLodCamera camera = cameraTemplate;
        for (size_t axis = 0; axis < 3; ++axis)
            camera.eye[axis] = eye[axis] - float (layer.metadata.rtcOrigin[axis]);
        const std::vector<uint32_t> selected =
            SelectPointCloudLod (layer.hierarchy, camera, kLodPixelError, remainingBudget);
        for (uint32_t id : selected) {
            if (id >= layer.nodes.size ())
                continue;
            const PointNode& node = layer.nodes[id];
            if (!node.active || node.buffer == nullptr || node.pointCount == 0)
                continue;

            const PointCloudHierarchyNode& hierarchyNode = layer.hierarchy.nodes[id];
            const float radius =
                (std::max) (kMinimumRadiusMetres, hierarchyNode.geometricError /
                                                      std::sqrt ((std::max) (float (node.pointCount), 1.0f)) * 0.65f);
            PointConstants constants;
            std::memcpy (constants.viewProj, viewProj, sizeof (constants.viewProj));
            for (size_t axis = 0; axis < 3; ++axis)
                constants.origin[axis] = float (layer.metadata.rtcOrigin[axis]);
            constants.viewport[0] = float (viewportWidth);
            constants.viewport[1] = float (viewportHeight);
            const float pixelsPerMetre =
                camera.orthographic
                    ? float (viewportHeight) / camera.orthographicHeight
                    : float (viewportHeight) / (2.0f * std::tan (camera.verticalFieldOfViewRadians * 0.5f));
            constants.params[0] = radius * pixelsPerMetre;
            constants.params[1] = camera.orthographic ? 0.0f : 1.0f;
            constants.params[2] = radius;

            Diligent::PVoid mapped = nullptr;
            context->MapBuffer (impl_->constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
            if (mapped == nullptr)
                continue;
            *static_cast<PointConstants*> (mapped) = constants;
            context->UnmapBuffer (impl_->constants, Diligent::MAP_WRITE);

            Diligent::IBuffer* buffers[] = { node.buffer };
            const Diligent::Uint64 offsets[] = { 0 };
            context->SetVertexBuffers (0, 1, buffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                       Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
            Diligent::DrawAttribs draw;
            draw.NumVertices = 6;
            draw.NumInstances = node.pointCount;
            draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
            context->Draw (draw);
            ++impl_->stats.drawCalls;
            impl_->stats.visiblePoints += node.pointCount;
            remainingBudget = node.pointCount < remainingBudget ? remainingBudget - node.pointCount : 0;
        }
    }
    return impl_->stats.drawCalls;
}

DiligentPointCloudStats DiligentPointCloudLayer::Stats () const
{
    DiligentPointCloudStats stats = impl_->stats;
    stats.layers = impl_->layers.size ();
    stats.nodes = 0;
    stats.points = 0;
    stats.gpuBytes = 0;
    for (const PointLayer& layer : impl_->layers) {
        for (const PointNode& node : layer.nodes) {
            if (!node.active)
                continue;
            ++stats.nodes;
            stats.points += node.pointCount;
            stats.gpuBytes += node.gpuBytes;
        }
    }
    return stats;
}

} // namespace archviz
} // namespace geomsrv
