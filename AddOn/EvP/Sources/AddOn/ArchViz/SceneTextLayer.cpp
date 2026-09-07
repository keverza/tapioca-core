#include "ArchViz/SceneTextLayer.hpp"

#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SceneTextAtlas.hpp"
#include "ArchViz/SceneTextAtlasCache.hpp"
#include "ArchViz/SceneTextLayoutCache.hpp"

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
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>

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
    uint32_t haloRgba = 0;
    float haloWidthPixels = 0.0f;
    bool backgroundPanel = false;
};

struct SceneTextVertex {
    float position[2];
    float uv[2];
    uint32_t fillAbgr;
    uint32_t haloAbgr;
    float haloWidthPixels;
};

struct SceneTextConstants {
    float surface[4]; // xy = inverse surface, zw = inverse atlas
    float atlas[4];   // x = distance range in atlas pixels
};

constexpr const char* kSceneTextVS = R"hlsl(
cbuffer SceneTextConstants { float4 g_surface; float4 g_atlasParams; };
struct VSInput { float2 position : ATTRIB0; float2 uv : ATTRIB1; float4 fillColor : ATTRIB2;
                 float4 haloColor : ATTRIB3; float haloWidth : ATTRIB4; };
struct PSInput { float4 position : SV_POSITION; float2 uv : TEX_COORD; float4 fillColor : COLOR0;
                 float4 haloColor : COLOR1; float haloWidth : TEX_COORD1; };
void main (in VSInput input, out PSInput output)
{
    output.position = float4(input.position.x*g_surface.x*2.0-1.0,
                             1.0-input.position.y*g_surface.y*2.0, 0.0, 1.0);
    output.uv = input.uv;
    output.fillColor = input.fillColor;
    output.haloColor = input.haloColor;
    output.haloWidth = input.haloWidth;
}
)hlsl";

constexpr const char* kSceneTextPS = R"hlsl(
cbuffer SceneTextConstants { float4 g_surface; float4 g_atlasParams; };
Texture2D g_atlas;
SamplerState g_atlas_sampler;
struct PSInput { float4 position : SV_POSITION; float2 uv : TEX_COORD; float4 fillColor : COLOR0;
                 float4 haloColor : COLOR1; float haloWidth : TEX_COORD1; };
float Median(float3 value) { return max(min(value.r, value.g), min(max(value.r, value.g), value.b)); }
float4 main (PSInput input) : SV_TARGET
{
    if (input.uv.x < 0.0)
        return float4(input.fillColor.rgb*input.fillColor.a, input.fillColor.a);
    float4 distance = g_atlas.Sample(g_atlas_sampler, input.uv);
    float2 unitRange = g_atlasParams.x*g_surface.zw;
    float2 screenTexelRange = 1.0/max(fwidth(input.uv), float2(1e-6, 1e-6));
    float screenRange = max(0.5*dot(unitRange, screenTexelRange), 1.0);
    float fillCoverage = saturate(screenRange*(Median(distance.rgb)-0.5)+0.5);
    float haloCoverage = saturate(screenRange*(distance.a-0.5)+0.5+max(input.haloWidth, 0.0));
    float fillAlpha = fillCoverage*input.fillColor.a;
    float haloAlpha = haloCoverage*input.haloColor.a*(1.0-fillAlpha);
    return float4(input.fillColor.rgb*fillAlpha+input.haloColor.rgb*haloAlpha, fillAlpha+haloAlpha);
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
              float v0, float u1, float v1, uint32_t fillColor, uint32_t haloColor = 0,
              float haloWidthPixels = 0.0f)
{
    const SceneTextVertex quad[6] = {
        { { left, top }, { u0, v1 }, fillColor, haloColor, haloWidthPixels },
        { { right, top }, { u1, v1 }, fillColor, haloColor, haloWidthPixels },
        { { right, bottom }, { u1, v0 }, fillColor, haloColor, haloWidthPixels },
        { { left, top }, { u0, v1 }, fillColor, haloColor, haloWidthPixels },
        { { right, bottom }, { u1, v0 }, fillColor, haloColor, haloWidthPixels },
        { { left, bottom }, { u0, v0 }, fillColor, haloColor, haloWidthPixels },
    };
    vertices.insert (vertices.end (), std::begin (quad), std::end (quad));
}

} // namespace

struct SceneTextLayer::Impl {
    struct DynamicPage {
        std::shared_ptr<const SceneTextAtlasPage> atlas;
        Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
        uint64_t lastAccess = 0;
    };

