#include "ArchViz/SceneTextLayer.hpp"

#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SceneTextAtlas.hpp"
#include "ArchViz/SceneTextLayout.hpp"

#include <windows.h>
#include <Buffer.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <InputLayout.h>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>
#include <Texture.h>
#include <TextureView.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace geomsrv::archviz {
namespace {

constexpr int kSceneTextFontResourceId = 32581;
constexpr size_t kMaximumLabels = 256;
constexpr size_t kMaximumCodepointsPerLabel = 512;

enum class VerticalAnchor : uint8_t { Baseline, Top, Bottom };

struct PreparedSceneTextLabel {
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    const std::string* text = nullptr;
    float sizePixels = 18.0f;
    uint32_t rgba = 0xFFFFFFFFu;
    SceneTextAlignment alignment = SceneTextAlignment::Left;
    VerticalAnchor verticalAnchor = VerticalAnchor::Baseline;
};

struct SceneTextVertex {
    float position[2];
    float uv[2];
    uint32_t abgr;
};

struct SceneTextConstants {
    float surface[4]; // xy = inverse surface, zw = inverse atlas
    float atlas[4];   // x = distance range in atlas pixels
};

constexpr const char* kSceneTextVS = R"hlsl(
cbuffer SceneTextConstants { float4 g_surface; float4 g_atlasParams; };
struct VSInput { float2 position : ATTRIB0; float2 uv : ATTRIB1; float4 color : ATTRIB2; };
struct PSInput { float4 position : SV_POSITION; float2 uv : TEX_COORD; float4 color : COLOR; };
void main (in VSInput input, out PSInput output)
{
    output.position = float4(input.position.x*g_surface.x*2.0-1.0,
                             1.0-input.position.y*g_surface.y*2.0, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
}
)hlsl";

constexpr const char* kSceneTextPS = R"hlsl(
cbuffer SceneTextConstants { float4 g_surface; float4 g_atlasParams; };
Texture2D g_atlas;
SamplerState g_atlas_sampler;
struct PSInput { float4 position : SV_POSITION; float2 uv : TEX_COORD; float4 color : COLOR; };
float Median(float3 value) { return max(min(value.r, value.g), min(max(value.r, value.g), value.b)); }
float4 main (PSInput input) : SV_TARGET
{
    float distance = Median(g_atlas.Sample(g_atlas_sampler, input.uv).rgb);
    float2 unitRange = g_atlasParams.x*g_surface.zw;
    float2 screenTexelRange = 1.0/max(fwidth(input.uv), float2(1e-6, 1e-6));
    float screenRange = max(0.5*dot(unitRange, screenTexelRange), 1.0);
    float coverage = saturate(screenRange*(distance-0.5)+0.5)*input.color.a;
    return float4(input.color.rgb*coverage, coverage);
}
)hlsl";

bool LoadFontResource (std::vector<uint8_t>& bytes, std::string& error)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR> (&LoadFontResource), &module)) {
        error = "could not resolve the add-on module for the bundled text font";
        return false;
    }
    const HRSRC resource = FindResourceW (module, MAKEINTRESOURCEW (kSceneTextFontResourceId), L"DATA");
    const DWORD resourceSize = resource != nullptr ? SizeofResource (module, resource) : 0;
    const HGLOBAL loaded = resource != nullptr ? LoadResource (module, resource) : nullptr;
    const void* data = loaded != nullptr ? LockResource (loaded) : nullptr;
    if (data == nullptr || resourceSize < sizeof (uint32_t)) {
        error = "bundled Noto Sans resource 32581 is missing or unreadable";
        return false;
    }
    uint32_t payloadSize = 0;
    std::memcpy (&payloadSize, data, sizeof (payloadSize));
    if (payloadSize == 0 || payloadSize > resourceSize - sizeof (payloadSize)) {
        error = "bundled Noto Sans resource has an invalid payload size";
        return false;
    }
    const uint8_t* payload = static_cast<const uint8_t*> (data) + sizeof (payloadSize);
    bytes.assign (payload, payload + payloadSize);
    return true;
}

float SrgbToLinear (uint8_t channel)
{
    const float value = float (channel) / 255.0f;
    return value <= 0.04045f ? value / 12.92f : std::pow ((value + 0.055f) / 1.055f, 2.4f);
}

