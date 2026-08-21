// The scene's LIFECYCLE: shaders, the constant buffer, the pipeline states and
// their resource bindings. The element map lives in DiligentSceneGeometry.cpp
// and the passes in DiligentSceneDraw.cpp; DiligentSceneImpl.hpp says why the
// class is spread over three files and what they share.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/DiligentPickBuffer.hpp"
#include "ArchViz/DiligentShaders.hpp"

#include <Sampler.h>

namespace geomsrv {
namespace archviz {

namespace {

// 2048 is the size at which one building's shadow reads cleanly on a 1080p
// palette without the depth pass costing anything measurable: 16 MB of D32 and a
// second pass over geometry that is already resident. 4096 buys a visible
// improvement only when the model fills the frustum edge to edge.
constexpr uint32_t kShadowResolution = 2048;

} // namespace

DiligentScene::DiligentScene () : impl_ (std::make_unique<Impl> ())
{
}
DiligentScene::~DiligentScene ()
{
    Shutdown ();
}

bool DiligentScene::IsReady () const
{
    return impl_ != nullptr && impl_->ready;
}

bool DiligentScene::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                          std::string& error)
{
    if (device == nullptr) {
        error = "DiligentScene::Init got no render device";
        return false;
    }
    if (impl_->ready)
        return true;

    auto compile = [&] (Diligent::SHADER_TYPE type, const char* name, const char* body,
                        RefCntAutoPtr<Diligent::IShader>& out, const char* more = nullptr,
                        const char* evenMore = nullptr, const char* last = nullptr) -> bool {
        // ⚠️ THE SOURCE MUST OUTLIVE CreateShader. `ArchVizShaderSource` returns
        // by value and `sci.Source` is a borrowed pointer; binding the temporary
        // to a named local is what keeps it alive across the call. Passing
        // `ArchVizShaderSource(body).c_str()` inline compiles, and reads freed
        // memory.
        const std::string source = ArchVizShaderSource (body, more, evenMore, last);
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = name;
        sci.Desc.ShaderType = type;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = source.c_str ();
        sci.SourceLength = source.size ();
        device->CreateShader (sci, &out, nullptr);
        if (out == nullptr) {
            error = std::string ("Diligent CreateShader(") + name +
                    ") failed -- the HLSL compiler's own message is in the debug output";
            return false;
        }
        return true;
    };

    if (!compile (Diligent::SHADER_TYPE_VERTEX, "ArchViz mesh VS", kArchVizMeshVS, impl_->vs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz mesh PS", kArchVizEnvCommonPS, impl_->ps, kArchVizMeshPS,
                  kArchVizMeshPSMain, kArchVizMeshPSMainTail))
        return false;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "ArchViz shadow VS", kArchVizShadowVS, impl_->shadowVs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz flat PS", kArchVizFlatPS, impl_->flatPs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "ArchViz outline VS", kArchVizOutlineVS, impl_->outlineVs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz G-buffer PS", kArchVizGBufferPS, impl_->gBufferPs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "ArchViz full-screen VS", kArchVizFullScreenVS, impl_->fullScreenVs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz G-buffer debug PS", kArchVizGBufferDebugPS,
                  impl_->gBufferDebugPs))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz ambient occlusion debug PS", kArchVizAmbientOcclusionDebugPS,
                  impl_->ambientOcclusionDebugPs))
        return false;
    // ⚠️ BOTH TAKE kArchVizEnvCommonPS AS THEIR PRELUDE, and so does the mesh PS
    // above -- that is the single copy of EnvUv. tools/quality/check_hlsl.py's
    // PRELUDES table mirrors these three lines; a stage added here and not
    // there is compiled by the gate without its prelude and fails loudly, which
    // is the intended way round.
    if (!compile (Diligent::SHADER_TYPE_VERTEX, "ArchViz environment background VS", kArchVizEnvCommonPS,
                  impl_->envBackgroundVs, kArchVizEnvBackgroundVS))
        return false;
    if (!compile (Diligent::SHADER_TYPE_PIXEL, "ArchViz environment background PS", kArchVizEnvCommonPS,
                  impl_->envBackgroundPs, kArchVizEnvBackgroundPS))
        return false;

    Diligent::BufferDesc cbd;
    cbd.Name = "ArchViz constants";
    cbd.Size = sizeof (DiligentSceneConstants);
    cbd.Usage = Diligent::USAGE_DYNAMIC;
    cbd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    cbd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    device->CreateBuffer (cbd, nullptr, &impl_->constants);
    if (impl_->constants == nullptr) {
        error = "Diligent CreateBuffer(ArchViz constants) failed";
        return false;
    }

    // ⚠️ THIS LAYOUT AND ArchVizVertex ARE ONE CONTRACT, exactly as
    // varying.def.sc was on the bgfx side. A mismatch does not fail: it reads
    // the wrong bytes, and the model renders confidently wrong.
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // normal
        // ⚠️ NORMALIZED uint8 x4. The struct calls it `abgr` after the 0xAABBGGRR
        // literal, but its BYTES are R,G,B,A, so this arrives in the shader as
        // rgba with no swizzle. See DiligentShaders.hpp.
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True }, // colour
    };

    // Borrowed for the deferred environment load -- see Impl::device.
    impl_->device = device;

    Diligent::GBuffer::ElementDesc gBufferElements[6] {};
    gBufferElements[kGBufferNormal].Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    gBufferElements[kGBufferNormal].ClearValue.SetColor (Diligent::TEX_FORMAT_RGBA16_FLOAT, 0.0f, 0.0f, 0.0f, 0.0f);
    // ⚠️ _SRGB, AND IT IS THE ONE G-BUFFER CHANNEL THAT WANTS IT. Albedo is the
    // only colour in the G-buffer; everything else here is a vector, a depth or
    // a scalar. Since RE51.B7 the value written is LINEAR reflectance, and linear
    // quantises badly in 8 bits at the dark end -- this project's anthracite grey
    // is 0.040, which lands on byte 10 where one step is 10% of the value, so
    // dark facades band visibly. An _SRGB view spends the same 8 bits where the
    // eye is: the hardware encodes on write and decodes on read, so the shader on
    // both sides still sees linear and nothing else changes.
    // ⚠️ DO NOT COPY THIS TO ANOTHER CHANNEL. A normal, a roughness or a motion
    // vector put through the sRGB curve is silently corrupted.
    gBufferElements[kGBufferAlbedo].Format = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    gBufferElements[kGBufferAlbedo].ClearValue.SetColor (Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB, 0.0f, 0.0f, 0.0f,
                                                         0.0f);
    gBufferElements[kGBufferRoughness].Format = Diligent::TEX_FORMAT_R16_FLOAT;
    gBufferElements[kGBufferRoughness].ClearValue.SetColor (Diligent::TEX_FORMAT_R16_FLOAT, 0.0f, 0.0f, 0.0f, 0.0f);
    gBufferElements[kGBufferMaterialData].Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
    gBufferElements[kGBufferMaterialData].ClearValue.SetColor (Diligent::TEX_FORMAT_RGBA16_FLOAT, 0.0f, 0.0f, 0.0f,
                                                               0.0f);
    gBufferElements[kGBufferDepth].Format = Diligent::TEX_FORMAT_D32_FLOAT;
    gBufferElements[kGBufferDepth].ClearValue.SetDepthStencil (Diligent::TEX_FORMAT_D32_FLOAT, 1.0f);
    gBufferElements[kGBufferMotion].Format = Diligent::TEX_FORMAT_RG16_FLOAT;
    gBufferElements[kGBufferMotion].ClearValue.SetColor (Diligent::TEX_FORMAT_RG16_FLOAT, 0.0f, 0.0f, 0.0f, 0.0f);
    impl_->gBuffer = std::make_unique<Diligent::GBuffer> (gBufferElements, _countof (gBufferElements));

    // ⚠️ THE SHADOW MAP GOES UP BEFORE THE MESH PIPELINES DO, because its shader
    // view is bound to them as a STATIC variable. Its failure is not fatal: a
    // viewer with no shadows is worth far more than no viewer, so the reason is
    // reported through the stats and the scene renders unshadowed.
    std::string shadowError;
    if (!impl_->shadowMap.Init (device, impl_->shadowVs, impl_->constants, layout, _countof (layout), kShadowResolution,
                                shadowError)) {
        // Not returned as `error` -- that would fail Init and leave a black
        // viewport for a missing shadow.
        impl_->shadowMap.Shutdown ();
    }

    // ⚠️ AND SO DOES THE ENVIRONMENT MAP, FOR THE SAME REASON AND WITH THE SAME
    // FAILURE POLICY. Its texture is allocated here, empty; nothing is loaded
    // until a path arrives over the bus. A viewer with no sky is worth far more
    // than no viewer, so a failure is recorded and the scene renders on the
    // two-colour ambient it has always had.
    std::string environmentError;
    if (!impl_->environment.Init (device, environmentError)) {
        impl_->environment.Shutdown ();
        impl_->environmentError = environmentError;
    }

    // Point sampling, clamped. ⚠️ CLAMP, NOT WRAP. A fragment just outside the
    // light's frustum would otherwise read the depth of something on the far
    // side of the model and be shadowed by it -- a shadow that appears in mid-air
    // with nothing casting it. The pixel shader also rejects out-of-range uv, so
    // this is the second of two guards, and both are cheap.
    Diligent::SamplerDesc shadowSampler;
    shadowSampler.MinFilter = Diligent::FILTER_TYPE_POINT;
    shadowSampler.MagFilter = Diligent::FILTER_TYPE_POINT;
    shadowSampler.MipFilter = Diligent::FILTER_TYPE_POINT;
    shadowSampler.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    shadowSampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    shadowSampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;
    // ⚠️ THE NAME IS THE SAMPLER'S OWN, NOT THE TEXTURE'S. Diligent only folds
    // `g_shadowMap_sampler` into `g_shadowMap` when a shader is created with
    // UseCombinedTextureSamplers, and these are not -- so an immutable sampler
    // named "g_shadowMap" matches nothing. That mismatch is not a failure: the
    // pipeline is created, the sampler falls back to the D3D11 default, shadows
    // still appear, and the only trace is six "No resource is assigned to static
    // shader variable 'g_shadowMap_sampler'" errors per launch. Which means the
    // CLAMP addressing reasoned about above was not actually in effect.

    // The environment's sampler is a DIFFERENT one, and every field differs for
    // a reason. ⚠️ U WRAPS AND V CLAMPS, WHICH IS NOT A DETAIL: an
    // equirectangular map is periodic in longitude (u) and bounded in latitude
    // (v), so wrapping v mirrors the sky through the poles and clamping u puts a
    // visible seam of stretched pixels down one side of every reflection.
    // Linear + mip-linear because the roughness blur IS the mip selection --
    // point mip filtering would make a surface's reflection jump between blur
    // levels as its roughness or its angle changes.
    Diligent::SamplerDesc envSampler;
    envSampler.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    envSampler.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    envSampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    envSampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;

    const Diligent::ImmutableSamplerDesc immutableSamplers[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_shadowMap_sampler", shadowSampler },
        { Diligent::SHADER_TYPE_PIXEL, "g_envMap_sampler", envSampler },
    };

    // See the ⚠️ at the mesh PSO's resource layout for why this one variable is
    // not STATIC like everything else the mesh shader reads.
    const Diligent::ShaderResourceVariableDesc meshVariables[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_ambientOcclusion", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };

    for (int i = 0; i < kCullModeCount; ++i) {
        // ⚠️ THIS ORDER IS CullIndex's, INVERTED. See DiligentSceneImpl.hpp.
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);

        for (int blended = 0; blended < 2; ++blended) {
            Diligent::GraphicsPipelineStateCreateInfo pci;
            pci.PSODesc.Name = blended ? "ArchViz mesh PSO (blend)" : "ArchViz mesh PSO";
            Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
            gp.NumRenderTargets = 1;
            // ⚠️ THE PSO RECORDS THE FORMATS IT WILL RENDER INTO. Passing the
            // swap chain's actual formats rather than a guess turns a mismatch
            // into a creation-time failure instead of a draw-time validation
            // error nobody reads.
            gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
            gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
            gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
            gp.RasterizerDesc.FrontCounterClockwise = Diligent::False; // bgfx's setting
            gp.DepthStencilDesc.DepthEnable = Diligent::True;
            // ⚠️ THE TRANSPARENT PASS DOES NOT WRITE DEPTH. See the header: a
            // pane of glass that writes depth hides the room behind it.
            gp.DepthStencilDesc.DepthWriteEnable = blended ? Diligent::False : Diligent::True;
            gp.InputLayout.LayoutElements = layout;
            gp.InputLayout.NumElements = _countof (layout);

            Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
            rt.BlendEnable = blended ? Diligent::True : Diligent::False;
            if (blended) {
                // ⚠️ PREMULTIPLIED (ONE / INV_SRC_ALPHA), NOT SRC_ALPHA, AND
                // THE REASON IS GLASS REFLECTIONS. With SRC_ALPHA the hardware
                // scales EVERYTHING the shader returns by the surface's own
                // opacity -- including the sky reflected off it. This project's
                // clear glass is 69% transparent, so its reflection arrived at
                // 31% strength on top of an already-subtle 8% Fresnel, which is
                // to say invisible; and the relationship is backwards, because a
                // window reflects MORE as it gets clearer, not less. Reported
                // live as "no reflections on glass, even with an HDR loaded".
                //
                // Premultiplied lets the shader decide per TERM: it attenuates
                // what is seen THROUGH the surface by alpha and leaves what
                // bounces OFF it alone. kArchVizMeshPSMain does that and must
                // stay in step -- the two halves are one decision.
                rt.SrcBlend = Diligent::BLEND_FACTOR_ONE;
                rt.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
                rt.BlendOp = Diligent::BLEND_OPERATION_ADD;
                rt.SrcBlendAlpha = Diligent::BLEND_FACTOR_ONE;
                rt.DestBlendAlpha = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
                rt.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;
            }

            pci.pVS = impl_->vs;
            pci.pPS = impl_->ps;
            pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
            // ⚠️ THE AMBIENT OCCLUSION IS THE ONE MUTABLE TEXTURE IN THIS
            // PIPELINE, AND IT HAS TO BE (RE51.C3). Every other resource here is
            // STATIC because it is allocated once and refilled -- that is the
            // whole reason EnvironmentMap fixes its own size. The AO texture is
            // owned by DiligentFX's PostFXContext, which REALLOCATES it on every
            // viewport resize, so a static binding would capture a view that is
            // freed the first time the panel is dragged wider. DYNAMIC lets Draw
            // re-point it per frame.
            pci.PSODesc.ResourceLayout.Variables = meshVariables;
            pci.PSODesc.ResourceLayout.NumVariables = _countof (meshVariables);
            pci.PSODesc.ResourceLayout.ImmutableSamplers = immutableSamplers;
            pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (immutableSamplers);

            RefCntAutoPtr<Diligent::IPipelineState>& pso = blended ? impl_->blendPso[i] : impl_->opaquePso[i];
            RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb = blended ? impl_->blendSrb[i] : impl_->opaqueSrb[i];

            device->CreateGraphicsPipelineState (pci, &pso);
            if (pso == nullptr) {
                error = "Diligent CreateGraphicsPipelineState(ArchViz mesh) failed";
                return false;
            }

            // Automatic resource binding: the cbuffer is resolved BY NAME from
            // shader reflection, with no register slot named on this side. Both
            // stages declare it and both must find it -- a stage that silently
            // missed would render with whatever was last in the buffer.
            const Diligent::SHADER_TYPE stages[2] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL };
            for (Diligent::SHADER_TYPE stage : stages) {
                Diligent::IShaderResourceVariable* variable = pso->GetStaticVariableByName (stage, "ArchVizConstants");
                if (variable == nullptr) {
                    error = "Diligent could not resolve the `ArchVizConstants` constant buffer "
                            "by name in the ";
                    error += (stage == Diligent::SHADER_TYPE_VERTEX ? "vertex" : "pixel");
                    error += " shader (automatic resource binding did not find it)";
                    return false;
                }
                variable->Set (impl_->constants);
            }

            // The shadow map itself. STATIC because the texture is created once
            // and never replaced -- only its CONTENTS change, every frame, which
            // a binding does not care about.
            //
            // ⚠️ ITS ABSENCE IS NOT AN ERROR, in either direction. When the map
            // failed to initialise there is nothing to bind, and the pixel
            // shader's `g_shadowParams.z` gate keeps it from sampling an unbound
            // texture. When the map exists but the variable does not, the HLSL
            // compiler optimised the sampling away -- which would mean the gate
            // is permanently off, and that is worth knowing rather than
            // crashing over, so it is left to the Shadow debug view to show.
            if (Diligent::ITextureView* shadowView = impl_->shadowMap.ShaderView ()) {
                Diligent::IShaderResourceVariable* shadowVar =
                    pso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_shadowMap");
                if (shadowVar != nullptr)
                    shadowVar->Set (shadowView);
            }

            // The environment map, on exactly the same terms as the shadow map
            // above -- allocated once, contents replaced on load, absence not an
            // error. ⚠️ THIS IS WHY EnvironmentMap FIXES ITS OWN SIZE: the view
            // bound here is captured by every SRB created below, so a texture
            // recreated at a different size on the next load would leave all of
            // them pointing at the freed one.
            if (Diligent::ITextureView* envView = impl_->environment.ShaderView ()) {
                Diligent::IShaderResourceVariable* envVar =
                    pso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_envMap");
                if (envVar != nullptr)
                    envVar->Set (envView);
            }

            pso->CreateShaderResourceBinding (&srb, true);
            if (srb == nullptr) {
                error = "Diligent CreateShaderResourceBinding(ArchViz mesh) failed";
                return false;
            }
        }
    }

    // ---- debug G-buffer geometry pipelines --------------------------------
    // Same mesh VS and layout as the visible pass, but four material targets and
    // the G-buffer's shader-readable depth attachment.
    for (int i = 0; i < kCullModeCount; ++i) {
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);

        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz G-buffer geometry PSO";
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 4;
        gp.RTVFormats[0] = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        gp.RTVFormats[1] = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;   // albedo; see the G-buffer element
        gp.RTVFormats[2] = Diligent::TEX_FORMAT_R16_FLOAT;
        gp.RTVFormats[3] = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        gp.DSVFormat = Diligent::TEX_FORMAT_D32_FLOAT;
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
        gp.InputLayout.LayoutElements = layout;
        gp.InputLayout.NumElements = _countof (layout);
        pci.pVS = impl_->vs;
        pci.pPS = impl_->gBufferPs;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

        device->CreateGraphicsPipelineState (pci, &impl_->gBufferPso[i]);
        if (impl_->gBufferPso[i] == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(ArchViz G-buffer geometry) failed";
            return false;
        }
        const Diligent::SHADER_TYPE stages[2] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL };
        for (Diligent::SHADER_TYPE stage : stages) {
            Diligent::IShaderResourceVariable* constantsVar =
                impl_->gBufferPso[i]->GetStaticVariableByName (stage, "ArchVizConstants");
            if (constantsVar == nullptr) {
                error = "Diligent could not resolve `ArchVizConstants` in the G-buffer ";
                error += (stage == Diligent::SHADER_TYPE_VERTEX ? "vertex" : "pixel");
                error += " shader";
                return false;
            }
            constantsVar->Set (impl_->constants);
        }
        impl_->gBufferPso[i]->CreateShaderResourceBinding (&impl_->gBufferSrb[i], true);
        if (impl_->gBufferSrb[i] == nullptr) {
            error = "Diligent CreateShaderResourceBinding(ArchViz G-buffer geometry) failed";
            return false;
        }
    }

    Diligent::ShaderResourceVariableDesc debugVariables[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_gbufferNormal", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_gbufferAlbedo", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_gbufferRoughness", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_gbufferMaterialData", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_depthRange", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    Diligent::GraphicsPipelineStateCreateInfo debugPci;
    debugPci.PSODesc.Name = "ArchViz G-buffer debug PSO";
    debugPci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    debugPci.PSODesc.ResourceLayout.Variables = debugVariables;
    debugPci.PSODesc.ResourceLayout.NumVariables = _countof (debugVariables);
    Diligent::GraphicsPipelineDesc& debugGp = debugPci.GraphicsPipeline;
    debugGp.NumRenderTargets = 1;
    debugGp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    debugGp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    debugGp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    debugGp.DepthStencilDesc.DepthEnable = Diligent::False;
    debugPci.pVS = impl_->fullScreenVs;
    debugPci.pPS = impl_->gBufferDebugPs;
    device->CreateGraphicsPipelineState (debugPci, &impl_->gBufferDebugPso);
    if (impl_->gBufferDebugPso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(ArchViz G-buffer debug) failed";
        return false;
    }
    Diligent::IShaderResourceVariable* debugConstants =
        impl_->gBufferDebugPso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "ArchVizConstants");
    if (debugConstants == nullptr) {
        error = "Diligent could not resolve `ArchVizConstants` in the G-buffer debug shader";
        return false;
    }
    debugConstants->Set (impl_->constants);
    impl_->gBufferDebugPso->CreateShaderResourceBinding (&impl_->gBufferDebugSrb, true);
    if (impl_->gBufferDebugSrb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(ArchViz G-buffer debug) failed";
        return false;
    }

    // ---- the sky background --------------------------------------------------
    //
    // ⚠️ ONE FULL-SCREEN TRIANGLE WITH DEPTH OFF, DRAWN BEFORE THE MODEL. The
    // alternative -- drawing it last where the depth buffer says nothing was
    // written -- needs a depth test the mesh PSOs do not carry and gains
    // nothing: the model overwrites the sky either way, and going first means
    // this pass never has to know what the depth buffer contains.
    {
        const Diligent::ImmutableSamplerDesc backgroundSampler[] = {
            { Diligent::SHADER_TYPE_PIXEL, "g_envMap_sampler", envSampler },
        };
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz environment background PSO";
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        pci.PSODesc.ResourceLayout.ImmutableSamplers = backgroundSampler;
        pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (backgroundSampler);
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        // ⚠️ NO DEPTH TEST *AND* NO DEPTH WRITE. Writing would put the sky at
        // the near plane and z-reject the entire building behind it.
        gp.DepthStencilDesc.DepthEnable = Diligent::False;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::False;
        pci.pVS = impl_->envBackgroundVs;
        pci.pPS = impl_->envBackgroundPs;
        device->CreateGraphicsPipelineState (pci, &impl_->envBackgroundPso);
        if (impl_->envBackgroundPso == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(ArchViz environment background) failed";
            return false;
        }
        for (Diligent::SHADER_TYPE stage : { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL }) {
            if (Diligent::IShaderResourceVariable* cb =
                    impl_->envBackgroundPso->GetStaticVariableByName (stage, "ArchVizConstants"))
                cb->Set (impl_->constants);
        }
        // Same texture, same lifetime rule as the mesh PSOs above: the view is
        // captured here, which is why EnvironmentMap never reallocates.
        if (Diligent::ITextureView* envView = impl_->environment.ShaderView ()) {
            if (Diligent::IShaderResourceVariable* envVar =
                    impl_->envBackgroundPso->GetStaticVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_envMap"))
                envVar->Set (envView);
        }
        impl_->envBackgroundPso->CreateShaderResourceBinding (&impl_->envBackgroundSrb, true);
        if (impl_->envBackgroundSrb == nullptr) {
            error = "Diligent CreateShaderResourceBinding(ArchViz environment background) failed";
            return false;
        }
    }

    // ⚠️ EAGERLY, NOT LAZILY LIKE THE AO. A compute PSO that fails to build is
    // worth hearing about here, where Init already reports why; deferring it to
    // the first depth-view frame turns a shader typo into a silently wrong
    // picture on the render thread, with nothing to attribute it to.
    if (!impl_->depthRange.Init (device, error))
        return false;

    Diligent::ShaderResourceVariableDesc ambientOcclusionVariables[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_ambientOcclusion", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    debugPci.PSODesc.Name = "ArchViz ambient occlusion debug PSO";
    debugPci.PSODesc.ResourceLayout.Variables = ambientOcclusionVariables;
    debugPci.PSODesc.ResourceLayout.NumVariables = _countof (ambientOcclusionVariables);
    debugPci.pPS = impl_->ambientOcclusionDebugPs;
    device->CreateGraphicsPipelineState (debugPci, &impl_->ambientOcclusionDebugPso);
    if (impl_->ambientOcclusionDebugPso == nullptr) {
        error = "Diligent CreateGraphicsPipelineState(ArchViz ambient occlusion debug) failed";
        return false;
    }
    impl_->ambientOcclusionDebugPso->CreateShaderResourceBinding (&impl_->ambientOcclusionDebugSrb, true);
    if (impl_->ambientOcclusionDebugSrb == nullptr) {
        error = "Diligent CreateShaderResourceBinding(ArchViz ambient occlusion debug) failed";
        return false;
    }

    // ---- the pick pipelines ------------------------------------------------
    // ⚠️ A SEPARATE SET, AND MATCHED TO THE MESH PSOs IN EVERYTHING THAT MOVES A
    // VERTEX. Same vertex shader, same input layout, same depth test, same cull
    // mode per index -- so a pixel the user can see is a pixel the pick pass
    // resolves. What differs is only what a pick must not have: the target
    // formats (an 8x8 RGBA8_UNORM, not the swap chain's sRGB back buffer), no
    // blending, and a pixel shader that writes the id and nothing else.
    for (int i = 0; i < kCullModeCount; ++i) {
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);

        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz pick id PSO";
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (PickColorFormat ());
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (PickDepthFormat ());
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
        // ⚠️ DEPTH ON, WRITING. The pick must resolve to the NEAREST element,
        // which is the whole question being asked.
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
        gp.InputLayout.LayoutElements = layout;
        gp.InputLayout.NumElements = _countof (layout);
        // ⚠️ BLENDING STAYS OFF, EXPLICITLY -- glass included. A half-transparent
        // pane blended into the id target produces the arithmetic mean of two
        // ids, which is a third id that exists and is wrong. The transparent
        // ranges are drawn here as opaque ids instead, so clicking a window
        // selects the window.
        gp.BlendDesc.RenderTargets[0].BlendEnable = Diligent::False;

        pci.pVS = impl_->vs;
        pci.pPS = impl_->flatPs;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

        device->CreateGraphicsPipelineState (pci, &impl_->pickPso[i]);
        if (impl_->pickPso[i] == nullptr) {
            error = "Diligent CreateGraphicsPipelineState(ArchViz pick id) failed";
            return false;
        }

        const Diligent::SHADER_TYPE stages[2] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL };
        for (Diligent::SHADER_TYPE stage : stages) {
            Diligent::IShaderResourceVariable* variable =
                impl_->pickPso[i]->GetStaticVariableByName (stage, "ArchVizConstants");
            if (variable == nullptr) {
                error = "Diligent could not resolve `ArchVizConstants` in the pick pipeline -- "
                        "the id would never reach the shader and every click would resolve to "
                        "the same element";
                return false;
            }
            variable->Set (impl_->constants);
        }

        impl_->pickPso[i]->CreateShaderResourceBinding (&impl_->pickSrb[i], true);
        if (impl_->pickSrb[i] == nullptr) {
            error = "Diligent CreateShaderResourceBinding(ArchViz pick id) failed";
            return false;
        }
    }

    // ---- the silhouette and wireframe pipelines ----------------------------
    // Both draw a FLAT colour into the swap chain's formats and blend with it, so
    // the only differences worth a parameter are the vertex shader, the cull mode
    // and the fill mode. Everything else being shared is what keeps a silhouette
    // from quietly projecting differently from the surface it outlines.
    auto makeFlatPipeline = [&] (const char* name, Diligent::IShader* vertexShader, CullMode cull,
                                 Diligent::FILL_MODE fill, bool writeDepth,
                                 RefCntAutoPtr<Diligent::IPipelineState>& pso,
                                 RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb) -> bool {
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = name;
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
        gp.RasterizerDesc.FillMode = fill;
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
        // ⚠️ DEPTH TESTED, AND FOR THE SILHOUETTE NOT WRITTEN. The test is what
        // turns an expanded copy of the model into a RING: everywhere the model
        // itself is nearer, the hull fails and only the few pixels that spill
        // past the silhouette survive. Writing depth as well would let the hull
        // occlude geometry that is genuinely in front of nothing.
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = writeDepth ? Diligent::True : Diligent::False;
        gp.InputLayout.LayoutElements = layout;
        gp.InputLayout.NumElements = _countof (layout);

        // The same "over" blend the transparent mesh pass uses. ⚠️ THE ALPHA
        // CHANNEL'S OWN EQUATION (ONE / INV_SRC_ALPHA) IS NOT DECORATION: on the
        // overlay's composition target the alpha channel IS the coverage DWM
        // composites with, so an outline drawn over transparent background has to
        // raise it. With the D3D default (alpha treated like colour) an outline
        // over empty sky would come out invisible on the overlay and perfectly
        // fine in the palette, which is the worst possible pair of symptoms.
        Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
        rt.BlendEnable = Diligent::True;
        rt.SrcBlend = Diligent::BLEND_FACTOR_SRC_ALPHA;
        rt.DestBlend = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
        rt.BlendOp = Diligent::BLEND_OPERATION_ADD;
        rt.SrcBlendAlpha = Diligent::BLEND_FACTOR_ONE;
        rt.DestBlendAlpha = Diligent::BLEND_FACTOR_INV_SRC_ALPHA;
        rt.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;

        pci.pVS = vertexShader;
        pci.pPS = impl_->flatPs;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

        device->CreateGraphicsPipelineState (pci, &pso);
        if (pso == nullptr) {
            error = std::string ("Diligent CreateGraphicsPipelineState(") + name + ") failed";
            return false;
        }
        const Diligent::SHADER_TYPE stages[2] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL };
        for (Diligent::SHADER_TYPE stage : stages) {
            Diligent::IShaderResourceVariable* variable = pso->GetStaticVariableByName (stage, "ArchVizConstants");
            if (variable != nullptr)
                variable->Set (impl_->constants);
        }
        pso->CreateShaderResourceBinding (&srb, true);
        if (srb == nullptr) {
            error = std::string ("Diligent CreateShaderResourceBinding(") + name + ") failed";
            return false;
        }
        return true;
    };

    for (int i = 0; i < kCullModeCount; ++i) {
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);
        // ⚠️ INDEXED BY THE VISIBLE PASS'S CULL MODE AND BUILT WITH ITS INVERSE.
        // The draw side then looks the outline up with the SAME index it used for
        // the surface, and cannot pick the arm that draws a solid expanded copy of
        // the model over the model.
        if (!makeFlatPipeline ("ArchViz outline PSO", impl_->outlineVs, InverseCull (cull), Diligent::FILL_MODE_SOLID,
                               false, impl_->outlinePso[i], impl_->outlineSrb[i]))
            return false;
    }

    if (!makeFlatPipeline ("ArchViz wireframe PSO", impl_->vs, CullMode::None, Diligent::FILL_MODE_WIREFRAME, true,
                           impl_->wirePso, impl_->wireSrb))
        return false;

    impl_->ready = true;
    return true;
}