    SceneTextAtlas atlas;
    SceneTextLayoutCache layoutCache;
    SceneTextAtlasCache atlasCache;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> constants;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> vertices;
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    std::vector<DynamicPage> dynamicPages;
    std::unordered_set<uint32_t> suppressedGlyphs;
    size_t vertexCapacity = 0;
    uint64_t pageAccessSequence = 0;
    SceneTextLayerStats stats;

    void UploadReadyPages (Diligent::IRenderDevice* device);
    const SceneTextGlyph* FindGlyphExact (uint32_t glyphIndex, size_t& pageIndex);
    bool DrawPrepared (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                       const std::vector<PreparedSceneTextLabel>& labels, uint32_t surfaceWidth, uint32_t surfaceHeight,
                        bool requireAllReady);
};

void SceneTextLayer::Impl::UploadReadyPages (Diligent::IRenderDevice* device)
{
    constexpr size_t kMaximumDynamicPages = 4;
    while (auto page = atlasCache.TakeReady ()) {
        const auto started = std::chrono::steady_clock::now ();
        DynamicPage uploaded;
        uploaded.atlas = std::move (page);
        Diligent::TextureDesc desc;
        desc.Name = "Dynamic scene text linear MTSDF atlas";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Width = static_cast<Diligent::Uint32> (uploaded.atlas->Width ());
        desc.Height = static_cast<Diligent::Uint32> (uploaded.atlas->Height ());
        desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
        desc.Usage = Diligent::USAGE_IMMUTABLE;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        Diligent::TextureSubResData level;
        level.pData = uploaded.atlas->Pixels ().data ();
        level.Stride = Diligent::Uint64 (uploaded.atlas->Width () * 4);
        Diligent::TextureData initial;
        initial.pSubResources = &level;
        initial.NumSubresources = 1;
        device->CreateTexture (desc, &initial, &uploaded.texture);
        if (uploaded.texture == nullptr) {
            suppressedGlyphs.insert (uploaded.atlas->GlyphIds ().begin (), uploaded.atlas->GlyphIds ().end ());
            ++stats.atlasUploadFailures;
            continue;
        }
        pso->CreateShaderResourceBinding (&uploaded.srb, true);
        auto* variable = uploaded.srb != nullptr
                             ? uploaded.srb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_atlas")
                             : nullptr;
        if (variable == nullptr) {
            suppressedGlyphs.insert (uploaded.atlas->GlyphIds ().begin (), uploaded.atlas->GlyphIds ().end ());
            ++stats.atlasUploadFailures;
            continue;
        }
        variable->Set (uploaded.texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        if (dynamicPages.size () >= kMaximumDynamicPages) {
            const auto victim = std::min_element (dynamicPages.begin (), dynamicPages.end (), [] (const auto& left,
                                                                                                  const auto& right) {
                return left.lastAccess < right.lastAccess;
            });
            suppressedGlyphs.insert (victim->atlas->GlyphIds ().begin (), victim->atlas->GlyphIds ().end ());
            stats.atlasBytes -= victim->atlas->Pixels ().size ();
            dynamicPages.erase (victim);
            ++stats.atlasEvictions;
        }
        uploaded.lastAccess = ++pageAccessSequence;
        stats.atlasBytes += uploaded.atlas->Pixels ().size ();
        dynamicPages.push_back (std::move (uploaded));
        ++stats.atlasUploads;
        stats.atlasUploadMicroseconds += static_cast<uint64_t> (
            std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - started)
                .count ());
    }
    stats.atlasPages = static_cast<uint32_t> (1 + dynamicPages.size ());
}

const SceneTextGlyph* SceneTextLayer::Impl::FindGlyphExact (uint32_t glyphIndex, size_t& pageIndex)
{
    if (const SceneTextGlyph* glyph = atlas.FindGlyphExact (glyphIndex)) {
        pageIndex = 0;
        return glyph;
    }
    for (size_t index = 0; index < dynamicPages.size (); ++index) {
        if (const SceneTextGlyph* glyph = dynamicPages[index].atlas->FindGlyphExact (glyphIndex)) {
            dynamicPages[index].lastAccess = ++pageAccessSequence;
            pageIndex = index + 1;
            return glyph;
        }
    }
    return nullptr;
}

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
    if (!LoadFontResource (fontBytes, error) || !impl_->layoutCache.Start (fontBytes.data (), fontBytes.size (), error))
        return false;
    const auto seedRun = impl_->layoutCache.WaitFor (SceneTextSeedText (), SceneTextDirection::Auto, error);
    if (seedRun == nullptr || !impl_->atlas.Build (fontBytes.data (), fontBytes.size (), *seedRun, error))
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
        { 3, 0, 4, Diligent::VT_UINT8, Diligent::True },
        { 4, 0, 1, Diligent::VT_FLOAT32, Diligent::False },
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
    if (!impl_->atlasCache.Start (fontBytes.data (), fontBytes.size (), error))
        return false;
    impl_->stats.ready = true;
    impl_->stats.atlasWidth = static_cast<uint32_t> (impl_->atlas.Width ());
    impl_->stats.atlasHeight = static_cast<uint32_t> (impl_->atlas.Height ());
    impl_->stats.atlasBytes = impl_->atlas.Pixels ().size ();
    impl_->stats.atlasPages = 1;
    return true;
}