uint32_t LinearAbgr (uint32_t rgba)
{
    const uint8_t red = uint8_t (std::round (SrgbToLinear (uint8_t (rgba >> 24)) * 255.0f));
    const uint8_t green = uint8_t (std::round (SrgbToLinear (uint8_t (rgba >> 16)) * 255.0f));
    const uint8_t blue = uint8_t (std::round (SrgbToLinear (uint8_t (rgba >> 8)) * 255.0f));
    const uint8_t alpha = uint8_t (rgba);
    return uint32_t (red) | (uint32_t (green) << 8) | (uint32_t (blue) << 16) | (uint32_t (alpha) << 24);
}

bool ProjectAnchor (const SceneTextLabel& label, const float viewProj[16], uint32_t width, uint32_t height, float& x,
                    float& y)
{
    const float input[4] = { float (label.anchor[0]), float (label.anchor[1]), float (label.anchor[2]), 1.0f };
    float clip[4];
    TransformPoint (clip, input, viewProj);
    if (!std::isfinite (clip[0]) || !std::isfinite (clip[1]) || !std::isfinite (clip[2]) || !std::isfinite (clip[3]) ||
        clip[3] <= 1e-6f || clip[2] < 0.0f || clip[2] > clip[3])
        return false;
    x = (clip[0] / clip[3] * 0.5f + 0.5f) * float (width);
    y = (0.5f - clip[1] / clip[3] * 0.5f) * float (height);
    return std::isfinite (x) && std::isfinite (y);
}

void AddQuad (std::vector<SceneTextVertex>& vertices, float left, float top, float right, float bottom, float u0,
              float v0, float u1, float v1, uint32_t color)
{
    const SceneTextVertex quad[6] = {
        { { left, top }, { u0, v1 }, color },     { { right, top }, { u1, v1 }, color },
        { { right, bottom }, { u1, v0 }, color }, { { left, top }, { u0, v1 }, color },
        { { right, bottom }, { u1, v0 }, color }, { { left, bottom }, { u0, v0 }, color },
    };
    vertices.insert (vertices.end (), std::begin (quad), std::end (quad));
}

} // namespace

struct SceneTextLayer::Impl {
    SceneTextAtlas atlas;
    SceneTextShaper shaper;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> constants;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> vertices;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    size_t vertexCapacity = 0;
    SceneTextLayerStats stats;

    void DrawPrepared (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                       const std::vector<PreparedSceneTextLabel>& labels, uint32_t surfaceWidth,
                       uint32_t surfaceHeight);
};

SceneTextLayer::SceneTextLayer () : impl_ (new Impl ())
{
}
SceneTextLayer::~SceneTextLayer ()
{
    Shutdown ();
}