void DiligentScene::SetEnvironmentMap (const char* path)
{
    if (impl_ == nullptr)
        return;
    // ⚠️ ONLY PARKED. The file read and every device call happen in Draw, on the
    // render thread -- see the load block there.
    impl_->pendingEnvironmentPath = path != nullptr ? path : "";
    impl_->environmentLoadPending = true;
}

void DiligentScene::SetEnvironmentSettings (bool enabled, float intensity, float rotationDegrees)
{
    if (impl_ == nullptr)
        return;
    impl_->environmentEnabled = enabled;
    impl_->environmentIntensity = intensity > 0.0f ? intensity : 0.0f;
    impl_->environmentRotationRadians = rotationDegrees * 3.14159265358979323846f / 180.0f;
}

void DiligentScene::SetEnvironmentBackground (bool enabled)
{
    if (impl_ != nullptr)
        impl_->environmentBackground = enabled;
}

void DiligentScene::SetSunWithSkyWeight (float weight)
{
    if (impl_ != nullptr)
        impl_->sunWithSkyWeight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
}

void DiligentScene::SetGrading (float exposure, float reflectance, float roughnessBias)
{
    if (impl_ == nullptr)
        return;
    impl_->exposure = exposure > 0.0f ? exposure : 0.0f;
    impl_->reflectance = reflectance > 0.0f ? reflectance : 0.0f;
    impl_->roughnessBias = roughnessBias < -1.0f ? -1.0f : (roughnessBias > 1.0f ? 1.0f : roughnessBias);
}

