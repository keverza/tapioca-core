// The still-camera mean (issues #14 P1, #247, #248): what the scene does with TAA's output while the
// camera and the content hold still. ArchViz/TemporalConvergence decides the phase and the weights
// and is tested offline; this file owns the GPU side -- the sample pass, its RGBA32_FLOAT target,
// the content key Draw folds in, and the teardown of TAA beside it.
//
// ⚠️ ITS OWN TRANSLATION UNIT ON PURPOSE. DiligentSceneDraw.cpp owns the pass ORDER and is at the
// size cap; this is one responsibility with its own state, so Draw keeps only the call sites.
//
// Measured with RenderBench --temporal (bathroom mirror and Cornell mirror, 960x540, still camera,
// pixels changing by more than 24 levels between consecutive frames, TAA alone vs the mean):
// bathroom 40 -> 0, Cornell mirror 313 (largest 241) -> 0, bathroom with a section cut 576 -> 0 once
// converged. The section's passes still move while the mean accumulates: they are drawn after the
// resolve, and only the jitter stopping stops them.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentPickBuffer.hpp" // PickRequestsIssued
#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/SurfaceClassifier.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

void DiligentScene::SetTemporalConvergence (const TemporalConvergenceSettings& settings)
{
    if (impl_ != nullptr)
        impl_->temporalConvergenceSettings = settings;
}

void DiligentScene::ShutdownTemporal ()
{
    impl_->temporalAntiAliasing.Shutdown ();
    impl_->accumSrb.Release ();
    impl_->accumPso.Release ();
    impl_->accumulatePs.Release ();
    impl_->accumInitFailed = false;
    impl_->accumTexture.Release ();
    impl_->accumRTV = nullptr;
    impl_->accumSRV = nullptr;
    impl_->accumWidth = 0;
    impl_->accumHeight = 0;
    impl_->temporalConvergence.Reset ();
    for (int i = 0; i < 3; ++i) {
        impl_->stableDepthSrb[i].Release ();
        impl_->stableDepthPso[i].Release ();
    }
    impl_->stableDepthInitFailed = false;
    impl_->overlaysUnjittered = false;
}

