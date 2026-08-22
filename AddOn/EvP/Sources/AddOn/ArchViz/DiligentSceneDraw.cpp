// The scene's PASSES: the sun's depth pass, the main opaque/transparent draw and
// the corner overlay. Everything here binds and submits; nothing here creates a
// GPU resource. The pipeline states live in DiligentScene.cpp and the element map
// in DiligentSceneGeometry.cpp; DiligentSceneImpl.hpp says why the class is
// spread over three files.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog -- a failed bind must say so, not crash
#include "ArchViz/AutoExposure.hpp"
#include "ArchViz/SurfaceClassifier.hpp"

#include <algorithm> // std::min, for the selection alpha against a glass material
#include <cstring>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

// The constants, uploaded. Shared by every pass so the light matrix, the sun and
// the shadow parameters cannot be right in one pass and stale in another.
//
// ⚠️ UNMAP BEFORE THE DRAW, unconditionally. A dynamic constant buffer still
// mapped when the draw is issued is a silent no-draw on some drivers rather than
// an error.


} // namespace

bool DiligentScene::RenderShadowMap (Diligent::IDeviceContext* context)
{
    if (impl_ != nullptr)
        impl_->shadow = SunShadow {};
    if (context == nullptr || !impl_->ready || !impl_->shadowMap.IsReady ())
        return false;

    // ⚠️ FITTED TO THE ELEMENTS ONLY. Anything larger spends the shadow map's
    // resolution on empty space, and a building's shadow rendered through a
    // quarter of the texels it should have reads as a low-quality shadow rather
    // than as a fitting mistake.
    float boundsMin[3];
    float boundsMax[3];
    if (!SceneBounds (boundsMin, boundsMax))
        return false;

    // A little headroom below the model, so a surface exactly at its lowest
    // point is inside the light's depth range rather than on its boundary.
    boundsMin[2] -= 0.05f;

    // ⚠️ THE SAME SUN THE PIXEL SHADER WILL USE. Fitting the shadow to Archicad's
    // sun while shading with the override would put the shadow somewhere the
    // light plainly is not coming from -- which reads as a broken shadow map
    // rather than as two sources of one value.
    float shadowSun[3];
    impl_->EffectiveSun (shadowSun);
    const SunShadow fit = FitSunShadow (boundsMin, boundsMax, shadowSun, impl_->shadowMap.Resolution ());
    if (!fit.valid)
        return false;
    impl_->shadow = fit;

    DiligentSceneConstants constants;
    std::memcpy (constants.lightViewProj, fit.lightViewProj, sizeof (float) * 16);
    UploadConstants (context, impl_->constants, constants);

    impl_->shadowMap.Begin (context);

    // ⚠️ OPAQUE RANGES ONLY. A pane of glass that writes into the shadow map
    // casts a solid black shadow, and a curtain-walled facade then shadows the
    // whole floor plate behind it -- the same fault as a transparent surface
    // writing depth in the main pass, in a place where it is much harder to
    // recognise.
    auto shadowCastRanges = [&] (const Entry& e, bool consultMaterials) {
        if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
            return;
        BindMesh (context, e);
        for (const MaterialRange& r : e.ranges) {
            if (r.indexCount == 0)
                continue;
            if (consultMaterials && impl_->materials.Lookup (r.material).alpha < kOpaqueAlpha)
                continue;
            DrawEntryRange (context, e, r);
        }
    };

    for (const Entry& e : impl_->elements)
        shadowCastRanges (e, true);
    for (const Entry& e : impl_->staticMeshes)
        shadowCastRanges (e, false);
    // ⚠️ THE OVERLAY GNOMON DOES NOT CAST -- it is not in the world.

    impl_->shadowMap.End (context);
    return true;
}

void DiligentScene::DrawIds (Diligent::IDeviceContext* context, const float viewProj[16], CullMode cull)
{
    if (context == nullptr || !impl_->ready)
        return;

    // ⚠️ NO SetViewports HERE. DiligentPickBuffer::Begin has already set the id
    // target's; the main pass's `SetViewports (1, nullptr, 0, 0)` would take the
    // dimensions Diligent last recorded for the bound target and is exactly what
    // must not happen in this pass.
    const int index = CullIndex (cull);
    context->SetPipelineState (impl_->pickPso[index]);
    context->CommitShaderResources (impl_->pickSrb[index], Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);

    // ⚠️ ELEMENTS ONLY. The debug cube and the gnomon have no GUID, so an id for
    // them could only map back to nothing -- and giving them one would let a
    // click on the gnomon in the corner deselect whatever is selected, which
    // reads as picking being unreliable.
    for (const Entry& e : impl_->elements) {
        if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr || e.pickId == kNoPickId)
            continue;

        // The id, unpacked to three 0..1 channels. 24 bits is 16.7M elements --
        // four orders of magnitude past any project -- and against an
        // RGBA8_UNORM target each channel round-trips to its exact byte.
        constants.baseColor[0] = float ((e.pickId >> 16) & 0xFF) / 255.0f;
        constants.baseColor[1] = float ((e.pickId >> 8) & 0xFF) / 255.0f;
        constants.baseColor[2] = float (e.pickId & 0xFF) / 255.0f;
        constants.baseColor[3] = 1.0f;
        UploadConstants (context, impl_->constants, constants);

        // ⚠️ THE WHOLE ELEMENT IN ONE DRAW, ranges ignored. The id is a property
        // of the element, and drawing 0..indexCount covers every range because
        // the ranges partition exactly that span.
        BindMesh (context, e);
        const MaterialRange whole { -1, 0, e.indexCount };
        DrawEntryRange (context, e, whole);
    }
}

