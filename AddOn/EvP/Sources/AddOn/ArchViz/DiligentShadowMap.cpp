#include "ArchViz/DiligentShadowMap.hpp"

#include "ArchViz/DiligentShaders.hpp"

#include <windows.h>
#include <d3d11.h>

#include "Components/interface/ShadowMapManager.hpp"
#include "Utilities/interface/DiligentFXShaderSourceStreamFactory.hpp"

#include <Buffer.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>
#include <InputLayout.h>
#include <MapHelper.hpp>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Sampler.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>
#include <TextureView.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace geomsrv {
namespace archviz {

using Diligent::RefCntAutoPtr;

namespace {

bool RequiresRebuild (const DiligentShadowSettings& a, const DiligentShadowSettings& b)
{
    return a.resolution != b.resolution || a.cascadeCount != b.cascadeCount || a.mode != b.mode;
}

} // namespace

bool CompileDiligentShadowPixelShader (Diligent::IRenderDevice* device, DiligentShadowMode mode,
                                       Diligent::IShader** shader, std::string& error)
{
    if (device == nullptr || shader == nullptr)
        return false;
    const char* const modeSources[] = { kArchVizShadowModePcf, kArchVizShadowModeVsm, kArchVizShadowModeEvsm2,
                                        kArchVizShadowModeEvsm4 };
    const int index = static_cast<int> (mode) - 1;
    if (index < 0 || index >= 4)
        return false;
    const std::string source = ArchVizShaderSource (modeSources[index], kArchVizEnvCommonPS, kArchVizMeshPS,
                                                    kArchVizMeshPSMain, kArchVizMeshPSMainTail);
    Diligent::ShaderCreateInfo createInfo;
    createInfo.Desc.Name = "ArchViz DiligentFX shadow mesh PS";
    createInfo.Desc.ShaderType = Diligent::SHADER_TYPE_PIXEL;
    createInfo.EntryPoint = "main";
    createInfo.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    createInfo.Source = source.c_str ();
    createInfo.SourceLength = source.size ();
    createInfo.pShaderSourceStreamFactory = &Diligent::DiligentFXShaderSourceStreamFactory::GetInstance ();
    device->CreateShader (createInfo, shader, nullptr);
    if (*shader == nullptr) {
        error = "Diligent CreateShader(ArchViz DiligentFX shadow mesh PS) failed";
        return false;
    }
    return true;
}

struct DiligentShadowMap::Impl {
    Diligent::IRenderDevice* device = nullptr;
    std::unique_ptr<Diligent::ShadowMapManager> manager;
    RefCntAutoPtr<Diligent::IBuffer> attribs;
    RefCntAutoPtr<Diligent::IPipelineState> pso;
    RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    RefCntAutoPtr<Diligent::ISampler> comparisonSampler;
    RefCntAutoPtr<Diligent::ISampler> filterableSampler;
    Diligent::ShadowMapAttribs shadowAttribs {};
    DiligentShadowSettings settings;
    bool ready = false;
    bool fitted = false;
};

DiligentShadowMap::DiligentShadowMap () : impl_ (new Impl ())
{
}

DiligentShadowMap::~DiligentShadowMap ()
{
    Shutdown ();
    delete impl_;
}

bool DiligentShadowMap::Init (Diligent::IRenderDevice* device, Diligent::IShader* shadowVs,
                              Diligent::IBuffer* sceneConstants, const void* inputLayout, uint32_t layoutElementCount,
                              const DiligentShadowSettings& settings, std::string& error)
{
    if (device == nullptr || shadowVs == nullptr || sceneConstants == nullptr || inputLayout == nullptr ||
        layoutElementCount == 0) {
        error = "DiligentShadowMap::Init got a null device, shader, constants or layout";
        return false;
    }

    impl_->device = device;

    Diligent::BufferDesc bufferDesc;
    bufferDesc.Name = "ArchViz DiligentFX shadow attributes";
    bufferDesc.Size = sizeof (Diligent::ShadowMapAttribs);
    bufferDesc.Usage = Diligent::USAGE_DYNAMIC;
    bufferDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    bufferDesc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (bufferDesc, nullptr, &impl_->attribs);
    if (impl_->attribs == nullptr) {
        error = "Diligent CreateBuffer(ArchViz shadow attributes) failed";
        return false;
    }

    Diligent::GraphicsPipelineStateCreateInfo pipelineInfo;
    pipelineInfo.PSODesc.Name = "ArchViz cascaded shadow depth PSO";
    Diligent::GraphicsPipelineDesc& pipeline = pipelineInfo.GraphicsPipeline;
    pipeline.NumRenderTargets = 0;
    pipeline.DSVFormat = Diligent::TEX_FORMAT_D32_FLOAT;
    pipeline.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    pipeline.DepthStencilDesc.DepthEnable = Diligent::True;
    pipeline.DepthStencilDesc.DepthWriteEnable = Diligent::True;
    pipeline.InputLayout.LayoutElements = static_cast<const Diligent::LayoutElement*> (inputLayout);
    pipeline.InputLayout.NumElements = layoutElementCount;
    pipelineInfo.pVS = shadowVs;
    pipelineInfo.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    device->CreateGraphicsPipelineState (pipelineInfo, &impl_->pso);
    if (impl_->pso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(ArchViz cascaded shadow depth) failed";
        return false;
    }

    Diligent::IShaderResourceVariable* variable =
        impl_->pso->GetStaticVariableByName (Diligent::SHADER_TYPE_VERTEX, "ArchVizConstants");
    if (variable == nullptr) {
        error = "the shadow vertex shader does not expose ArchVizConstants";
        return false;
    }
    variable->Set (sceneConstants);
    impl_->pso->CreateShaderResourceBinding (&impl_->srb, true);
    if (impl_->srb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(ArchViz cascaded shadow depth) failed";
        return false;
    }

    Diligent::SamplerDesc comparison;
    comparison.MinFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
    comparison.MagFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
    comparison.MipFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
    comparison.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    comparison.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    comparison.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    comparison.ComparisonFunc = Diligent::COMPARISON_FUNC_LESS;
    device->CreateSampler (comparison, &impl_->comparisonSampler);

    Diligent::SamplerDesc filterable;
    filterable.MinFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    filterable.MagFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    filterable.MipFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
    filterable.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    filterable.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    filterable.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    filterable.MaxAnisotropy = 4;
    device->CreateSampler (filterable, &impl_->filterableSampler);
    if (impl_->comparisonSampler == nullptr || impl_->filterableSampler == nullptr) {
        error = "Diligent CreateSampler(ArchViz shadows) failed";
        return false;
    }

    if (!SetSettings (settings, error))
        return false;
    impl_->ready = true;
    return true;
}

bool DiligentShadowMap::SetSettings (const DiligentShadowSettings& requested, std::string& error)
{
    if (impl_->device == nullptr) {
        error = "DiligentShadowMap::SetSettings called before Init";
        return false;
    }

    DiligentShadowSettings settings = requested;
    settings.resolution = (std::max) (512u, (std::min) (settings.resolution, 4096u));
    settings.cascadeCount = (std::max) (1u, (std::min) (settings.cascadeCount, 8u));
    settings.fixedFilterSize =
        settings.fixedFilterSize == 0
            ? 0
            : (settings.fixedFilterSize <= 2
                   ? 2
                   : (settings.fixedFilterSize <= 3 ? 3 : (settings.fixedFilterSize <= 5 ? 5 : 7)));
    settings.partitioningFactor = (std::max) (0.0f, (std::min) (settings.partitioningFactor, 1.0f));

    if (impl_->manager == nullptr || RequiresRebuild (settings, impl_->settings)) {
        auto manager = std::make_unique<Diligent::ShadowMapManager> ();
        Diligent::ShadowMapManager::InitInfo init;
        init.Format = Diligent::TEX_FORMAT_D32_FLOAT;
        init.Resolution = settings.resolution;
        init.NumCascades = settings.cascadeCount;
        init.ShadowMode = static_cast<int> (settings.mode);
        init.Is32BitFilterableFmt = true;
        init.pComparisonSampler = impl_->comparisonSampler;
        init.pFilterableShadowMapSampler = impl_->filterableSampler;
        manager->Initialize (impl_->device, nullptr, init);
        if (manager->GetSRV () == nullptr) {
            error = "DiligentFX ShadowMapManager created no shadow-map SRV";
            return false;
        }
        impl_->manager = std::move (manager);
    }

    impl_->settings = settings;
    impl_->fitted = false;
    return true;
}

bool DiligentShadowMap::Prepare (Diligent::IDeviceContext* context, const float view[16], const float projection[16],
                                 const float towardSun[3])
{
    impl_->fitted = false;
    if (context == nullptr || !impl_->ready || impl_->manager == nullptr || view == nullptr || projection == nullptr ||
        towardSun == nullptr)
        return false;

    const float length =
        std::sqrt (towardSun[0] * towardSun[0] + towardSun[1] * towardSun[1] + towardSun[2] * towardSun[2]);
    if (!(length > 1e-6f))
        return false;

    Diligent::float4x4 cameraView;
    Diligent::float4x4 cameraProjection;
    std::memcpy (&cameraView, view, sizeof (cameraView));
    std::memcpy (&cameraProjection, projection, sizeof (cameraProjection));
    const Diligent::float3 lightDirection { -towardSun[0] / length, -towardSun[1] / length, -towardSun[2] / length };

    impl_->shadowAttribs = Diligent::ShadowMapAttribs {};
    // The PCF shader uses DiligentFX's world-constant kernel so its size remains
    // meaningful as cascades change scale. Filterable modes use the conversion
    // pass's fixed kernel.
    impl_->shadowAttribs.iFixedFilterSize =
        impl_->settings.mode == DiligentShadowMode::Pcf ? 0 : impl_->settings.fixedFilterSize;
    impl_->shadowAttribs.fFilterWorldSize = impl_->settings.filterWorldSize;
    impl_->shadowAttribs.fReceiverPlaneDepthBiasClamp = impl_->settings.receiverPlaneBiasClamp;
    impl_->shadowAttribs.fFixedDepthBias = impl_->settings.fixedDepthBias;
    impl_->shadowAttribs.fCascadeTransitionRegion = impl_->settings.cascadeTransition;
    impl_->shadowAttribs.fEVSMPositiveExponent = impl_->settings.evsmPositiveExponent;
    impl_->shadowAttribs.fEVSMNegativeExponent = impl_->settings.evsmNegativeExponent;
    impl_->shadowAttribs.fVSMLightBleedingReduction = impl_->settings.lightBleedingReduction;
    impl_->shadowAttribs.fVSMBias = impl_->settings.vsmBias;
    impl_->shadowAttribs.iMaxAnisotropy = 4;
    impl_->shadowAttribs.bVisualizeCascades = impl_->settings.visualizeCascades ? 1 : 0;
    impl_->shadowAttribs.bVisualizeShadowing = impl_->settings.shadowsOnly ? 1 : 0;

    Diligent::ShadowMapManager::DistributeCascadeInfo distribute;
    distribute.pCameraView = &cameraView;
    distribute.pCameraProj = &cameraProjection;
    distribute.pLightDir = &lightDirection;
    distribute.SnapCascades = impl_->settings.snapCascades;
    distribute.StabilizeExtents = impl_->settings.stabilizeExtents;
    distribute.EqualizeExtents = impl_->settings.equalizeExtents;
    distribute.fPartitioningFactor = impl_->settings.partitioningFactor;
    distribute.UseRightHandedLightViewTransform = true;
    // ArchViz uploads row-major matrices into default column-major HLSL and uses
    // mul(matrix, vector), the same convention as its camera matrices.
    distribute.PackMatrixRowMajor = true;
    impl_->manager->DistributeCascades (distribute, impl_->shadowAttribs);

    if (Diligent::MapHelper<Diligent::ShadowMapAttribs> mapped { context, impl_->attribs, Diligent::MAP_WRITE,
                                                                 Diligent::MAP_FLAG_DISCARD })
        *mapped = impl_->shadowAttribs;
    else
        return false;

    impl_->fitted = true;
    return true;
}

void DiligentShadowMap::BeginCascade (Diligent::IDeviceContext* context, uint32_t cascade)
{
    if (context == nullptr || !impl_->fitted || cascade >= impl_->settings.cascadeCount)
        return;
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::ITextureView* dsv = impl_->manager->GetCascadeDSV (cascade);
    context->SetRenderTargets (0, nullptr, dsv, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil (dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::Viewport viewport;
    viewport.Width = float (impl_->settings.resolution);
    viewport.Height = float (impl_->settings.resolution);
    viewport.MaxDepth = 1.0f;
    context->SetViewports (1, &viewport, impl_->settings.resolution, impl_->settings.resolution);
    context->SetPipelineState (impl_->pso);
    context->CommitShaderResources (impl_->srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void DiligentShadowMap::End (Diligent::IDeviceContext* context)
{
    if (context == nullptr || !impl_->fitted)
        return;
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (impl_->settings.mode != DiligentShadowMode::Pcf)
        impl_->manager->ConvertToFilterable (context, impl_->shadowAttribs);
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void DiligentShadowMap::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->attribs.Release ();
    impl_->comparisonSampler.Release ();
    impl_->filterableSampler.Release ();
    impl_->manager.reset ();
    impl_->device = nullptr;
    impl_->ready = false;
    impl_->fitted = false;
}

bool DiligentShadowMap::IsReady () const
{
    return impl_ != nullptr && impl_->ready;
}
bool DiligentShadowMap::IsFitted () const
{
    return impl_ != nullptr && impl_->fitted;
}
uint32_t DiligentShadowMap::Resolution () const
{
    return impl_ != nullptr ? impl_->settings.resolution : 0;
}
uint32_t DiligentShadowMap::CascadeCount () const
{
    return impl_ != nullptr ? impl_->settings.cascadeCount : 0;
}
const DiligentShadowSettings& DiligentShadowMap::Settings () const
{
    return impl_->settings;
}

float DiligentShadowMap::FirstCascadeTexelMetres () const
{
    if (impl_ == nullptr || !impl_->fitted || impl_->settings.resolution == 0)
        return 0.0f;
    const float scale = impl_->shadowAttribs.Cascades[0].f4LightSpaceScale.x;
    return std::abs (scale) > 1e-8f ? (2.0f / std::abs (scale)) / float (impl_->settings.resolution) : 0.0f;
}

void DiligentShadowMap::CopyCascadeViewProjection (uint32_t cascade, float out[16]) const
{
    if (out == nullptr || impl_ == nullptr || impl_->manager == nullptr || cascade >= impl_->settings.cascadeCount)
        return;
    const Diligent::float4x4& matrix = impl_->manager->GetCascadeTransform (cascade).WorldToLightProjSpace;
    std::memcpy (out, &matrix, sizeof (matrix));
}

Diligent::IBuffer* DiligentShadowMap::AttribsBuffer () const
{
    return impl_ != nullptr ? impl_->attribs.RawPtr () : nullptr;
}
Diligent::ITextureView* DiligentShadowMap::ShaderView () const
{
    return impl_ != nullptr && impl_->manager != nullptr ? impl_->manager->GetSRV () : nullptr;
}
Diligent::ITextureView* DiligentShadowMap::FilterableShaderView () const
{
    return impl_ != nullptr && impl_->manager != nullptr ? impl_->manager->GetFilterableSRV () : nullptr;
}
Diligent::IShaderResourceBinding* DiligentShadowMap::Srb () const
{
    return impl_ != nullptr ? impl_->srb.RawPtr () : nullptr;
}

} // namespace archviz
} // namespace geomsrv