bool SceneTextLayer::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                           std::string& error)
{
    if (device == nullptr) {
        error = "SceneTextLayer::Init got no render device";
        return false;
    }
    std::vector<uint8_t> fontBytes;
    if (!LoadFontResource (fontBytes, error) || !impl_->shaper.Init (fontBytes.data (), fontBytes.size (), error) ||
        !impl_->atlas.Build (fontBytes.data (), fontBytes.size (), error))
        return false;

    Diligent::TextureDesc textureDesc;
    textureDesc.Name = "Scene text linear MTSDF atlas";
    textureDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    textureDesc.Width = static_cast<Diligent::Uint32> (impl_->atlas.Width ());
    textureDesc.Height = static_cast<Diligent::Uint32> (impl_->atlas.Height ());
    textureDesc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
    textureDesc.Usage = Diligent::USAGE_IMMUTABLE;
    textureDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    Diligent::TextureSubResData level;
    level.pData = impl_->atlas.Pixels ().data ();
    level.Stride = Diligent::Uint64 (impl_->atlas.Width () * 4);
    Diligent::TextureData initial;
    initial.pSubResources = &level;
    initial.NumSubresources = 1;
    device->CreateTexture (textureDesc, &initial, &impl_->texture);
    if (impl_->texture == nullptr) {
        error = "Diligent could not create the linear MTSDF atlas texture";
        return false;
    }

    auto compile = [&] (Diligent::SHADER_TYPE type, const char* name, const char* source,
                        Diligent::RefCntAutoPtr<Diligent::IShader>& shader) {
        Diligent::ShaderCreateInfo info;
        info.Desc.Name = name;
        info.Desc.ShaderType = type;
        info.EntryPoint = "main";
        info.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        info.Source = source;
        info.SourceLength = std::strlen (source);
        device->CreateShader (info, &shader, nullptr);
        return shader != nullptr;
    };
    Diligent::RefCntAutoPtr<Diligent::IShader> vertexShader, pixelShader;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "Scene text VS", kSceneTextVS, vertexShader) ||
        !compile (Diligent::SHADER_TYPE_PIXEL, "Scene text MTSDF PS", kSceneTextPS, pixelShader)) {
        error = "Diligent could not compile the scene-text shaders";
        return false;
    }

    Diligent::BufferDesc constantDesc;
    constantDesc.Name = "Scene text constants";
    constantDesc.Size = sizeof (SceneTextConstants);
    constantDesc.Usage = Diligent::USAGE_DYNAMIC;
    constantDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    constantDesc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (constantDesc, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent could not create the scene-text constant buffer";
        return false;
    }

    const Diligent::LayoutElement layout[] = {
        { 0, 0, 2, Diligent::VT_FLOAT32, Diligent::False },
        { 1, 0, 2, Diligent::VT_FLOAT32, Diligent::False },
        { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },
    };
    Diligent::GraphicsPipelineStateCreateInfo pipeline;
    pipeline.PSODesc.Name = "Scene text MTSDF PSO";
    auto& graphics = pipeline.GraphicsPipeline;
    graphics.NumRenderTargets = 1;
    graphics.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    graphics.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    graphics.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    graphics.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    graphics.DepthStencilDesc.DepthEnable = Diligent::False;
    graphics.DepthStencilDesc.DepthWriteEnable = Diligent::False;
    graphics.InputLayout.LayoutElements = layout;
    graphics.InputLayout.NumElements = _countof (layout);
    auto& blend = graphics.BlendDesc.RenderTargets[0];
    blend.BlendEnable = Diligent::True;
    blend.SrcBlend = Diligent::BLEND_FACTOR_ONE;
    blend.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
    blend.SrcBlendAlpha = Diligent::BLEND_FACTOR_ONE;
    blend.DestBlendAlpha = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
    pipeline.pVS = vertexShader;
    pipeline.pPS = pixelShader;
    pipeline.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    Diligent::ShaderResourceVariableDesc variable { Diligent::SHADER_TYPE_PIXEL, "g_atlas",
                                                    Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE };
    pipeline.PSODesc.ResourceLayout.Variables = &variable;
    pipeline.PSODesc.ResourceLayout.NumVariables = 1;
    Diligent::SamplerDesc samplerDesc;
    samplerDesc.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    samplerDesc.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    samplerDesc.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    samplerDesc.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    const Diligent::ImmutableSamplerDesc sampler { Diligent::SHADER_TYPE_PIXEL, "g_atlas_sampler", samplerDesc };
    pipeline.PSODesc.ResourceLayout.ImmutableSamplers = &sampler;
    pipeline.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    device->CreateGraphicsPipelineState (pipeline, &impl_->pso);
    if (impl_->pso == nullptr) {
        error = "Diligent could not create the scene-text pipeline";
        return false;
    }
    if (auto* value = impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "SceneTextConstants"))
        value->Set (impl_->constants);
    if (auto* value = impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "SceneTextConstants"))
        value->Set (impl_->constants);
    impl_->pso->CreateShaderResourceBinding (&impl_->srb, true);
    if (impl_->srb == nullptr) {
        error = "Diligent could not create scene-text shader bindings";
        return false;
    }
    auto* atlasVariable = impl_->srb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_atlas");
    if (atlasVariable == nullptr) {
        error = "the scene-text atlas shader variable is missing";
        return false;
    }
    atlasVariable->Set (impl_->texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    impl_->stats.ready = true;
    impl_->stats.atlasWidth = static_cast<uint32_t> (impl_->atlas.Width ());
    impl_->stats.atlasHeight = static_cast<uint32_t> (impl_->atlas.Height ());
    impl_->stats.atlasBytes = impl_->atlas.Pixels ().size ();
    return true;
}