// ⚠️ NOT BUILT IN Init. Init's pipeline list is the scene's start-up cost and DiligentScene.cpp is
// frozen in size; a viewer that never holds the camera still with TAA on never needs this.
bool DiligentScene::EnsureTemporalMeanPipeline ()
{
    if (impl_->accumPso != nullptr && impl_->accumSrb != nullptr)
        return true;
    if (impl_->accumInitFailed || impl_->device == nullptr || impl_->fullScreenVs == nullptr)
        return false;
    impl_->accumInitFailed = true; // until it has succeeded

    // ⚠️ THE SOURCE MUST OUTLIVE CreateShader (see Init's `compile`): a named local, not a temporary.
    const std::string source = ArchVizShaderSource (kArchVizAccumulatePS);
    Diligent::ShaderCreateInfo sci;
    sci.Desc.Name = "ArchViz still-camera mean PS";
    sci.Desc.ShaderType = Diligent::SHADER_TYPE_PIXEL;
    sci.EntryPoint = "main";
    sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    sci.Source = source.c_str ();
    sci.SourceLength = source.size ();
    impl_->device->CreateShader (sci, &impl_->accumulatePs, nullptr);
    if (impl_->accumulatePs == nullptr) {
        ArchVizLog ("Diligent scene: the still-camera mean is unavailable (its pixel shader did not compile)");
        return false;
    }

    // ⚠️ THE MEAN IS THE BLEND: src * w + dst * (1 - w), w set per draw with SetBlendFactors
    // (TemporalStep::weight). Alpha blends the same way: it is the coverage being averaged.
    const Diligent::ShaderResourceVariableDesc variables[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_taaColor", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_PIXEL, "g_taaCoverage", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    Diligent::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = "ArchViz still-camera mean PSO";
    pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    pci.PSODesc.ResourceLayout.Variables = variables;
    pci.PSODesc.ResourceLayout.NumVariables = _countof (variables);
    Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = Diligent::False;
    Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
    rt.BlendEnable = Diligent::True;
    rt.SrcBlend = Diligent::BLEND_FACTOR_BLEND_FACTOR;
    rt.DestBlend = Diligent::BLEND_FACTOR_INV_BLEND_FACTOR;
    rt.BlendOp = Diligent::BLEND_OPERATION_ADD;
    rt.SrcBlendAlpha = Diligent::BLEND_FACTOR_BLEND_FACTOR;
    rt.DestBlendAlpha = Diligent::BLEND_FACTOR_INV_BLEND_FACTOR;
    rt.BlendOpAlpha = Diligent::BLEND_OPERATION_ADD;
    pci.pVS = impl_->fullScreenVs;
    pci.pPS = impl_->accumulatePs;
    impl_->device->CreateGraphicsPipelineState (pci, &impl_->accumPso);
    if (impl_->accumPso != nullptr)
        impl_->accumPso->CreateShaderResourceBinding (&impl_->accumSrb, true);
    if (impl_->accumPso == nullptr || impl_->accumSrb == nullptr) {
        impl_->accumPso.Release ();
        ArchVizLog ("Diligent scene: the still-camera mean is unavailable (its pipeline was not created)");
        return false;
    }
    impl_->accumInitFailed = false;
    return true;
}

// The mean, RGBA32_FLOAT (see DiligentSceneImpl::accumTexture for why not 16). Allocated on the
// first frame TAA resolves with the feature on, like the coverage target.
bool DiligentScene::EnsureAccumTarget ()
{
    if (impl_->device == nullptr)
        return false;
    if (impl_->accumWidth == impl_->viewportWidth && impl_->accumHeight == impl_->viewportHeight &&
        impl_->accumTexture != nullptr)
        return true;

    impl_->accumTexture.Release ();
    impl_->accumRTV = nullptr;
    impl_->accumSRV = nullptr;
    impl_->accumWidth = 0;
    impl_->accumHeight = 0;
    if (impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return false;

    Diligent::TextureDesc td;
    td.Name = "ArchViz still-camera mean";
    td.Type = Diligent::RESOURCE_DIM_TEX_2D;
    td.Width = impl_->viewportWidth;
    td.Height = impl_->viewportHeight;
    td.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    td.MipLevels = 1;
    td.Usage = Diligent::USAGE_DEFAULT;
    td.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
    impl_->device->CreateTexture (td, nullptr, &impl_->accumTexture);
    if (impl_->accumTexture == nullptr)
        return false;
    impl_->accumRTV = impl_->accumTexture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    impl_->accumSRV = impl_->accumTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    impl_->accumWidth = impl_->viewportWidth;
    impl_->accumHeight = impl_->viewportHeight;
    // ⚠️ A NEW TARGET HOLDS NOTHING, so no mean in progress may continue into it. The size is in
    // the camera key too; this only matters if the two ever disagree.
    impl_->temporalConvergence.Reset ();
    return true;
}

Diligent::ITextureView* DiligentScene::StepTemporalConvergence (Diligent::IDeviceContext* context,
                                                                Diligent::ITextureView* taaColor, size_t pointDraws)
{
    TemporalConvergence& convergence = impl_->temporalConvergence;
    impl_->idleEligible = false;
    if (convergence.Phase () == TemporalPhase::Off)
        return nullptr;
    // What only drawing can tell: the effects switch on asynchronously (#247), so whether each one
    // actually ran is content.
    const bool aoRan = impl_->aoView != nullptr;
    const bool ssrRan = impl_->ssrView != nullptr;
    impl_->frameContent.Add (aoRan);
    impl_->frameContent.Add (ssrRan);
    impl_->frameContent.Add (impl_->atmosphereRan);
    impl_->frameContent.Add (uint64_t (pointDraws));
    const uint64_t contentKey = impl_->frameContent.Value ();
    const uint32_t restartsBefore = convergence.Status ().restarts;
    // ⚠️ BOTH HALVES OR NEITHER. The mean averages radiance and coverage together; a frame where
    // TAA resolved one without the other has nothing consistent to add.
    const bool resolved = context != nullptr && taaColor != nullptr && impl_->taaCoverageView != nullptr &&
                          EnsureTemporalMeanPipeline () && EnsureAccumTarget ();
    const TemporalStep step = convergence.EndFrame (contentKey, resolved);
    // A still image thrown back by its content or by TAA: name what changed. A camera restart is
    // the user navigating and is not logged.
    const TemporalStatus status = convergence.Status ();
    if (status.restarts != restartsBefore && status.lastRestart != TemporalRestart::Camera) {
        std::string what;
        if (impl_->earlyKey != impl_->lastEarlyKey)
            what += " settings/materials/elements";
        if (aoRan != impl_->lastAoRan)
            what += aoRan ? " AO came on" : " AO went off";
        if (ssrRan != impl_->lastSsrRan)
            what += ssrRan ? " SSR came on" : " SSR went off";
        if (impl_->atmosphereRan != impl_->lastAtmosphereRan)
            what += impl_->atmosphereRan ? " atmosphere came on" : " atmosphere went off";
        if (pointDraws != impl_->lastPointDraws)
            what += " point splats";
        ArchVizLog ("Diligent scene: still image restarted (" + std::string (TemporalRestartName (status.lastRestart)) +
                    "):" + (what.empty () ? std::string (" TAA resolved nothing") : what));
    }
    impl_->lastEarlyKey = impl_->earlyKey;
    impl_->lastAoRan = aoRan;
    impl_->lastSsrRan = ssrRan;
    impl_->lastAtmosphereRan = impl_->atmosphereRan;
    impl_->lastPointDraws = pointDraws;
    if (step.accumulate) {
        if (Diligent::IShaderResourceVariable* var =
                impl_->accumSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_taaColor"))
            var->Set (taaColor);
        if (Diligent::IShaderResourceVariable* var =
                impl_->accumSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_taaCoverage"))
            var->Set (impl_->taaCoverageView);
        context->SetRenderTargets (1, &impl_->accumRTV, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetViewports (1, nullptr, 0, 0);
        context->SetPipelineState (impl_->accumPso);
        const float weight[4] = { step.weight, step.weight, step.weight, step.weight };
        context->SetBlendFactors (weight);
        context->CommitShaderResources (impl_->accumSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawAttribs sample;
        sample.NumVertices = 3;
        sample.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw (sample);
        context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
    }
    // #247 idle: a converged mean may stand in for later frames only if every effect the settings
    // ask for is already in it. One still waiting for its pipeline keeps the frames rendering, so
    // its arrival restarts the mean through the key above instead of being skipped forever.
    if (step.present && convergence.Phase () == TemporalPhase::Converged) {
        const bool aoComplete = aoRan || !(impl_->aoEnabled && impl_->aoIntensity > 0.0f);
        const bool ssrComplete = ssrRan || !(impl_->ssrEnabled && impl_->ssrIntensity > 0.0f);
        const bool atmosphereComplete = impl_->atmosphereRan || !impl_->atmosphereEnabled;
        impl_->idleEligible = aoComplete && ssrComplete && atmosphereComplete;
        impl_->idleKey = impl_->earlyKey;
    }
    return step.present ? impl_->accumSRV : nullptr;
}

// ---- the content key ---------------------------------------------------------
//
// Everything the frame's HDR image depends on that is not the camera, so a converged mean
// restarts the moment any of it changes: the frame constants as Draw has them before the range
// loop (sun, sky, environment, AO, exposure, focus), the HDR settings that live outside them,
// then every drawn range.
//
// ⚠️ THE JITTERED AND FRAME-NUMBERED LANES ARE SCRUBBED. The converged frames are unjittered and
// the frames before them are not, and the PCSS phase turns every frame: left in, the key would
// change on exactly the frame the mean converges and restart it forever.
// ⚠️ A SETTING LEFT OUT OF THIS KEY KEEPS A STALE IMAGE ON SCREEN AFTER IT CHANGES. New state that
// changes the HDR image goes in here, not only into the constants.
// #14 white-flash diagnostic. Live: single frames with every surface near white (224-255), the
// clear colour, the edges and the camera unchanged, in Technical with TAA, the still image and idle
// all off -- so the shading itself, which RenderBench never reproduced. Every lane the surfaces'
// light reads that is not the camera's is compared with the last frame, and a change is logged with
// both values: a lane that flips for one frame shows as a pair of lines. 20 lines a second at most.
void DiligentScene::Impl::NoteLightChange (const DiligentSceneConstants& constants, const ClayLook& look,
                                           bool drawSurfaces)
{
    static const char* const kNames[] = { "sunDir.x",     "sunDir.y",
                                          "sunDir.z",     "ambient",
                                          "sky.r",        "sky.g",
                                          "sky.b",        "sunStrength",
                                          "ground.r",     "ground.g",
                                          "ground.b",     "exposure",
                                          "reflectance",  "roughnessBias",
                                          "aoIntensity",  "wb.r",
                                          "wb.g",         "wb.b",
                                          "frame.hdr",    "frame.coverage",
                                          "frame.focus",  "frame.focusOpacity",
                                          "sunWithSky",   "envIntensity",
                                          "envActive",    "shadows",
                                          "clay",         "clay.r",
                                          "drawSurfaces", "renderQuality",
                                          "baseColor.r" };
    const std::vector<float> now = {
        constants.sunDir[0],        constants.sunDir[1],         constants.sunDir[2],         constants.ambient,
        constants.skyColor[0],      constants.skyColor[1],       constants.skyColor[2],       constants.skyColor[3],
        constants.groundColor[0],   constants.groundColor[1],    constants.groundColor[2],    constants.gradeParams[0],
        constants.gradeParams[1],   constants.gradeParams[2],    constants.gradeParams[3],    constants.whiteBalance[0],
        constants.whiteBalance[1],  constants.whiteBalance[2],   constants.frameControl[0],   constants.frameControl[1],
        constants.frameControl[2],  constants.frameControl[3],   constants.materialParams[1], constants.envParams[0],
        constants.envParams[2],     constants.shadowParams[0],   look.enabled ? 1.0f : 0.0f,  look.r,
        drawSurfaces ? 1.0f : 0.0f, float (int (renderQuality)), constants.baseColor[0]
    };
    static_assert (sizeof (kNames) / sizeof (kNames[0]) == 31, "a name for every lane");
    if (lightSignature.size () == now.size ()) {
        std::string changed;
        for (size_t k = 0; k < now.size (); ++k)
            if (!(std::abs (now[k] - lightSignature[k]) <= 1e-4f * (std::max) (1.0f, std::abs (now[k]))))
                changed += std::string (" ") + kNames[k] + " " + std::to_string (lightSignature[k]) + "->" +
                           std::to_string (now[k]);
        if (!changed.empty ()) {
            const int64_t second =
                std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now ().time_since_epoch ())
                    .count ();
            if (second != lightLogSecond) {
                lightLogSecond = second;
                lightLogLines = 0;
            }
            if (++lightLogLines <= 20)
                ArchVizLog ("Diligent scene: light changed:" + changed);
        }
    }
    lightSignature = now;
}

void DiligentScene::WatchFrameFlash (Diligent::IDeviceContext* context, Diligent::ITextureView* target,
                                     Diligent::ITextureView* depth, uint64_t frameIndex)
{
    if (impl_ == nullptr || !impl_->ready)
        return;
    FrameFlashState state;
    state.frame = frameIndex;
    state.picksIssued = PickRequestsIssued ();
    state.drawCalls = impl_->drawCalls;
    state.meshCommits = impl_->meshCommits;
    state.stillPhase = int (impl_->temporalConvergence.Phase ());
    state.idle = impl_->idleLastFrame;
    state.taa = impl_->taaResolvedThisFrame;
    impl_->frameFlash.Sample (impl_->device, context, target, depth, state);
}

void DiligentScene::Impl::BeginFrameContent (const DiligentSceneConstants& constants, const ClayLook& look,
                                             bool drawSurfaces)
{
    NoteTextureLane (); // #252 diagnostic: closes the previous frame's textured-range count
    NoteLightChange (constants, look, drawSurfaces);
    frameContent = ContentKey ();
    DiligentSceneConstants key = constants;
    std::memset (key.viewProj, 0, sizeof (key.viewProj));
    std::memset (key.motionViewProj, 0, sizeof (key.motionViewProj));
    std::memset (key.prevViewProj, 0, sizeof (key.prevViewProj));
    key.shadowParams[1] = 0.0f;
    // ⚠️ THE AO LANE SAYS WHETHER AO RAN THIS FRAME, which an idle frame (no prepass) cannot know;
    // the settings go in here and whether it ran is late content (StepTemporalConvergence).
    key.gradeParams[3] = 0.0f;
    frameContent.Add (&key, sizeof (key));
    frameContent.Add (sectionPlane, sizeof (sectionPlane));
    frameContent.Add (aoEnabled);
    frameContent.Add (aoIntensity);
    frameContent.Add (rayTraced.settings.enabled);
    frameContent.Add (rayTraced.settings.reflections);
    frameContent.Add (rayTraced.settings.skyLight);
    frameContent.Add (materialTexturesEnabled); // #252
    frameContent.Add (aoRadius);
    frameContent.Add (ssrEnabled);
    frameContent.Add (ssrIntensity);
    frameContent.Add (ssrRoughnessThreshold);
    frameContent.Add (ssrTuning, sizeof (ssrTuning));
    frameContent.Add (atmosphereEnabled);
    frameContent.Add (atmosphereLightShafts);
    frameContent.Add (atmosphereLightingOnly);
    frameContent.Add (atmosphereIntensity);
    frameContent.Add (taaStability);
    frameContent.Add (uint64_t (renderMode));
    const DiligentShadowSettings& shadow = shadowMap.Settings ();
    frameContent.Add (uint64_t (shadow.mode));
    frameContent.Add (uint64_t (shadow.resolution) << 32 | shadow.cascadeCount);
    frameContent.Add (shadow.shadowsOnly);
    frameContent.Add (shadow.visualizeCascades);
    frameContent.Add (shadow.fitCascadesToScene);
    frameContent.Add (shadow.pcssEnabled);
    frameContent.Add (uint64_t (int64_t (shadow.diagnostic)));
    frameContent.Add (uint64_t (int64_t (shadow.fixedFilterSize)));
    for (float v : { shadow.partitioningFactor, shadow.fixedDepthBias, shadow.receiverPlaneBiasClamp,
                     shadow.cascadeTransition, shadow.filterWorldSize, shadow.evsmPositiveExponent,
                     shadow.evsmNegativeExponent, shadow.lightBleedingReduction, shadow.vsmBias,
                     shadow.pcssLightAngularDiameter, shadow.pcssBlockerSearch, shadow.pcssMaxPenumbra })
        frameContent.Add (v);

    // Every range Draw is about to draw, as it will draw it (its loop, in the same order). Done
    // here, before anything is drawn, so an idle frame can tell an unchanged scene from a changed one.
    if (drawSurfaces) {
        for (const Entry& e : staticMeshes)
            if (e.vertexBuffer != nullptr && e.indexBuffer != nullptr)
                NoteDrawnContent (e, nullptr, nullptr);
        for (const Entry& e : elements) {
            if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr || e.hidden)
                continue;
            for (const MaterialRange& r : e.ranges) {
                if (r.indexCount == 0)
                    continue;
                const SurfaceMaterial& mat = materials.Lookup (r.material);
                const DrawnSurface drawn = ResolveDrawnSurface (mat, PresetFor (mat), look);
                NoteDrawnContent (e, &r, &drawn);
                frameContent.Add (mat.emission, sizeof (mat.emission));
                frameContent.Add (mat.textureKey);           // #252: a texture swapped under an unchanged colour
                frameContent.Add (mat.textureRotation.cosA); // #252: or turned
                frameContent.Add (mat.textureRotation.sinA);
            }
        }
    }
    earlyKey = frameContent.Value ();
}

// What was drawn, where, and as what. The buffer identity and the topology hash catch a
// re-extracted element; the resolved surface catches materials, clay and the study model; an
// element hidden or removed simply stops contributing.
void DiligentScene::Impl::NoteDrawnContent (const Entry& entry, const MaterialRange* range, const DrawnSurface* surface)
{
    frameContent.Add (uint64_t (reinterpret_cast<uintptr_t> (entry.vertexBuffer.RawPtr ())));
    frameContent.Add (entry.topologyHash);
    frameContent.Add (uint64_t (entry.indexCount));
    if (range != nullptr) {
        frameContent.Add (uint64_t (uint32_t (range->material)) << 32 | range->firstIndex);
        frameContent.Add (uint64_t (range->indexCount));
    }
    if (surface != nullptr) {
        for (float v : { surface->r, surface->g, surface->b, surface->alpha, surface->roughness, surface->reflectance,
                         surface->metallic })
            frameContent.Add (v);
    }
}

// ---- #248: the overlays on an unjittered depth -------------------------------------------------
//
// Everything after the resolve -- the sun-study tint, the section's back faces and caps, the
// wireframe, the technical and selection edges, the outlines, and the viewport's previews and
// labels -- is drawn once per frame and never averaged. Drawn with the jittered camera, each of
// them moves by up to half a pixel every frame, which is the edge crawl #248 reports. Drawn with
// the unjittered camera against the JITTERED depth it would be worse: the wireframe and the tint
// test against the surface they lie on with no pull, and half a pixel of depth slope fails them.
//
// So on a jittered frame the opaque depth is drawn again, unjittered, with the shaded pass's own
// vertex shader, cull and opaque partition, and the passes after it use the same camera. TAA and
// SSR have already read the jittered depth by then. The point splats are not redrawn: overlays
// draw over them, which is what they did over the jitter's empty half-pixels already.
bool DiligentScene::EnsureStableDepthPipelines ()
{
    if (impl_->stableDepthPso[0] != nullptr)
        return true;
    if (impl_->stableDepthInitFailed || impl_->device == nullptr || impl_->vs == nullptr || impl_->flatPs == nullptr)
        return false;
    impl_->stableDepthInitFailed = true; // until it has succeeded

    // The mesh layout every mesh pipeline uses (ArchVizVertex).
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False },
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },
    };
    for (int i = 0; i < 3; ++i) {
        const CullMode cull = i == 0 ? CullMode::Ccw : (i == 1 ? CullMode::Cw : CullMode::None);
        Diligent::GraphicsPipelineStateCreateInfo pci;
        pci.PSODesc.Name = "ArchViz unjittered overlay depth PSO";
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
        // ⚠️ ONE TARGET, MASKED OFF, for the occlusion prepass's reason: the frame's colour view
        // stays bound, and a PSO declaring none against it fails Diligent's validation.
        gp.NumRenderTargets = 1;
        gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (impl_->initColorFormat);
        gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (impl_->initDepthFormat);
        gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        // ⚠️ THE SHADED PASS'S CULL AND DEPTH TEST (LESS), not the occlusion prepass's CULL_NONE:
        // inside a cut solid that pass shows the far side, and the depth must say the same.
        gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
        gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_SOLID;
        gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;
        gp.DepthStencilDesc.DepthEnable = Diligent::True;
        gp.DepthStencilDesc.DepthWriteEnable = Diligent::True;
        gp.BlendDesc.RenderTargets[0].BlendEnable = Diligent::False;
        gp.BlendDesc.RenderTargets[0].RenderTargetWriteMask = Diligent::COLOR_MASK_NONE;
        gp.InputLayout.LayoutElements = layout;
        gp.InputLayout.NumElements = _countof (layout);
        pci.pVS = impl_->vs;
        pci.pPS = impl_->flatPs;
        impl_->device->CreateGraphicsPipelineState (pci, &impl_->stableDepthPso[i]);
        if (impl_->stableDepthPso[i] == nullptr)
            break;
        const Diligent::SHADER_TYPE stages[2] = { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL };
        for (Diligent::SHADER_TYPE stage : stages)
            if (Diligent::IShaderResourceVariable* variable =
                    impl_->stableDepthPso[i]->GetStaticVariableByName (stage, "ArchVizConstants"))
                variable->Set (impl_->constants);
        impl_->stableDepthPso[i]->CreateShaderResourceBinding (&impl_->stableDepthSrb[i], true);
        if (impl_->stableDepthSrb[i] == nullptr)
            break;
    }
    if (impl_->stableDepthSrb[2] == nullptr) {
        for (int i = 0; i < 3; ++i) {
            impl_->stableDepthSrb[i].Release ();
            impl_->stableDepthPso[i].Release ();
        }
        ArchVizLog ("Diligent scene: the unjittered overlay depth is unavailable -- overlays keep the jitter");
        return false;
    }
    impl_->stableDepthInitFailed = false;
    return true;
}

void DiligentScene::UnjitterOverlays (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                      CullMode cull, Diligent::ITextureView* colorTarget,
                                      Diligent::ITextureView* depthTarget, bool drawSurfaces)
{
    if (impl_ != nullptr) {
        // #14: what the line overlays draw into, so the Technical edges can copy the depth and rebind.
        impl_->overlayColor = colorTarget;
        impl_->overlayDepth = depthTarget;
    }
    if (impl_ == nullptr || context == nullptr || depthTarget == nullptr ||
        !impl_->temporalConvergenceSettings.stableOverlays)
        return;
    // Not jittered (TAA off, Fast quality, a debug view, a converged mean): nothing to undo.
    if (std::memcmp (constants.viewProj, constants.motionViewProj, sizeof (constants.viewProj)) == 0)
        return;
    // Without the pipeline the overlays keep the jittered camera that matches the depth they test.
    if (drawSurfaces && !EnsureStableDepthPipelines ())
        return;
    std::memcpy (constants.viewProj, constants.motionViewProj, sizeof (constants.viewProj));
    if (drawSurfaces) {
        impl_->Upload (context, constants);
        DrawStableDepth (context, cull, depthTarget);
    }
    impl_->overlaysUnjittered = true;
}

// The shaded pass's opaque set, depth-only, with the camera the uploaded constants hold.
void DiligentScene::DrawStableDepth (Diligent::IDeviceContext* context, CullMode cull,
                                     Diligent::ITextureView* depthTarget)
{
    {
        const int index = CullIndex (cull);
        context->ClearDepthStencil (depthTarget, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState (impl_->stableDepthPso[index]);
        context->CommitShaderResources (impl_->stableDepthSrb[index],
                                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        // The shaded pass's opaque set: every helper mesh, and each element's opaque ranges.
        for (const Entry& e : impl_->staticMeshes) {
            if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            BindMesh (context, e);
            for (const MaterialRange& r : e.ranges) {
                DrawEntryRange (context, e, r);
                ++impl_->drawCalls;
            }
        }
        const ClayLook look = impl_->SurfaceLook (impl_->StudyModelShown ());
        for (const Entry& e : impl_->elements) {
            if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr || e.hidden)
                continue;
            bool bound = false;
            for (const MaterialRange& r : e.ranges) {
                if (r.indexCount == 0 || DrawnAlpha (impl_->materials.Lookup (r.material), look) < kOpaqueAlpha)
                    continue;
                if (!bound) {
                    BindMesh (context, e);
                    bound = true;
                }
                DrawEntryRange (context, e, r);
                ++impl_->drawCalls;
            }
        }
    }
}

// ---- #247: idle once converged -----------------------------------------------------------------
//
// A converged mean is final: while the camera and the content key hold, every later frame would
// render the same scene only to throw the result away and present the mean again. So the shadow
// map and the G-buffer prepass (both skip on `idleThisFrame`) and every pass Draw makes up to the
// resolve are skipped. What remains is a depth-only redraw (the overlays test against it), the
// resolve of the mean with the constants its converged frame uploaded, and the overlays.
//
// ⚠️ THE KEY IS CHECKED HERE, AFTER THE SHADOW MAP AND THE PREPASS WERE ALREADY SKIPPED. A change
// found now (a sun moved, a material edited, an element re-sent) cannot be drawn properly in this
// frame, so the mean is presented once more and the convergence restarts: the next frame renders
// in full. One frame late, never wrong for longer.
bool DiligentScene::PresentIdleFrame (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                      Diligent::ITextureView* colorTarget, Diligent::ITextureView* depthTarget,
                                      CullMode cull, bool drawSurfaces)
{
    const bool idle = impl_->idleThisFrame;
    impl_->idleThisFrame = false;
    impl_->idleLastFrame = false;
    if (!idle || context == nullptr || colorTarget == nullptr || impl_->accumSRV == nullptr ||
        impl_->resolvePso == nullptr || impl_->resolveSrb == nullptr)
        return false;
    if (impl_->earlyKey != impl_->idleKey) {
        impl_->temporalConvergence.Reset (TemporalRestart::Content);
        impl_->idleEligible = false;
        ArchVizLog ("Diligent scene: still image restarted -- the content changed while idle");
    }
    // ⚠️ THE RESOLVE'S CONSTANTS WITH THIS FRAME'S CAMERA. The frame that converged was the last
    // jittered one, so its `viewProj` is jittered; the depth drawn here must match the overlays,
    // which draw unjittered.
    DiligentSceneConstants idleConstants = impl_->resolveConstants;
    std::memcpy (idleConstants.viewProj, constants.motionViewProj, sizeof (idleConstants.viewProj));
    impl_->Upload (context, idleConstants);
    context->SetRenderTargets (1, &colorTarget, depthTarget, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetViewports (1, nullptr, 0, 0);
    if (depthTarget != nullptr && drawSurfaces && EnsureStableDepthPipelines ())
        DrawStableDepth (context, cull, depthTarget);
    for (const char* name : { "g_hdrColor", "g_hdrCoverage" })
        if (Diligent::IShaderResourceVariable* var =
                impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, name))
            var->Set (impl_->accumSRV);
    context->SetPipelineState (impl_->resolvePso);
    context->CommitShaderResources (impl_->resolveSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DrawAttribs resolve;
    resolve.NumVertices = 3;
    resolve.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->Draw (resolve);
    ++impl_->drawCalls;
    // What follows the resolve reads the constants as a resolved frame leaves them.
    constants.frameControl[0] = 0.0f;
    constants.frameControl[1] = 2.0f;
    impl_->idleLastFrame = true;
    return true;
}

bool DiligentScene::OverlaysUnjittered () const
{
    return impl_ != nullptr && impl_->overlaysUnjittered;
}

} // namespace archviz
} // namespace geomsrv