void DiligentScene::DrawOverlay (Diligent::IDeviceContext* context, const float viewProj[16], const float eye[3])
{
    if (context == nullptr || !impl_->ready || impl_->overlayMeshes.empty ())
        return;

    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);
    constants.eyePos[0] = eye[0];
    constants.eyePos[1] = eye[1];
    constants.eyePos[2] = eye[2];
    // Debug view Final, no shadow, and a high ambient: the gnomon's job is to
    // be READ, and shading it with the scene's sun makes one arm of three
    // disappear whenever the sun is behind it.
    constants.debugView = 0.0f;
    constants.shadowParams[2] = 0.0f;
    constants.ambient = 0.75f;
    constants.sunDir[0] = 0.3f;
    constants.sunDir[1] = -0.4f;
    constants.sunDir[2] = 0.87f;
    constants.skyColor[0] = constants.skyColor[1] = constants.skyColor[2] = 1.0f;
    constants.groundColor[0] = constants.groundColor[1] = constants.groundColor[2] = 0.6f;
    UploadConstants (context, impl_->constants, constants);

    const int index = CullIndex (CullMode::Cw);
    context->SetPipelineState (impl_->opaquePso[index]);
    context->CommitShaderResources (impl_->opaqueSrb[index], Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    for (const Entry& e : impl_->overlayMeshes) {
        if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
            continue;
        BindMesh (context, e);
        for (const MaterialRange& r : e.ranges)
            DrawEntryRange (context, e, r);
    }
}

void DiligentScene::SetViewportSize (uint32_t width, uint32_t height)
{
    if (impl_ == nullptr)
        return;
    impl_->viewportWidth = width;
    impl_->viewportHeight = height;
}

