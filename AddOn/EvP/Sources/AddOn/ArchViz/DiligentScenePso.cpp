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

bool DiligentScene::CreateSemanticWirePipeline (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                                uint32_t depthBufferFormat, std::string& error)
{
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },
    };
    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz face-aware tessellated wire PSO";
    Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_SOLID;
    gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
    gp.DepthStencilDesc.DepthEnable = Diligent::True;
    gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
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

    pci.pVS = impl_->wireVs;
    pci.pHS = impl_->wireHs;
    pci.pDS = impl_->wireDs;
    pci.pGS = impl_->wireGs;
    pci.pPS = impl_->wirePs;
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    const Diligent::ShaderResourceVariableDesc variables[] = {
        { Diligent::SHADER_TYPE_HULL, "g_wirePatchFlags", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    pci.PSODesc.ResourceLayout.Variables = variables;
    pci.PSODesc.ResourceLayout.NumVariables = _countof (variables);
    device->CreateGraphicsPipelineState (pci, &impl_->semanticWirePso);
    if (impl_->semanticWirePso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(face-aware tessellated wire) failed";
        return false;
    }

    const Diligent::SHADER_TYPE stages[] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_HULL,
                                             Diligent::SHADER_TYPE_DOMAIN, Diligent::SHADER_TYPE_GEOMETRY,
                                             Diligent::SHADER_TYPE_PIXEL };
    for (Diligent::SHADER_TYPE stage : stages) {
        if (Diligent::IShaderResourceVariable* variable =
                impl_->semanticWirePso->GetStaticVariableByName (stage, "ArchVizConstants"))
            variable->Set (impl_->constants);
    }
    impl_->semanticWirePso->CreateShaderResourceBinding (&impl_->semanticWireSrb, true);
    if (impl_->semanticWireSrb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(face-aware tessellated wire) failed";
        return false;
    }
    return true;
}