void SceneTextLayer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->vertices.Release ();
    impl_->constants.Release ();
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->texture.Release ();
    impl_->vertexCapacity = 0;
    impl_->stats = {};
}

void SceneTextLayer::Draw (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                           const std::vector<SceneTextLabel>& labels, const float viewProj[16], uint32_t surfaceWidth,
                           uint32_t surfaceHeight, float dpiScale)
{
    impl_->stats.labels = impl_->stats.glyphs = 0;
    if (!impl_->stats.ready || device == nullptr || context == nullptr || surfaceWidth == 0 || surfaceHeight == 0 ||
        !std::isfinite (dpiScale) || dpiScale <= 0.0f)
        return;
    std::vector<PreparedSceneTextLabel> prepared;
    prepared.reserve ((std::min) (labels.size (), kMaximumLabels));
    for (size_t labelIndex = 0; labelIndex < (std::min) (labels.size (), kMaximumLabels); ++labelIndex) {
        const SceneTextLabel& label = labels[labelIndex];
        float anchorX = 0.0f, anchorY = 0.0f;
        if (label.text.empty () || !ProjectAnchor (label, viewProj, surfaceWidth, surfaceHeight, anchorX, anchorY))
            continue;
        prepared.push_back ({ anchorX, anchorY, &label.text, std::clamp (label.sizePixels * dpiScale, 6.0f, 192.0f),
                              label.rgba, label.alignment, VerticalAnchor::Baseline });
    }
    impl_->DrawPrepared (device, context, prepared, surfaceWidth, surfaceHeight);
}

void SceneTextLayer::DrawProjected (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                    const std::vector<ScreenLabel>& labels, uint32_t surfaceWidth,
                                    uint32_t surfaceHeight, float dpiScale)
{
    if (!impl_->stats.ready || device == nullptr || context == nullptr || surfaceWidth == 0 || surfaceHeight == 0 ||
        !std::isfinite (dpiScale) || dpiScale <= 0.0f)
        return;
    std::vector<PreparedSceneTextLabel> prepared;
    prepared.reserve ((std::min) (labels.size (), kMaximumLabels));
    for (size_t labelIndex = 0; labelIndex < (std::min) (labels.size (), kMaximumLabels); ++labelIndex) {
        const ScreenLabel& label = labels[labelIndex];
        if (label.text.empty () || !std::isfinite (label.anchor.x) || !std::isfinite (label.anchor.y))
            continue;
        const float pixelSize = label.fontSize > 0.0f ? label.fontSize : 18.0f * dpiScale;
        const float horizontalOffset = label.centered ? 0.0f : 4.0f * dpiScale;
        prepared.push_back ({ label.anchor.x + horizontalOffset, label.anchor.y, &label.text,
                              std::clamp (pixelSize, 6.0f, 192.0f), label.rgba,
                              label.centered ? SceneTextAlignment::Center : SceneTextAlignment::Left,
                              label.centered ? VerticalAnchor::Top : VerticalAnchor::Bottom });
    }
    impl_->DrawPrepared (device, context, prepared, surfaceWidth, surfaceHeight);
}

