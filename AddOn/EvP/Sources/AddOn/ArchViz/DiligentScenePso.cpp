// The HDR resolve family: the pipeline states that read the offscreen scene
// colour back, and the offscreen targets themselves.
//
// ⚠️ THIS FILE IS THE SPLIT tools/quality/check_cpp.py ASKED FOR BY NAME. Its
// exception for DiligentScene.cpp said the natural next split was "a
// DiligentScenePso.cpp carrying the HDR/resolve PSO creation out of Init ...
// when the next PSO family arrives". RE51.C8's coverage-extract pipeline is
// that family, so the extraction landed with it. Nothing here changed shape on
// the way across: it is the same code, called from the same point in Init.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/DiligentShaders.hpp"

#include <Sampler.h>

namespace geomsrv {
namespace archviz {

bool DiligentScene::CreateResolvePipelines (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                            const Diligent::SamplerDesc& envSampler, std::string& error)
{
    // ---- the resolve PSO ----------------------------------------------------
    // Full-screen triangle, no depth; the HDR target is DYNAMIC because it
    // is recreated on resize.
    {
        Diligent::ShaderResourceVariableDesc resolveVariables[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_hdrColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_hdrCoverage", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_ssrColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferRoughness", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferNormal", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferAlbedo", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferMaterialData", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        };
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz HDR resolve PSO";
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        pci.PSODesc.ResourceLayout.Variables = resolveVariables;
        pci.PSODesc.ResourceLayout.NumVariables = _countof (resolveVariables);
        const Diligent::ImmutableSamplerDesc resolveSamplers[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_envMap_sampler", envSampler },
        };
        pci.PSODesc.ResourceLayout.ImmutableSamplers = resolveSamplers;
        pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (resolveSamplers);
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        gp.DepthStencilDesc.DepthEnable = Diligent::False;
        pci.pVS = impl_->fullScreenVs;
        pci.pPS = impl_->resolvePs;
        device->CreateGraphicsPipelineState (pci, &impl_->resolvePso);
        if (impl_->resolvePso == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(ArchViz HDR resolve) failed";
            return false;
        }
        if (Diligent::IShaderResourceVariable* cb =
                impl_->resolvePso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "ArchVizConstants"))
            cb->Set (impl_->constants);
        if (Diligent::ITextureView* envView = impl_->environment.ShaderView ()) {
            if (Diligent::IShaderResourceVariable* envVar =
                    impl_->resolvePso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_envMap"))
                envVar->Set (envView);
        }
        impl_->resolvePso->CreateShaderResourceBinding (&impl_->resolveSrb, true);
        if (impl_->resolveSrb == nullptr) {
            error = "Diligent CreateShaderResourceBinding(ArchViz HDR resolve) failed";
            return false;
        }
    }

    // ---- RE51.C8: the coverage extract PSO ----------------------------------
    // The same full-screen triangle as the resolve, writing into an
    // RGBA16_FLOAT target because that is the format DiligentFX's accumulation
    // buffers use -- see TemporalAntiAliasing.cpp, which creates them as
    // TEX_FORMAT_RGBA16_FLOAT unconditionally. A narrower coverage target would
    // be honest about carrying one channel and would then have to be converted
    // before TAA would take it.
    {
        Diligent::ShaderResourceVariableDesc coverageVariables[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_hdrColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        };
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz coverage extract PSO";
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        pci.PSODesc.ResourceLayout.Variables = coverageVariables;
        pci.PSODesc.ResourceLayout.NumVariables = _countof (coverageVariables);
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        gp.DepthStencilDesc.DepthEnable = Diligent::False;
        pci.pVS = impl_->fullScreenVs;
        pci.pPS = impl_->coveragePs;
        device->CreateGraphicsPipelineState (pci, &impl_->coveragePso);
        if (impl_->coveragePso == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(ArchViz coverage extract) failed";
            return false;
        }
        impl_->coveragePso->CreateShaderResourceBinding (&impl_->coverageSrb, true);
        if (impl_->coverageSrb == nullptr) {
            error = "Diligent CreateShaderResourceBinding(ArchViz coverage extract) failed";
            return false;
        }
    }
    return true;
}

bool DiligentScene::EnsureHdrTarget ()
{
    if (impl_ == nullptr || impl_->device == nullptr)
        return false;
    if (impl_->hdrWidth == impl_->viewportWidth && impl_->hdrHeight == impl_->viewportHeight &&
        impl_->hdrColorTexture != nullptr)
        return true;

    impl_->hdrColorTexture.Release ();
    impl_->hdrColorRTV = nullptr;
    impl_->hdrColorSRV = nullptr;

    if (impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return false;

    Diligent::TextureDesc td;
    td.Name = "ArchViz HDR scene colour";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = impl_->viewportWidth;
    td.Height = impl_->viewportHeight;
    td.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    td.MipLevels = 1;
    td.Usage = Diligent::USAGE_DEFAULT;
    td.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    td.ClearValue.SetColor (Diligent::TEX_FORMAT_RGBA16_FLOAT, 0.0f, 0.0f, 0.0f, 0.0f);

    impl_->device->CreateTexture (td, nullptr, &impl_->hdrColorTexture);
    if (impl_->hdrColorTexture == nullptr)
        return false;

    impl_->hdrColorRTV = impl_->hdrColorTexture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    impl_->hdrColorSRV = impl_->hdrColorTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    impl_->hdrWidth = impl_->viewportWidth;
    impl_->hdrHeight = impl_->viewportHeight;

    // ⚠️ RE-BIND THE RESOLVE SRB ON EVERY RESIZE. The HDR texture is DYNAMIC in
    // the resolve PSO's resource layout, so the SRB's variable survives the
    // resize -- but it points at the OLD texture's SRV, which was just released.
    // This is the same lesson as the G-buffer's resize rebind, and the same
    // shape: a DYNAMIC variable whose value is invalidated by the very event
    // that made it DYNAMIC in the first place.
    if (impl_->resolveSrb != nullptr) {
        if (Diligent::IShaderResourceVariable* var =
                impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_hdrColor"))
            var->Set (impl_->hdrColorSRV);
        if (Diligent::IShaderResourceVariable* var =
                impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_hdrCoverage"))
            var->Set (impl_->hdrColorSRV);
    }
    return true;
}

// RE51.C8. The intermediate the coverage extract writes into, sized with the
// HDR target it is derived from.
//
// ⚠️ SEPARATE FROM EnsureHdrTarget ON PURPOSE. This is only needed when TAA is
// actually running, and TAA is off by default; folding it into the HDR path
// would allocate a viewport-sized RGBA16_FLOAT texture for every Realistic
// frame whether or not anything reads it.
bool DiligentScene::EnsureCoverageTarget ()
{
    if (impl_ == nullptr || impl_->device == nullptr)
        return false;
    if (impl_->coverageWidth == impl_->viewportWidth && impl_->coverageHeight == impl_->viewportHeight &&
        impl_->coverageTexture != nullptr)
        return true;

    impl_->coverageTexture.Release ();
    impl_->coverageRTV = nullptr;
    impl_->coverageSRV = nullptr;
    impl_->coverageWidth = 0;
    impl_->coverageHeight = 0;

    if (impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return false;

    Diligent::TextureDesc td;
    td.Name = "ArchViz coverage (TAA-resolved)";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = impl_->viewportWidth;
    td.Height = impl_->viewportHeight;
    td.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    td.MipLevels = 1;
    td.Usage = Diligent::USAGE_DEFAULT;
    td.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    td.ClearValue.SetColor (Diligent::TEX_FORMAT_RGBA16_FLOAT, 0.0f, 0.0f, 0.0f, 0.0f);

    impl_->device->CreateTexture (td, nullptr, &impl_->coverageTexture);
    if (impl_->coverageTexture == nullptr)
        return false;

    impl_->coverageRTV = impl_->coverageTexture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    impl_->coverageSRV = impl_->coverageTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    impl_->coverageWidth = impl_->viewportWidth;
    impl_->coverageHeight = impl_->viewportHeight;
    return true;
}

} // namespace archviz
} // namespace geomsrv