bool DiligentScene::CreateResolvePipelines (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                            const Diligent::SamplerDesc& envSampler, std::string& error)
{
    // ---- the resolve PSO ----------------------------------------------------
    // Full-screen triangle, no depth; the HDR target is DYNAMIC because it
    // is recreated on resize.
    {
        // ⚠️ ONLY THE TWO THE SHADER STILL DECLARES. The SSR and G-buffer
        // entries moved to the composite PSO with the code that read them; a
        // layout naming variables the shader does not have is not an error, it
        // is just a lie that outlives the next reader's patience.
        Diligent::ShaderResourceVariableDesc resolveVariables[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_hdrColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_hdrCoverage", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
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
    // ---- RE51.C7: the SSR composite PSO -------------------------------------
    // Writes HDR, not display-referred colour, so its target format is the HDR
    // one rather than the swap chain's -- see kArchVizSsrCompositePS for why
    // this pass exists separately from the resolve at all.
    {
        Diligent::ShaderResourceVariableDesc compositeVariables[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_hdrColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_ssrColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferRoughness", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferNormal", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferAlbedo", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
            { Diligent::SHADER_TYPE_PIXEL, "g_gbufferMaterialData", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        };
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz SSR composite PSO";
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        pci.PSODesc.ResourceLayout.Variables = compositeVariables;
        pci.PSODesc.ResourceLayout.NumVariables = _countof (compositeVariables);
        const Diligent::ImmutableSamplerDesc compositeSamplers[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_envMap_sampler", envSampler },
        };
        pci.PSODesc.ResourceLayout.ImmutableSamplers = compositeSamplers;
        pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (compositeSamplers);
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        gp.DepthStencilDesc.DepthEnable = Diligent::False;
        pci.pVS = impl_->fullScreenVs;
        pci.pPS = impl_->ssrCompositePs;
        device->CreateGraphicsPipelineState (pci, &impl_->ssrCompositePso);
        if (impl_->ssrCompositePso == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(ArchViz SSR composite) failed";
            return false;
        }
        if (Diligent::IShaderResourceVariable* cb =
                impl_->ssrCompositePso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "ArchVizConstants"))
            cb->Set (impl_->constants);
        if (Diligent::ITextureView* envView = impl_->environment.ShaderView ()) {
            if (Diligent::IShaderResourceVariable* envVar =
                    impl_->ssrCompositePso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_envMap"))
                envVar->Set (envView);
        }
        impl_->ssrCompositePso->CreateShaderResourceBinding (&impl_->ssrCompositeSrb, true);
        if (impl_->ssrCompositeSrb == nullptr) {
            error = "Diligent CreateShaderResourceBinding(ArchViz SSR composite) failed";
            return false;
        }
    }

    return true;
}

// RE51.C7. The target the SSR composite writes into. Same shape and reasoning
// as EnsureCoverageTarget: allocated only when the pass that needs it runs.
bool DiligentScene::EnsureSsrCompositeTarget ()
{
    if (impl_ == nullptr || impl_->device == nullptr)
        return false;
    if (impl_->ssrCompositeWidth == impl_->viewportWidth && impl_->ssrCompositeHeight == impl_->viewportHeight &&
        impl_->ssrCompositeTexture != nullptr)
        return true;

    impl_->ssrCompositeTexture.Release ();
    impl_->ssrCompositeRTV = nullptr;
    impl_->ssrCompositeSRV = nullptr;
    impl_->ssrCompositeWidth = 0;
    impl_->ssrCompositeHeight = 0;

    if (impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return false;

    Diligent::TextureDesc td;
    td.Name = "ArchViz HDR scene colour with SSR composited";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = impl_->viewportWidth;
    td.Height = impl_->viewportHeight;
    td.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    td.MipLevels = 1;
    td.Usage = Diligent::USAGE_DEFAULT;
    td.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    td.ClearValue.SetColor (Diligent::TEX_FORMAT_RGBA16_FLOAT, 0.0f, 0.0f, 0.0f, 0.0f);

    impl_->device->CreateTexture (td, nullptr, &impl_->ssrCompositeTexture);
    if (impl_->ssrCompositeTexture == nullptr)
        return false;

    impl_->ssrCompositeRTV = impl_->ssrCompositeTexture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    impl_->ssrCompositeSRV = impl_->ssrCompositeTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    impl_->ssrCompositeWidth = impl_->viewportWidth;
    impl_->ssrCompositeHeight = impl_->viewportHeight;
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

// The selection's depth prime and its two edge passes (#118). ⚠️ NOT CALLED
// FROM Init: DrawLineOverlays builds them on the first frame anything is marked,
// against the formats Init recorded, and latches a failure -- the occlusion
// prepass's shape, for its reasons.
bool DiligentScene::CreateSelectionPipelines (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                              uint32_t depthBufferFormat, std::string& error)
{
    if (device == nullptr || impl_->vs == nullptr || impl_->flatPs == nullptr) {
        error = "the selection pipelines need the scene's mesh VS and flat PS, which Init builds first";
        return false;
    }
    Impl::SelectionPipelines& sel = impl_->selectionPipelines;
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },
    };

    // Everything both families share: the formats, the mesh layout, and the
    // "over" blend every line pass uses (see makeFlatPipeline in DiligentScene.cpp
    // for why the alpha channel has its own equation).
    auto describe = [&] (Diligent::GraphicsPipelineStateCreateInfo& pci, const char* name) {
        pci.PSODesc.Name = name;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_SOLID;
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
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
    };
    // ⚠️ ONLY THE STAGES THE PIPELINE HAS. Asking a VS/PS pipeline for a hull
    // stage's variable is not a harmless null on every backend.
    auto finish = [&] (Diligent::GraphicsPipelineStateCreateInfo& pci, RefCntAutoPtr<Diligent::IPipelineState>& pso,
                       RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb, bool tessellated) -> bool {
        device->CreateGraphicsPipelineState (pci, &pso);
        if (pso == nullptr) {
            error = std::string ("Diligent CreateGraphicsPipelineState(") + pci.PSODesc.Name + ") failed";
            return false;
        }
        const Diligent::SHADER_TYPE stages[] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL,
                                                 Diligent::SHADER_TYPE_HULL, Diligent::SHADER_TYPE_DOMAIN,
                                                 Diligent::SHADER_TYPE_GEOMETRY };
        for (size_t k = 0; k < (tessellated ? _countof (stages) : 2u); ++k) {
            if (Diligent::IShaderResourceVariable* variable =
                    pso->GetStaticVariableByName (stages[k], "ArchVizConstants"))
                variable->Set (impl_->constants);
        }
        pso->CreateShaderResourceBinding (&srb, true);
        if (srb == nullptr) {
            error = std::string ("Diligent CreateShaderResourceBinding(") + pci.PSODesc.Name + ") failed";
            return false;
        }
        return true;
    };

    // ---- the depth prime ----------------------------------------------------
    // ⚠️ THE SAME VERTEX SHADER AS THE SHADED PASS, AND LESS_EQUAL. Over an
    // opaque element the depth it writes is bit-identical to what is already
    // there, so priming one costs a draw and changes nothing; over glass it
    // supplies the depth the transparent pass never wrote. No colour at all.
    for (int i = 0; i < kCullModeCount; ++i) {
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);
        Diligent::GraphicsPipelineStateCreateInfo pci;
        describe (pci, "ArchViz selection depth prime PSO");
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
        gp.DepthStencilDesc.DepthFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;
        gp.BlendDesc.RenderTargets[0].BlendEnable = Diligent::False;
        gp.BlendDesc.RenderTargets[0].RenderTargetWriteMask = Diligent::COLOR_MASK_NONE;
        pci.pVS = impl_->vs;
        pci.pPS = impl_->flatPs;
        if (!finish (pci, sel.depthPso[i], sel.depthSrb[i], false))
            return false;
    }

    // ---- the edges ----------------------------------------------------------
    // ⚠️ LESS_EQUAL AND GREATER ARE EXACT COMPLEMENTS, so every edge fragment
    // lands in exactly one of the two passes -- bright where nothing is in front
    // of it, faint where something is. The face-aware wire is tessellated and so
    // not bit-identical to the surface it lies on; its geometry shader pulls it
    // toward the camera (g_edgeParams) so an edge ON a visible face reads as
    // visible. The plain wire runs the shaded pass's own VS and needs no pull.
    sel.semantic = impl_->semanticWirePso != nullptr;
    const Diligent::COMPARISON_FUNCTION tests[2] = { Diligent::COMPARISON_FUNC_LESS_EQUAL,
                                                     Diligent::COMPARISON_FUNC_GREATER };
    const Diligent::ShaderResourceVariableDesc variables[] = {
        { Diligent::SHADER_TYPE_HULL, "g_wirePatchFlags", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    for (int i = 0; i < 2; ++i) {
        Diligent::GraphicsPipelineStateCreateInfo pci;
        describe (pci, i == Impl::kVisibleEdges ? "ArchViz selection visible edges PSO"
                                                : "ArchViz selection hidden edges PSO");
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        // Lines have no facing to cull (see wirePso in DiligentSceneImpl.hpp).
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
        gp.DepthStencilDesc.DepthFunc = tests[i];
        if (sel.semantic) {
            gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            pci.pVS = impl_->wireVs;
            pci.pHS = impl_->wireHs;
            pci.pDS = impl_->wireDs;
            pci.pGS = impl_->wireGs;
            pci.pPS = impl_->wirePs;
            pci.PSODesc.ResourceLayout.Variables = variables;
            pci.PSODesc.ResourceLayout.NumVariables = _countof (variables);
        }
        else {
            gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_WIREFRAME;
            pci.pVS = impl_->vs;
            pci.pPS = impl_->flatPs;
        }
        if (!finish (pci, sel.edgePso[i], sel.edgeSrb[i], sel.semantic))
            return false;
    }
    return true;
}

} // namespace archviz
} // namespace geomsrv