void SceneTextLayer::Impl::DrawPrepared (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                         const std::vector<PreparedSceneTextLabel>& labels, uint32_t surfaceWidth,
                                         uint32_t surfaceHeight)
{
    std::vector<SceneTextVertex> vertices;
    for (const PreparedSceneTextLabel& label : labels) {
        SceneTextGlyphRun run;
        std::string shapeError;
        if (label.text == nullptr || !shaper.Shape (*label.text, SceneTextDirection::Auto, run, shapeError))
            continue;
        if (run.glyphs.size () > kMaximumCodepointsPerLabel)
            run.glyphs.resize (kMaximumCodepointsPerLabel);
        const float pixelSize = label.sizePixels;
        float advance = 0.0f;
        for (const SceneTextPositionedGlyph& glyph : run.glyphs)
            advance += glyph.xAdvance * pixelSize;
        float pen = label.anchorX;
        if (label.alignment == SceneTextAlignment::Center)
            pen -= advance * 0.5f;
        else if (label.alignment == SceneTextAlignment::Right)
            pen -= advance;
        float baseline = label.anchorY;
        if (label.verticalAnchor != VerticalAnchor::Baseline) {
            bool hasBounds = false;
            float edge = 0.0f;
            for (const SceneTextPositionedGlyph& positioned : run.glyphs) {
                const SceneTextGlyph* glyph = atlas.FindGlyph (positioned.glyphIndex);
                if (glyph == nullptr)
                    continue;
                const float value =
                    positioned.yOffset +
                    (label.verticalAnchor == VerticalAnchor::Top ? glyph->planeTop : glyph->planeBottom);
                edge = !hasBounds ? value
                                  : (label.verticalAnchor == VerticalAnchor::Top ? (std::max) (edge, value)
                                                                                 : (std::min) (edge, value));
                hasBounds = true;
            }
            if (hasBounds) {
                baseline += edge * pixelSize;
                if (label.verticalAnchor == VerticalAnchor::Bottom)
                    baseline -= 3.0f * (pixelSize / 18.0f);
            }
        }
        const uint32_t color = LinearAbgr (label.rgba);
        size_t emitted = 0;
        for (const SceneTextPositionedGlyph& positioned : run.glyphs) {
            const SceneTextGlyph* glyph = atlas.FindGlyph (positioned.glyphIndex);
            if (glyph == nullptr)
                continue;
            const float left = pen + (positioned.xOffset + glyph->planeLeft) * pixelSize;
            const float right = pen + (positioned.xOffset + glyph->planeRight) * pixelSize;
            const float top = baseline - (positioned.yOffset + glyph->planeTop) * pixelSize;
            const float bottom = baseline - (positioned.yOffset + glyph->planeBottom) * pixelSize;
            if (right >= 0.0f && left <= float (surfaceWidth) && bottom >= 0.0f && top <= float (surfaceHeight)) {
                AddQuad (vertices, left, top, right, bottom, glyph->atlasLeft / float (atlas.Width ()),
                         glyph->atlasBottom / float (atlas.Height ()), glyph->atlasRight / float (atlas.Width ()),
                         glyph->atlasTop / float (atlas.Height ()), color);
                ++emitted;
            }
            pen += positioned.xAdvance * pixelSize;
        }
        if (emitted > 0) {
            ++stats.labels;
            stats.glyphs += emitted;
        }
    }
    if (vertices.empty ())
        return;

    if (this->vertices == nullptr || vertices.size () > vertexCapacity) {
        this->vertices.Release ();
        vertexCapacity = std::max<size_t> (vertices.size () * 2, 1024);
        Diligent::BufferDesc desc;
        desc.Name = "Scene text vertices";
        desc.Size = Diligent::Uint64 (vertexCapacity * sizeof (SceneTextVertex));
        desc.Usage = Diligent::USAGE_DYNAMIC;
        desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
        device->CreateBuffer (desc, nullptr, &this->vertices);
        if (this->vertices == nullptr) {
            vertexCapacity = 0;
            return;
        }
    }
    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (this->vertices, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr)
        return;
    std::memcpy (mapped, vertices.data (), vertices.size () * sizeof (SceneTextVertex));
    context->UnmapBuffer (this->vertices, Diligent::MAP_WRITE);

    context->MapBuffer (constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr)
        return;
    auto* constants = static_cast<SceneTextConstants*> (mapped);
    constants->surface[0] = 1.0f / float (surfaceWidth);
    constants->surface[1] = 1.0f / float (surfaceHeight);
    constants->surface[2] = 1.0f / float (atlas.Width ());
    constants->surface[3] = 1.0f / float (atlas.Height ());
    constants->atlas[0] = SceneTextAtlas::kDistanceRangePixels;
    constants->atlas[1] = constants->atlas[2] = constants->atlas[3] = 0.0f;
    context->UnmapBuffer (this->constants, Diligent::MAP_WRITE);

    Diligent::IBuffer* buffers[] = { this->vertices };
    const Diligent::Uint64 offsets[] = { 0 };
    context->SetVertexBuffers (0, 1, buffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetPipelineState (pso);
    context->CommitShaderResources (srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DrawAttribs draw;
    draw.NumVertices = static_cast<Diligent::Uint32> (vertices.size ());
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->Draw (draw);
}

bool SceneTextLayer::IsReady () const
{
    return impl_->stats.ready;
}

SceneTextLayerStats SceneTextLayer::Stats () const
{
    return impl_->stats;
}

} // namespace geomsrv::archviz