void SceneTextLayer::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->atlasCache.Stop ();
    impl_->layoutCache.Stop ();
    impl_->dynamicPages.clear ();
    impl_->suppressedGlyphs.clear ();
    impl_->vertices.Release ();
    impl_->constants.Release ();
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->texture.Release ();
    impl_->vertexCapacity = 0;
    impl_->pageAccessSequence = 0;
    impl_->stats = {};
}

void SceneTextLayer::Draw (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                           const std::vector<SceneTextLabel>& labels, const float viewProj[16], uint32_t surfaceWidth,
                           uint32_t surfaceHeight, float dpiScale)
{
    impl_->stats.labels = impl_->stats.glyphs = impl_->stats.drawCalls = 0;
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
                               label.rgba, label.alignment, VerticalAnchor::Baseline, label.haloRgba,
                               std::clamp (label.haloWidthPixels * dpiScale, 0.0f, 8.0f) });
    }
    impl_->DrawPrepared (device, context, prepared, surfaceWidth, surfaceHeight, false);
}

bool SceneTextLayer::DrawProjected (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                    const std::vector<ScreenLabel>& labels, uint32_t surfaceWidth,
                                    uint32_t surfaceHeight, float dpiScale)
{
    impl_->stats.labels = impl_->stats.glyphs = impl_->stats.drawCalls = 0;
    if (!impl_->stats.ready || device == nullptr || context == nullptr || surfaceWidth == 0 || surfaceHeight == 0 ||
        !std::isfinite (dpiScale) || dpiScale <= 0.0f)
        return false;
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
                               label.centered ? VerticalAnchor::Top : VerticalAnchor::Bottom, label.haloRgba,
                              std::clamp (label.haloWidthPixels, 0.0f, 8.0f), label.backgroundPanel });
    }
    return impl_->DrawPrepared (device, context, prepared, surfaceWidth, surfaceHeight, true);
}