void DiligentScene::Draw (Diligent::IDeviceContext* context, Diligent::ITextureView* colorTarget,
                          Diligent::ITextureView* depthTarget, const float view[16], const float proj[16],
                          const float viewProj[16], const float motionViewProj[16], const float eye[3],
                          const float jitter[2], CullMode cull, int debugView, float nearClip,
                          float farClip, float focusDistance, uint32_t frameIndex)
{
    if (context == nullptr || !impl_->ready)
        return;

    // ⚠️ NO SILENT FALLBACK TO INHERITING. A version of this that quietly used
    // whatever happened to be bound is exactly what produced the grey viewport,
    // so a missing target is reported and the frame is skipped instead. It
    // cannot happen from the one caller; if it ever does, the log says so rather
    // than the picture.
    if (colorTarget == nullptr) {
        ArchVizLog ("ArchViz Draw: no colour target was supplied; the frame is skipped. The caller "
                    "must pass the view it wants drawn into -- Draw no longer inherits a binding.");
        return;
    }

    // ---- the HDR scene-colour path (RE51.C7 prerequisite) -------------------
    //
    // ⚠️ REALISTIC + FINAL VIEW ONLY. Fast quality has no tone mapping to move,
    // and every debug view answers a question about the model's own data, not
    // about the finished image. Routing those through the HDR target and resolve
    // would tone-map the normals or the roughness, which is exactly what the
    // debug views exist to avoid.
    //
    // ⚠️ IF THE HDR TARGET CANNOT BE ALLOCATED, FALL BACK TO LDR. A resize that
    // fails to create the offscreen texture must not take the whole frame with
    // it; the LDR path produces a correct image, just one where each shader
    // tone-maps individually rather than in one resolve.
    const bool useHdr = (impl_->renderQuality == RenderQuality::Realistic && debugView == 0 &&
                         EnsureHdrTarget ());

    // ⚠️ BOUND HERE, NOT INHERITED. See the header: two passes inside this call
    // rebind the render targets for their own purposes, and inheriting the
    // frame loop's binding meant everything after the first of them drew into
    // nothing at all -- silently.
    //
    // ⚠️ THE HDR PATH BINDS THE OFFSCREEN TARGET, NOT THE SWAP CHAIN. The
    // resolve pass at the end copies back to the swap chain with Grade() applied
    // in one place. The LDR path binds the swap chain directly, as before.
    Diligent::ITextureView* sceneColorTarget = useHdr ? impl_->hdrColorRTV : colorTarget;
    const auto bindFrameTargets = [&] () {
        context->SetRenderTargets (1, &sceneColorTarget, depthTarget,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        // ⚠️ AFTER THE BIND, ALWAYS. SetRenderTargets resets the viewport
        // whenever the framebuffer actually changes.
        //
        // ⚠️ AND (1, nullptr, 0, 0), NOT (0, ...). Zero means "bind no viewports
        // at all" and every triangle is clipped away while the clear still
        // happens -- which reads exactly like a renderer that draws nothing. It
        // cost TransparencyProbe a full run; see PatternRenderer.cpp -> Render.
        context->SetViewports (1, nullptr, 0, 0);
    };
    bindFrameTargets ();

    // ⚠️ CLEAR THE HDR TARGET TO TRANSPARENT BLACK. The resolve pass discards
    // alpha==0 pixels, so the swap chain's own clear (grey for the palette,
    // transparent for the overlay) survives wherever no geometry was drawn.
    if (useHdr && impl_->hdrColorRTV != nullptr) {
        const float hdrClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearRenderTarget (impl_->hdrColorRTV, hdrClear,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    const int index = CullIndex (cull);
    impl_->drawCalls = 0;

    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);
    std::memcpy (constants.motionViewProj, motionViewProj, sizeof (float) * 16);
    // ⚠️ THE FORWARD PASS DOES NOT *USE* THIS, AND IT IS UPLOADED ANYWAY. The
    // mesh vertex shader is shared with the G-buffer pass, so it computes
    // `prevClip` here too; the forward pixel shader simply never reads it.
    // Leaving the field at its identity default would work today and would hand
    // the first consumer added to this path -- TAA, motion blur, anything
    // temporal -- a previous camera at the world origin, silently.
    std::memcpy (constants.prevViewProj, impl_->havePrevViewProj ? impl_->prevViewProj : viewProj,
                 sizeof (float) * 16);
    impl_->EffectiveSun (constants.sunDir);
    constants.ambient = impl_->ambient;
    constants.eyePos[0] = eye[0];
    constants.eyePos[1] = eye[1];
    constants.eyePos[2] = eye[2];
    constants.debugView = float (debugView);

    // ⚠️ THE LIGHT MATRIX MUST BE THE ONE THE MAP WAS RENDERED WITH, this frame.
    // Re-fitting it here would use the same inputs and usually agree, and would
    // silently disagree on the frame where geometry arrived between the two
    // calls -- a shadow that slides off the building for one frame per element
    // during a live extraction. RenderShadowMap stores it; this reads it.
    std::memcpy (constants.lightViewProj, impl_->shadow.lightViewProj, sizeof (float) * 16);
    constants.shadowParams[0] = impl_->shadow.texelWorldSize;
    constants.shadowParams[1] = impl_->shadow.depthRange;
    constants.shadowParams[2] = impl_->shadow.valid && impl_->shadowMap.IsReady () ? 1.0f : 0.0f;
    constants.shadowParams[3] =
        impl_->shadowMap.Resolution () > 0 ? 1.0f / float (impl_->shadowMap.Resolution ()) : 0.0f;

    // The silhouette's offset, in NDC. ⚠️ THE FACTOR OF 2 IS THE WHOLE NDC RANGE:
    // clip x runs -1..1 over `width` pixels, so one pixel is 2/width. Dropping it
    // halves every outline, which looks like a tuning choice rather than an
    // arithmetic slip.
    constants.outlineParams[0] =
        impl_->viewportWidth > 0 ? kSelectionOutlinePixels * 2.0f / float (impl_->viewportWidth) : 0.0f;
    constants.outlineParams[1] =
        impl_->viewportHeight > 0 ? kSelectionOutlinePixels * 2.0f / float (impl_->viewportHeight) : 0.0f;
    // The render quality, riding the same float4's spare lane -- see
    // DiligentSceneConstants. The pixel shader branches on it; no second PSO.
    constants.outlineParams[2] = (impl_->renderQuality == RenderQuality::Realistic) ? 1.0f : 0.0f;

    // ---- the HDR sky --------------------------------------------------------
    //
    // ⚠️ THE LOAD HAPPENS HERE, ON THE RENDER THREAD, NOT WHERE IT WAS ASKED
    // FOR. `SetEnvironmentMap` is called from the bus thread and only parks the
    // path: reading a file and calling UpdateTexture/GenerateMips on the device
    // context from another thread is exactly the kind of race that produces a
    // crash somewhere else entirely, minutes later.
    if (impl_->environmentLoadPending) {
        impl_->environmentLoadPending = false;
        std::string environmentError;
        if (impl_->pendingEnvironmentPath.empty ()) {
            impl_->environment.Clear ();
            impl_->environmentError.clear ();
        }
        else if (impl_->environment.Load (impl_->device, context, impl_->pendingEnvironmentPath.c_str (),
                                          environmentError)) {
            impl_->environmentError.clear ();
        }
        else {
            // ⚠️ THE PREVIOUS SKY SURVIVES A FAILED LOAD (EnvironmentMap::Load
            // commits nothing until it has succeeded), so the message is the
            // only evidence -- which is why it is kept rather than logged and
            // dropped. It is reported through the stats.
            impl_->environmentError = environmentError;
        }
        // ⚠️ THE LOAD RENDERS. EnvironmentMap::Load runs the GGX prefilter
        // (RE51.B6), which binds each mip of the environment map as a render
        // target in turn and leaves the LAST one -- a 1x1 texel -- bound. Every
        // draw after this point in the frame would go there. One frame, on the
        // frame an HDR arrives, and it would also have corrupted that mip.
        bindFrameTargets ();
    }

    const bool envActive =
        impl_->environment.IsLoaded () && impl_->environmentEnabled && impl_->environmentIntensity > 0.0f;
    if (envActive)
        impl_->environment.CopyShCoefficients (constants.sh);
    constants.envParams[0] = impl_->environmentIntensity;
    constants.envParams[1] = impl_->environmentRotationRadians;
    constants.envParams[2] = envActive ? 1.0f : 0.0f;
    constants.envParams[3] = float (impl_->environment.MipLevels ());
    std::memcpy (constants.envRayRight, impl_->cameraRayRight, sizeof (float) * 3);
    std::memcpy (constants.envRayUp, impl_->cameraRayUp, sizeof (float) * 3);
    std::memcpy (constants.envRayForward, impl_->cameraRayForward, sizeof (float) * 3);

    // ⚠️ THE GRADING IS SET *BEFORE* THE SKY PASS, NOT WITH THE REST OF THE
    // MATERIAL CONSTANTS. The background shader finishes through the same
    // `Grade` the model does -- deliberately, so the two cannot drift -- and it
    // uploads the constant buffer several lines below. Setting the exposure or
    // the white balance after that point leaves the SKY on the previous frame's
    // values while the building is on this frame's, which reads as the sky
    // lagging the exposure slider by one frame and is very easy to dismiss.
    //
    // ⚠️ `materialParams[1]` MOVED UP HERE WITH IT, because the estimate reads
    // the sun's share out of the buffer rather than from impl_.
    constants.materialParams[1] = impl_->sunWithSkyWeight;
    // ---- RE51.B9: the exposure the light implies, and the white balance ------
    //
    // ⚠️ COMPUTED EVERY FRAME EVEN WHEN IT IS NOT APPLIED. The estimate has one
    // calibration constant and no live measurement behind it yet, so it ships
    // switched off -- but a control nobody can see the effect of is a control
    // nobody can calibrate. Computing it regardless costs a few dozen floating
    // point operations and puts the answer in the stats and the HUD, so ONE run
    // settles whether middle grey is right for this project.
    //
    // ⚠️ THE INPUTS ARE READ FROM `constants`, NOT RE-DERIVED FROM impl_. The
    // estimate has to describe the frame that is about to be drawn; taking the
    // sun weight or the SH from anywhere other than the buffer being uploaded is
    // how it would end up metering a scene nobody rendered.
    {
        SceneLight light;
        light.environmentActive = envActive;
        for (int c = 0; c < 3; ++c) {
            // ⚠️ 0.282095 IS ShDiffuse's DC BASIS FUNCTION, and applying it here
            // is what makes this the AVERAGE of ShDiffuse over all normals
            // rather than the raw coefficient. Dropping it over-estimates the
            // sky by 3.5x, which would under-expose every image by that much.
            light.skyIrradiance[c] = constants.sh[0][c] * 0.282095f * constants.envParams[0];
            light.ambientSky[c] = constants.skyColor[c];
            light.ambientGround[c] = constants.groundColor[c];
        }
        light.sunStrength = constants.skyColor[3];
        light.sunWeight = constants.materialParams[1];
        // ⚠️ `ambient` IS THE FOURTH LANE OF g_sunAndAmbient ON THE SHADER SIDE,
        // and a separate member on this one. Same value, two names -- see
        // DiligentSceneConstants, where sunDir[3] and ambient are one float4.
        light.ambientShare = constants.ambient;
        light.meanAlbedo = MeanPoolAlbedo (impl_->materials);

        impl_->lastSceneLuminance = MeanSceneLuminance (light);
        impl_->lastAutoExposure = AutoExposure (light);
    }
    constants.gradeParams[0] = impl_->autoExposureEnabled ? impl_->lastAutoExposure : impl_->exposure;

    // ⚠️ THE HDR FLAG RIDES THE CBUFFER, NOT A SECOND BIND. When this is 1, the
    // mesh PS and the env background PS skip Grade() and write raw radiance;
    // the resolve pass at the end applies it once. Set once here and it rides
    // through every per-range upload, exactly as materialParams[1] does.
    constants.frameControl[0] = useHdr ? 1.0f : 0.0f;

    const WhiteBalanceGains balance =
        ComputeWhiteBalance (impl_->whiteBalanceKelvin, impl_->whiteBalanceTint);
    for (int c = 0; c < 3; ++c)
        constants.whiteBalance[c] = balance.rgb[c];

    // ---- the sky behind the model, before anything else ----------------------
    //
    // ⚠️ FINAL VIEW ONLY. Every other debug view answers a question about the
    // model, and a photographic sky behind a normals or roughness read makes it
    // harder, not easier, to see what the view exists to show.
    if (envActive && impl_->environmentBackground && debugView == 0 && impl_->envBackgroundPso != nullptr) {
        UploadConstants (context, impl_->constants, constants);
        // ⚠️ HDR PATH USES THE HDR ENV BACKGROUND PSO, which writes raw radiance
        // into the RGBA16_FLOAT target. The LDR path uses the original PSO that
        // tone-maps inline into the swap chain's sRGB view.
        Diligent::IPipelineState* skyPso = useHdr ? impl_->hdrEnvBackgroundPso : impl_->envBackgroundPso;
        Diligent::IShaderResourceBinding* skySrb = useHdr ? impl_->hdrEnvBackgroundSrb : impl_->envBackgroundSrb;
        context->SetPipelineState (skyPso);
        context->CommitShaderResources (skySrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawAttribs sky;
        sky.NumVertices = 3;
        sky.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw (sky);
        ++impl_->drawCalls;
    }

    // ⚠️ THE ROUGHNESS DEFAULTS TO MATTE, AND EVERY CALLER THAT TAKES THE
    // DEFAULT WANTS IT. Four of the five other call sites (wireframe, selection
    // silhouette, hover silhouette, id pick) draw through kArchVizFlatPS, which
    // has no lighting to apply it to; the fifth is the helper meshes -- gnomon,
    // ground plane, debug cube -- which are instruments rather than
    // architecture and must not acquire a highlight that could be mistaken for
    // one on the model.
    // ⚠️ `specular` DEFAULTS TO 0.5, NOT 0. It is the neutral dielectric, and
    // the shader's F0 = 0.08 * specular maps it onto the 0.04 every surface used
    // before the channel was read. Defaulting to 0 would strip the highlight off
    // the wireframe, the outlines and the helper meshes -- everything that
    // uploads a colour without a material behind it.
    // Set once, before the range loop: it is a frame value riding in the
    // per-range float4. See DiligentSceneConstants::materialParams.
    constants.gradeParams[1] = impl_->reflectance;
    constants.gradeParams[2] = impl_->roughnessBias;


    auto uploadConstants = [&] (float r, float g, float b, float a, float roughness = 1.0f, float specular = 0.5f,
                                float metallic = 0.0f) {
        constants.baseColor[0] = r;
        constants.baseColor[1] = g;
        constants.baseColor[2] = b;
        constants.baseColor[3] = a;
        constants.outlineParams[3] = roughness;
        constants.materialParams[0] = specular;
        // ⚠️ LANE z, AND IT WAS SPARE. Metalness rides the float4 that already
        // carries the specular level rather than growing the cbuffer, so the
        // static_assert in DiligentShaders.hpp and every PSO built against that
        // layout stay put.
        constants.materialParams[2] = metallic;
        UploadConstants (context, impl_->constants, constants);
    };

    auto drawRange = [&] (const Entry& e, const MaterialRange& r, bool blended) {
        // ⚠️ HDR PATH SELECTS THE HDR PSO SET. Same shaders, same SRB layout,
        // different RTV format (RGBA16_FLOAT). The pixel shader branches on
        // g_frameControl.x to skip Grade() when writing into the HDR target.
        Diligent::IPipelineState* pso;
        Diligent::IShaderResourceBinding* srb;
        if (useHdr) {
            pso = blended ? impl_->hdrBlendPso[index] : impl_->hdrOpaquePso[index];
            srb = blended ? impl_->hdrBlendSrb[index] : impl_->hdrOpaqueSrb[index];
        } else {
            pso = blended ? impl_->blendPso[index] : impl_->opaquePso[index];
            srb = blended ? impl_->blendSrb[index] : impl_->opaqueSrb[index];
        }
        context->SetPipelineState (pso);
        context->CommitShaderResources (srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        BindMesh (context, e);
        DrawEntryRange (context, e, r);
        ++impl_->drawCalls;
    };

    // ---- RE51.C3: the occlusion prepared before this call -------------------
    //
    // ⚠️ BOUND ON EVERY MESH SRB, NOT ONLY THE ONE ABOUT TO DRAW. The cull mode
    // and the blend state each pick a different SRB, and the transparent pass
    // switches between them mid-frame; binding only `index` leaves the others
    // pointing at whatever the last resize left there, which on a fresh device
    // is nothing at all and reads as a validation error rather than as dark
    // glass.
    //
    // ⚠️ AND THE INTENSITY IS ZEROED WHEN THERE IS NO TEXTURE. The shader's gate
    // is that constant, not a null check -- HLSL cannot ask whether a texture is
    // bound, and sampling an unbound one returns zero, which would darken the
    // entire model to black rather than leaving it alone.
    // ⚠️ THE VARIABLE IS ASSIGNED ON EVERY FRAME, INCLUDING FRAMES WITH NO
    // OCCLUSION. It is DYNAMIC, so leaving it unset and committing the SRB is a
    // validation failure inside Archicad's process -- and there are plenty of
    // such frames: before geometry arrives, with the effect switched off, or
    // when the AO pass declined. The fallback is a 1x1 WHITE texel meaning
    // "nothing is occluded"; see DiligentSceneImpl::aoFallback for why white
    // and not simply "leave it".
    Diligent::ITextureView* occlusionView = impl_->aoView;
    if (occlusionView == nullptr && impl_->aoFallback != nullptr)
        occlusionView = impl_->aoFallback->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (occlusionView != nullptr) {
        for (int cullIdx = 0; cullIdx < kCullModeCount; ++cullIdx) {
            // ⚠️ BOTH LDR AND HDR SRBs. The HDR path uses its own PSO set, so its
            // SRBs carry the same DYNAMIC g_ambientOcclusion variable -- and the
            // same validation failure if it is left unset.
            for (Diligent::IShaderResourceBinding* srb :
                 { impl_->opaqueSrb[cullIdx].RawPtr (), impl_->blendSrb[cullIdx].RawPtr (),
                   impl_->hdrOpaqueSrb[cullIdx].RawPtr (), impl_->hdrBlendSrb[cullIdx].RawPtr () }) {
                if (srb == nullptr)
                    continue;
                if (Diligent::IShaderResourceVariable* var =
                        srb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_ambientOcclusion"))
                    var->Set (occlusionView);
            }
        }
    }
    // The constant is still the gate: the fallback is bound so the SRB is
    // valid, not so the shader reads it.
    constants.gradeParams[3] = impl_->aoView != nullptr ? impl_->aoIntensity : 0.0f;

    const bool drawSurfaces = impl_->renderMode != SceneRenderMode::Wireframe;
    const bool drawWireframe = impl_->renderMode != SceneRenderMode::Shaded && impl_->wirePso != nullptr;

    // Two passes over the same ranges: opaque first, then everything that
    // blends. Not a sort -- a partition, which needs no comparator and no
    // per-frame allocation, and gets the one ordering that actually matters
    // right for free.
    for (int pass = 0; drawSurfaces && pass < 2; ++pass) {
        const bool transparentPass = pass == 1;

        if (!transparentPass) {
            // The helper meshes are always opaque and carry their own vertex
            // colours, so the material colour stays white.
            uploadConstants (1.0f, 1.0f, 1.0f, 1.0f);
            for (const Entry& e : impl_->staticMeshes) {
                if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                    continue;
                for (const MaterialRange& r : e.ranges)
                    drawRange (e, r, false);
            }
        }

        for (const Entry& e : impl_->elements) {
            if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            // ⚠️ ONE DRAW PER MATERIAL RANGE. Drawing the element in one call
            // paints every surface in the first material's colour -- the exact
            // bug MeshGroups exists to prevent, one layer up.
            for (const MaterialRange& r : e.ranges) {
                if (r.indexCount == 0)
                    continue;
                const SurfaceMaterial& mat = impl_->materials.Lookup (r.material);
                // ⚠️ A SELECTED ELEMENT BLENDS EVEN WHEN ITS MATERIAL IS OPAQUE,
                // and that is what puts it in the SECOND pass. Drawing it
                // translucent in the opaque pass would blend it against whatever
                // happened to be behind it in draw order rather than against the
                // finished scene, so a selected wall would show the elements
                // drawn before it and hide the ones drawn after -- an
                // arbitrary-looking half of the room, changing with the
                // extraction order.
                const bool selectedBlend = e.selected && kSelectionAlpha < kOpaqueAlpha;
                const bool blended = mat.alpha < kOpaqueAlpha || selectedBlend;
                if (blended != transparentPass)
                    continue;
                // ⚠️ THE HIGHLIGHT IS A TINT, NOT A REPLACEMENT. Painting a
                // selected element flat cyan hides which surfaces it has and
                // whether its shading is right -- so a selection would make an
                // element harder to inspect at exactly the moment the user asked
                // to look at it. Mixing keeps the shape and the material readable
                // and stays unmistakable against a building. The silhouette pass
                // below adds the boundary the tint cannot show, and the alpha
                // lets whatever the element stands in front of come through.
                //
                // ⚠️ AND IT IS DONE HERE, ON THE MATERIAL COLOUR, NOT IN THE
                // SHADER. A shader-side flag would need a field in
                // DiligentSceneConstants, whose layout is an ABI with three HLSL
                // stages; the tint needs no new state at all because the base
                // colour is already uploaded per range.
                // ⚠️ NOT NAMED r/g/b -- `r` is the MaterialRange this loop is
                // over, and shadowing it hands the range's colour to the draw.
                float tintR = mat.r;
                float tintG = mat.g;
                float tintB = mat.b;
                float alpha = mat.alpha;
                if (e.selected) {
                    tintR = mat.r * kSelectionTintMix + kSelectionTintR;
                    tintG = mat.g * kSelectionTintMix + kSelectionTintG;
                    tintB = mat.b * kSelectionTintMix + kSelectionTintB;
                    // Glass that is already more transparent than the selection
                    // must not become LESS transparent by being selected.
                    // ⚠️ PARENTHESISED: <windows.h> comes in through DiligentSceneImpl.hpp
                    // and defines a `min` MACRO, which turns `std::min (` into a
                    // syntax error on the `::`.
                    alpha = (std::min) (alpha, kSelectionAlpha);
                }
                // ⚠️ THE WHOLE PBR DESCRIPTION, FROM THE CLASSIFIER, NOT FROM
                // THE RAW CHANNELS. `PresetFor` runs SurfaceClassifier over the
                // measured numbers and returns the roughness, the dielectric
                // specular level and -- the part Archicad has no channel for at
                // all -- the metalness. Reading `mat.shininess` directly here
                // again would quietly restore the dielectric-only renderer.
                const SurfacePreset preset = PresetFor (mat);
                // ⚠️ PER RANGE, NOT PER FRAME. Hoisting the upload out of this
                // loop paints every range in the last material's colour -- and
                // now its finish too, in both channels.
                uploadConstants (tintR, tintG, tintB, alpha, preset.roughness, preset.reflectance, preset.metallic);
                drawRange (e, r, blended);
            }
        }
    }

    // ---- the HDR resolve pass with SSR composition (RE51.C7) ----------------
    //
    // ⚠️ THIS PASS BINDS THE SWAP CHAIN TARGET AND LEAVES IT BOUND. It is the
    // second of the two contracts from the 2026-08-21 binding fault: the HDR
    // scene pass binds its own target (the HDR RTV) and this pass restores to
    // the caller's target. The wireframe and outline draws that follow inherit
    // the swap chain binding exactly as they did before the HDR path existed.
    //
    // ⚠️ THE FRAME CONTROL FLAG IS SET BACK TO 0 BEFORE THE UPLOAD, so the
    // resolve PS's own Grade() call reads the same exposure and white balance
    // the LDR path would have applied. The resolve PS does NOT branch on
    // g_frameControl.x -- it always tone-maps.
    //
    // ⚠️ g_gradeParams.w IS REUSED AS THE SSR INTENSITY IN THE RESOLVE PASS.
    // In the mesh shader it is the AO intensity; in the resolve pass AO is
    // already baked into the HDR colour, so the lane is free. When SSR is off,
    // it is set to 0 and the shader's roughness check skips the SSR branch.
    if (useHdr) {
        // ---- RE51.C7: prepare SSR before the resolve -----------------------
        //
        // ⚠️ SSR READS THE HDR SCENE COLOUR, so it must run AFTER the mesh draw
        // and BEFORE the resolve. The opaque G-buffer was rendered by the AO
        // prepass earlier in the frame; SSR completes it with glass receivers.
        // The HDR colour SRV is available because EnsureHdrTarget succeeded.
        if (impl_->ssrEnabled && impl_->ssrIntensity > 0.0f) {
            PrepareScreenSpaceReflection (context, view, proj, viewProj, motionViewProj, eye, jitter,
                                          nearClip, farClip, focusDistance, frameIndex);
        }

        Diligent::ITextureView* resolvedHdr = ExecuteTemporalAntiAliasing (
            context, view, proj, viewProj, motionViewProj, eye, jitter,
            nearClip, farClip, focusDistance, frameIndex);
        if (resolvedHdr == nullptr)
            resolvedHdr = impl_->hdrColorSRV;

        constants.frameControl[0] = 0.0f;
        // ⚠️ gradeParams.w: AO intensity was already applied in the mesh shader,
        // so the lane is repurposed for SSR intensity in the resolve pass.
        constants.gradeParams[3] = (impl_->ssrView != nullptr) ? impl_->ssrIntensity : 0.0f;
        UploadConstants (context, impl_->constants, constants);
        context->SetRenderTargets (1, &colorTarget, depthTarget,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetViewports (1, nullptr, 0, 0);

        // TAA's alpha is history weight, not coverage. Resolve RGB from the TAA
        // result when available, but always read coverage from this frame's
        // original HDR target so the composition overlay stays transparent.
        if (Diligent::IShaderResourceVariable* hdrVar =
                impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_hdrColor"))
            hdrVar->Set (resolvedHdr);
        if (Diligent::IShaderResourceVariable* coverageVar =
                impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_hdrCoverage"))
            coverageVar->Set (impl_->hdrColorSRV);
        Diligent::ITextureView* ssrColorView = impl_->ssrView;
        if (ssrColorView == nullptr && impl_->ssrFallback != nullptr)
            ssrColorView = impl_->ssrFallback->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        Diligent::ITextureView* roughnessView = nullptr;
        if (impl_->ssrView != nullptr && impl_->gBuffer != nullptr) {
            Diligent::ITexture* roughnessTex = impl_->gBuffer->GetBuffer (kGBufferRoughness);
            if (roughnessTex != nullptr)
                roughnessView = roughnessTex->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        }
        if (roughnessView == nullptr && impl_->ssrRoughnessFallback != nullptr)
            roughnessView = impl_->ssrRoughnessFallback->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        Diligent::ITextureView* depthView = nullptr;
        Diligent::ITextureView* normalView = nullptr;
        Diligent::ITextureView* albedoView = nullptr;
        Diligent::ITextureView* materialView = nullptr;
        if (impl_->ssrView != nullptr && impl_->gBuffer != nullptr) {
            Diligent::ITexture* depthTex = impl_->gBuffer->GetBuffer (kGBufferDepth);
            if (depthTex != nullptr)
                depthView = depthTex->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
            Diligent::ITexture* normalTex = impl_->gBuffer->GetBuffer (kGBufferNormal);
            Diligent::ITexture* albedoTex = impl_->gBuffer->GetBuffer (kGBufferAlbedo);
            Diligent::ITexture* materialTex = impl_->gBuffer->GetBuffer (kGBufferMaterialData);
            if (normalTex != nullptr)
                normalView = normalTex->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
            if (albedoTex != nullptr)
                albedoView = albedoTex->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
            if (materialTex != nullptr)
                materialView = materialTex->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        }
        Diligent::ITextureView* dataFallback =
            impl_->ssrFallback != nullptr
                ? impl_->ssrFallback->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE)
                : nullptr;
        if (normalView == nullptr)
            normalView = dataFallback;
        if (albedoView == nullptr)
            albedoView = dataFallback;
        if (materialView == nullptr)
            materialView = dataFallback;
        if (ssrColorView != nullptr) {
            if (Diligent::IShaderResourceVariable* var =
                    impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_ssrColor"))
                var->Set (ssrColorView);
        }
        if (roughnessView != nullptr) {
            if (Diligent::IShaderResourceVariable* var =
                    impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferRoughness"))
                var->Set (roughnessView);
        }
        if (depthView != nullptr) {
            if (Diligent::IShaderResourceVariable* var =
                    impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth"))
                var->Set (depthView);
        }
        const auto bindResolveTexture = [&] (const char* name, Diligent::ITextureView* view) {
            if (view == nullptr)
                return;
            if (Diligent::IShaderResourceVariable* var =
                    impl_->resolveSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, name))
                var->Set (view);
        };
        bindResolveTexture ("g_gbufferNormal", normalView);
        bindResolveTexture ("g_gbufferAlbedo", albedoView);
        bindResolveTexture ("g_gbufferMaterialData", materialView);

        context->SetPipelineState (impl_->resolvePso);
        context->CommitShaderResources (impl_->resolveSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Diligent::DrawAttribs resolve;
        resolve.NumVertices = 3;
        resolve.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw (resolve);
        ++impl_->drawCalls;

        // Clear the SSR view so the next frame does not reuse a freed texture.
        ClearScreenSpaceReflection ();
        impl_->taaView = nullptr;
    }

    // ---- the wireframe overlay ---------------------------------------------
    // ⚠️ ELEMENTS ONLY, AND THE WHOLE ELEMENT IN ONE DRAW. Material ranges do not
    // matter to a line colour, and drawing per range would multiply the pass's
    // cost by the material count for an identical picture. The gnomon and the
    // debug cube are excluded for the same reason they are excluded from the id
    // pass: they are not the model, and in the overlay's wireframe they would be
    // the only things NOT matching Archicad's window.
    if (drawWireframe) {
        context->SetPipelineState (impl_->wirePso);
        context->CommitShaderResources (impl_->wireSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        uploadConstants (kWireframeColor[0], kWireframeColor[1], kWireframeColor[2], kWireframeColor[3]);
        for (const Entry& e : impl_->elements) {
            if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            BindMesh (context, e);
            const MaterialRange whole { -1, 0, e.indexCount };
            DrawEntryRange (context, e, whole);
            ++impl_->drawCalls;
        }
    }

    // ---- the selection silhouette (PLAT-RE41) -------------------------------
    // ⚠️ LAST, AFTER THE TRANSPARENT PASS. The outline is what says where the
    // element ENDS, so anything drawn over it takes that away -- and a selected
    // element is very often behind glass, which is drawn in the pass above.
    //
    // ⚠️ SKIPPED ENTIRELY UNDER CullMode::None. The hull is exactly the faces the
    // visible pass throws away; with nothing thrown away there is no hull, and
    // the pass would paint a solid expanded copy of the model over the model.
    // ---- the hover silhouette (PLAT-RE136) ----------------------------------
    // Same pass, same hull, a different colour and a thinner offset: what a click
    // WOULD take, drawn before the user commits to it. It shares the selection's
    // pipeline state because it is the same inverted hull -- a second PSO would be
    // two objects that must agree about depth, cull and blend forever.
    const uint32_t hoverId = impl_->hoverId;
    const int outlineIndex = CullIndex (cull);
    const bool anyOutline = !impl_->selectionGuids.empty () || hoverId != kNoPickId;
    if (cull != CullMode::None && impl_->outlinePso[outlineIndex] != nullptr && anyOutline) {
        context->SetPipelineState (impl_->outlinePso[outlineIndex]);
        context->CommitShaderResources (impl_->outlineSrb[outlineIndex],
                                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        auto drawHull = [&] (const Entry& e) {
            BindMesh (context, e);
            const MaterialRange whole { -1, 0, e.indexCount };
            DrawEntryRange (context, e, whole);
            ++impl_->drawCalls;
        };

        uploadConstants (kSelectionOutlineColor[0], kSelectionOutlineColor[1], kSelectionOutlineColor[2],
                         kSelectionOutlineColor[3]);
        for (const Entry& e : impl_->elements) {
            if (!e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                continue;
            drawHull (e);
        }

        // ⚠️ SKIPPED WHEN THE HOVERED ELEMENT IS ALREADY SELECTED. Two hulls on
        // one element paint the amber over the cyan, so hovering a selected
        // element would make it look deselected -- the opposite of what both
        // marks are for. Selection is the stronger statement and wins.
        if (hoverId != kNoPickId) {
            for (const Entry& e : impl_->elements) {
                if (e.pickId != hoverId || e.selected || e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
                    continue;
                // ⚠️ THE OFFSET IS RE-UPLOADED, NOT JUST THE COLOUR. The hull's
                // thickness lives in `outlineParams`, which was filled for the
                // SELECTION's 3 px far above; `uploadConstants` only touches
                // `baseColor`, so without this the hover would silently inherit
                // the selection's weight and the constant below would do nothing.
                constants.outlineParams[0] =
                    impl_->viewportWidth > 0 ? kHoverOutlinePixels * 2.0f / float (impl_->viewportWidth) : 0.0f;
                constants.outlineParams[1] =
                    impl_->viewportHeight > 0 ? kHoverOutlinePixels * 2.0f / float (impl_->viewportHeight) : 0.0f;
                uploadConstants (kHoverOutlineColor[0], kHoverOutlineColor[1], kHoverOutlineColor[2],
                                 kHoverOutlineColor[3]);
                drawHull (e);
                break; // one element carries an id; nothing after it can match
            }
        }
    }
}

} // namespace archviz
} // namespace geomsrv