void DiligentScene::SetAutoExposure (bool enabled)
{
    if (impl_ != nullptr)
        impl_->autoExposureEnabled = enabled;
}

void DiligentScene::SetWhiteBalance (float kelvin, float tint)
{
    if (impl_ == nullptr)
        return;
    impl_->whiteBalanceKelvin = kelvin;
    impl_->whiteBalanceTint = tint;
}

void DiligentScene::SetCameraRays (const float right[3], const float up[3], const float forward[3])
{
    if (impl_ == nullptr)
        return;
    std::memcpy (impl_->cameraRayRight, right, sizeof (float) * 3);
    std::memcpy (impl_->cameraRayUp, up, sizeof (float) * 3);
    std::memcpy (impl_->cameraRayForward, forward, sizeof (float) * 3);
}

void DiligentScene::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    // RefCntAutoPtr releases on destruction, so this is ORDERING rather than
    // freeing: everything the device owns goes before the device does, which is
    // the caller's job to arrange.
    impl_->elements.clear ();
    impl_->staticMeshes.clear ();
    impl_->overlayMeshes.clear ();
    impl_->shadowMap.Shutdown ();
    impl_->environment.Shutdown ();
    impl_->ambientOcclusion.Shutdown ();
    impl_->depthRange.Shutdown ();
    impl_->envBackgroundSrb.Release ();
    impl_->envBackgroundPso.Release ();
    impl_->ambientOcclusionDebugSrb.Release ();
    impl_->ambientOcclusionDebugPso.Release ();
    impl_->gBufferDebugSrb.Release ();
    impl_->gBufferDebugPso.Release ();
    for (int i = 0; i < kCullModeCount; ++i) {
        impl_->opaqueSrb[i].Release ();
        impl_->blendSrb[i].Release ();
        impl_->pickSrb[i].Release ();
        impl_->outlineSrb[i].Release ();
        impl_->gBufferSrb[i].Release ();
        impl_->opaquePso[i].Release ();
        impl_->blendPso[i].Release ();
        impl_->pickPso[i].Release ();
        impl_->outlinePso[i].Release ();
        impl_->gBufferPso[i].Release ();
    }
    impl_->gBuffer.reset ();
    impl_->gBufferWidth = 0;
    impl_->gBufferHeight = 0;
    impl_->wireSrb.Release ();
    impl_->wirePso.Release ();
    impl_->constants.Release ();
    impl_->outlineVs.Release ();
    impl_->gBufferDebugPs.Release ();
    impl_->ambientOcclusionDebugPs.Release ();
    impl_->fullScreenVs.Release ();
    impl_->gBufferPs.Release ();
    impl_->flatPs.Release ();
    impl_->shadowVs.Release ();
    impl_->ps.Release ();
    impl_->vs.Release ();
    impl_->ready = false;
}

void DiligentScene::SetRenderMode (SceneRenderMode mode)
{
    if (impl_ != nullptr)
        impl_->renderMode = mode;
}

SceneRenderMode DiligentScene::RenderMode () const
{
    return impl_ != nullptr ? impl_->renderMode : SceneRenderMode::Shaded;
}

void DiligentScene::SetRenderQuality (RenderQuality quality)
{
    if (impl_ != nullptr)
        impl_->renderQuality = quality;
}

RenderQuality DiligentScene::GetRenderQuality () const
{
    return impl_ != nullptr ? impl_->renderQuality : RenderQuality::Fast;
}

void DiligentScene::SetSunOverride (bool enabled, float azimuthDegrees, float altitudeDegrees)
{
    if (impl_ == nullptr)
        return;
    impl_->sunOverride = enabled;
    impl_->sunOverrideAzimuth = azimuthDegrees;
    impl_->sunOverrideAltitude = altitudeDegrees;
}

} // namespace archviz
} // namespace geomsrv