bool SceneTextLayer::Impl::DrawPrepared (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                          const std::vector<PreparedSceneTextLabel>& labels, uint32_t surfaceWidth,
                                          uint32_t surfaceHeight, bool requireAllReady)
{
    stats.labels = stats.glyphs = stats.drawCalls = 0;
    struct ResolvedLabel {
        const PreparedSceneTextLabel* label = nullptr;
        std::shared_ptr<const SceneTextGlyphRun> run;
    };
    std::vector<ResolvedLabel> resolved;
    resolved.reserve (labels.size ());
    for (const PreparedSceneTextLabel& label : labels) {
        if (label.text == nullptr)
            continue;
        auto run = layoutCache.FindOrRequest (*label.text, SceneTextDirection::Auto);
        if (run == nullptr) {
            if (requireAllReady)
                return false;
            continue;
        }
        resolved.push_back ({ &label, std::move (run) });
    }

    UploadReadyPages (device);
    std::vector<uint32_t> missingGlyphs;
    for (const ResolvedLabel& resolvedLabel : resolved) {
        for (const SceneTextPositionedGlyph& positioned : resolvedLabel.run->glyphs) {
            size_t pageIndex = 0;
            if (FindGlyphExact (positioned.glyphIndex, pageIndex) == nullptr &&
                suppressedGlyphs.find (positioned.glyphIndex) == suppressedGlyphs.end ())
                missingGlyphs.push_back (positioned.glyphIndex);
        }
    }
    std::sort (missingGlyphs.begin (), missingGlyphs.end ());
    missingGlyphs.erase (std::unique (missingGlyphs.begin (), missingGlyphs.end ()), missingGlyphs.end ());
    for (size_t begin = 0; begin < missingGlyphs.size (); begin += SceneTextAtlasPage::kMaximumGlyphs) {
        const size_t end = (std::min) (begin + SceneTextAtlasPage::kMaximumGlyphs, missingGlyphs.size ());
        atlasCache.Request (std::vector<uint32_t> (missingGlyphs.begin () + begin, missingGlyphs.begin () + end));
    }
    if (requireAllReady && !missingGlyphs.empty ())
        return false;

    struct VertexBatch {
        size_t pageIndex = 0;
        std::vector<SceneTextVertex> vertices;
    };
    std::vector<VertexBatch> batches;
    const auto resolveGlyph = [&] (uint32_t glyphIndex, size_t& pageIndex) {
        if (const SceneTextGlyph* glyph = FindGlyphExact (glyphIndex, pageIndex))
            return glyph;
        pageIndex = 0;
        return atlas.FindGlyph (glyphIndex);
    };
    for (const ResolvedLabel& resolvedLabel : resolved) {
        const PreparedSceneTextLabel& label = *resolvedLabel.label;
        const SceneTextGlyphRun& run = *resolvedLabel.run;
        const float pixelSize = label.sizePixels;
        float advance = 0.0f;
        const size_t glyphCount = (std::min) (run.glyphs.size (), kMaximumCodepointsPerLabel);
        for (size_t index = 0; index < glyphCount; ++index) {
            const SceneTextPositionedGlyph& glyph = run.glyphs[index];
            advance += glyph.xAdvance * pixelSize;
        }
        float pen = label.anchorX;
        if (label.alignment == SceneTextAlignment::Center)
            pen -= advance * 0.5f;
        else if (label.alignment == SceneTextAlignment::Right)
            pen -= advance;
        float baseline = label.anchorY;
        if (label.verticalAnchor != VerticalAnchor::Baseline) {
            bool hasBounds = false;
            float edge = 0.0f;
            for (size_t index = 0; index < glyphCount; ++index) {
                const SceneTextPositionedGlyph& positioned = run.glyphs[index];
                size_t pageIndex = 0;
                const SceneTextGlyph* glyph = resolveGlyph (positioned.glyphIndex, pageIndex);
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
        const uint32_t haloColor = LinearAbgr (label.haloRgba);
        std::vector<VertexBatch> labelBatches;
        size_t emitted = 0;
        float boundsLeft = 0.0f, boundsTop = 0.0f, boundsRight = 0.0f, boundsBottom = 0.0f;
        for (size_t index = 0; index < glyphCount; ++index) {
            const SceneTextPositionedGlyph& positioned = run.glyphs[index];
            size_t pageIndex = 0;
            const SceneTextGlyph* glyph = resolveGlyph (positioned.glyphIndex, pageIndex);
            if (glyph == nullptr)
                continue;
            const float atlasWidth = pageIndex == 0 ? float (atlas.Width ())
                                                    : float (dynamicPages[pageIndex - 1].atlas->Width ());
            const float atlasHeight = pageIndex == 0 ? float (atlas.Height ())
                                                     : float (dynamicPages[pageIndex - 1].atlas->Height ());
            const float left = pen + (positioned.xOffset + glyph->planeLeft) * pixelSize;
            const float right = pen + (positioned.xOffset + glyph->planeRight) * pixelSize;
            const float top = baseline - (positioned.yOffset + glyph->planeTop) * pixelSize;
            const float bottom = baseline - (positioned.yOffset + glyph->planeBottom) * pixelSize;
            if (right >= 0.0f && left <= float (surfaceWidth) && bottom >= 0.0f && top <= float (surfaceHeight)) {
                if (labelBatches.empty () || labelBatches.back ().pageIndex != pageIndex)
                    labelBatches.push_back ({ pageIndex, {} });
                AddQuad (labelBatches.back ().vertices, left, top, right, bottom, glyph->atlasLeft / atlasWidth,
                          glyph->atlasBottom / atlasHeight, glyph->atlasRight / atlasWidth,
                          glyph->atlasTop / atlasHeight, color, haloColor, label.haloWidthPixels);
                if (emitted == 0) {
                    boundsLeft = left;
                    boundsTop = top;
                    boundsRight = right;
                    boundsBottom = bottom;
                }
                else {
                    boundsLeft = (std::min) (boundsLeft, left);
                    boundsTop = (std::min) (boundsTop, top);
                    boundsRight = (std::max) (boundsRight, right);
                    boundsBottom = (std::max) (boundsBottom, bottom);
                }
                ++emitted;
            }
            pen += positioned.xAdvance * pixelSize;
        }
        if (emitted > 0) {
            if (label.backgroundPanel) {
                const float scale = pixelSize / 18.0f;
                VertexBatch panel;
                AddQuad (panel.vertices, boundsLeft - 3.0f * scale, boundsTop - 2.0f * scale,
                         boundsRight + 3.0f * scale,
                         boundsBottom + 2.0f * scale, -1.0f, -1.0f, -1.0f, -1.0f, LinearAbgr (0xFFFFFFE0u));
                batches.push_back (std::move (panel));
            }
            for (VertexBatch& batch : labelBatches) {
                if (!batches.empty () && batches.back ().pageIndex == batch.pageIndex)
                    batches.back ().vertices.insert (batches.back ().vertices.end (), batch.vertices.begin (),
                                                      batch.vertices.end ());
                else
                    batches.push_back (std::move (batch));
            }
            ++stats.labels;
            stats.glyphs += emitted;
        }
    }
    size_t vertexCount = 0;
    for (const VertexBatch& batch : batches)
        vertexCount += batch.vertices.size ();
    if (vertexCount == 0)
        return true;

    if (this->vertices == nullptr || vertexCount > vertexCapacity) {
        this->vertices.Release ();
        vertexCapacity = std::max<size_t> (vertexCount * 2, 1024);
        Diligent::BufferDesc desc;
        desc.Name = "Scene text vertices";
        desc.Size = Diligent::Uint64 (vertexCapacity * sizeof (SceneTextVertex));
        desc.Usage = Diligent::USAGE_DYNAMIC;
        desc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
        device->CreateBuffer (desc, nullptr, &this->vertices);
        if (this->vertices == nullptr) {
            vertexCapacity = 0;
            return false;
        }
    }
    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (this->vertices, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped == nullptr)
        return false;
    auto* destination = static_cast<SceneTextVertex*> (mapped);
    for (const VertexBatch& batch : batches) {
        if (!batch.vertices.empty ()) {
            std::memcpy (destination, batch.vertices.data (), batch.vertices.size () * sizeof (SceneTextVertex));
            destination += batch.vertices.size ();
        }
    }
    context->UnmapBuffer (this->vertices, Diligent::MAP_WRITE);

    Diligent::IBuffer* buffers[] = { this->vertices };
    const Diligent::Uint64 offsets[] = { 0 };
    context->SetVertexBuffers (0, 1, buffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetPipelineState (pso);
    Diligent::Uint32 startVertex = 0;
    const auto drawBatch = [&] (const std::vector<SceneTextVertex>& batch, size_t pageIndex) {
        if (batch.empty ())
            return true;
        const float atlasWidth = pageIndex == 0 ? float (atlas.Width ())
                                                : float (dynamicPages[pageIndex - 1].atlas->Width ());
        const float atlasHeight = pageIndex == 0 ? float (atlas.Height ())
                                                 : float (dynamicPages[pageIndex - 1].atlas->Height ());
        context->MapBuffer (constants, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
        if (mapped == nullptr)
            return false;
        auto* values = static_cast<SceneTextConstants*> (mapped);
        values->surface[0] = 1.0f / float (surfaceWidth);
        values->surface[1] = 1.0f / float (surfaceHeight);
        values->surface[2] = 1.0f / atlasWidth;
        values->surface[3] = 1.0f / atlasHeight;
        values->atlas[0] = SceneTextAtlas::kDistanceRangePixels;
        values->atlas[1] = values->atlas[2] = values->atlas[3] = 0.0f;
        context->UnmapBuffer (constants, Diligent::MAP_WRITE);
        context->CommitShaderResources (pageIndex == 0 ? srb : dynamicPages[pageIndex - 1].srb,
                                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawAttribs draw;
        draw.NumVertices = static_cast<Diligent::Uint32> (batch.size ());
        draw.StartVertexLocation = startVertex;
        draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw (draw);
        startVertex += draw.NumVertices;
        ++stats.drawCalls;
        return true;
    };
    for (const VertexBatch& batch : batches) {
        if (!drawBatch (batch.vertices, batch.pageIndex))
            return false;
    }
    return true;
}

bool SceneTextLayer::IsReady () const
{
    return impl_->stats.ready;
}

SceneTextLayerStats SceneTextLayer::Stats () const
{
    SceneTextLayerStats stats = impl_->stats;
    const SceneTextAtlasCacheStats cache = impl_->atlasCache.Stats ();
    stats.pendingGlyphs = static_cast<uint32_t> (cache.pending);
    stats.stagingBytes = cache.stagingBytes;
    stats.atlasMisses = cache.misses;
    stats.atlasRejected = cache.rejected;
    stats.atlasGenerationFailures = cache.failures;
    stats.atlasGenerationMicroseconds = cache.generationMicroseconds;
    return stats;
}

} // namespace geomsrv::archviz
