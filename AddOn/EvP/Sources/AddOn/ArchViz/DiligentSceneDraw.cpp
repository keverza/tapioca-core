// The scene's PASSES: the sun's depth pass, the main opaque/transparent draw and
// the corner overlay. Everything here binds and submits; nothing here creates a
// GPU resource. The pipeline states live in DiligentScene.cpp and the element map
// in DiligentSceneGeometry.cpp; DiligentSceneImpl.hpp says why the class is
// spread over three files.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/SurfaceClassifier.hpp"

#include <algorithm> // std::min, for the selection alpha against a glass material
#include <cstring>
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
void UploadConstants (Diligent::IDeviceContext* context, Diligent::IBuffer* buffer,
                      const DiligentSceneConstants& constants)
{
    Diligent::PVoid mapped = nullptr;
    context->MapBuffer (buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, mapped);
    if (mapped != nullptr) {
        *static_cast<DiligentSceneConstants*> (mapped) = constants;
        context->UnmapBuffer (buffer, Diligent::MAP_WRITE);
    }
}

void BindMesh (Diligent::IDeviceContext* context, const Entry& e)
{
    Diligent::IBuffer* vertexBuffers[1] = { e.vertexBuffer };
    const Diligent::Uint64 offsets[1] = { 0 };
    context->SetVertexBuffers (0, 1, vertexBuffers, offsets, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer (e.indexBuffer, 0, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void DrawEntryRange (Diligent::IDeviceContext* context, const Entry& e, const MaterialRange& r)
{
    Diligent::DrawIndexedAttribs draw;
    draw.NumIndices = r.indexCount;
    draw.FirstIndexLocation = r.firstIndex;
    draw.IndexType = e.indices32 ? Diligent::VT_UINT32 : Diligent::VT_UINT16;
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->DrawIndexed (draw);
}

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

void DiligentScene::DrawGBufferDebug (Diligent::IDeviceContext* context, Diligent::ITextureView* colorTarget,
                                      const float view[16], const float proj[16], const float viewProj[16],
                                      const float eye[3], float nearClip, float farClip, float focusDistance,
                                      bool perspective, uint32_t frameIndex, CullMode cull, int debugView)
{
    if (context == nullptr || colorTarget == nullptr || !impl_->ready || impl_->gBuffer == nullptr ||
        impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return;

    if (impl_->gBufferWidth != impl_->viewportWidth || impl_->gBufferHeight != impl_->viewportHeight) {
        impl_->gBuffer->Resize (impl_->device, impl_->viewportWidth, impl_->viewportHeight);
        if (impl_->gBuffer->GetBuffer (kGBufferNormal) == nullptr ||
            impl_->gBuffer->GetBuffer (kGBufferDepth) == nullptr ||
            impl_->gBuffer->GetBuffer (kGBufferMotion) == nullptr ||
            impl_->gBuffer->GetBuffer (kGBufferAlbedo) == nullptr ||
            impl_->gBuffer->GetBuffer (kGBufferRoughness) == nullptr ||
            impl_->gBuffer->GetBuffer (kGBufferMaterialData) == nullptr)
            return;

        impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferNormal")
            ->Set (impl_->gBuffer->GetBuffer (kGBufferNormal)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth")
            ->Set (impl_->gBuffer->GetBuffer (kGBufferDepth)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferAlbedo")
            ->Set (impl_->gBuffer->GetBuffer (kGBufferAlbedo)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferRoughness")
            ->Set (
                impl_->gBuffer->GetBuffer (kGBufferRoughness)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferMaterialData")
            ->Set (impl_->gBuffer->GetBuffer (kGBufferMaterialData)
                       ->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_depthRange")
            ->Set (impl_->depthRange.BufferView ());
        impl_->gBufferWidth = impl_->viewportWidth;
        impl_->gBufferHeight = impl_->viewportHeight;
    }

    context->SetViewports (1, nullptr, 0, 0);
    impl_->gBuffer->Bind (context, kGBufferGeometryMask, nullptr, kGBufferGeometryMask);
    // Binding a different framebuffer resets the viewport on D3D11. Set it
    // again after the G-buffer bind so every MRT receives the full scene.
    context->SetViewports (1, nullptr, 0, 0);

    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);
    constants.gradeParams[2] = impl_->roughnessBias;
    UploadConstants (context, impl_->constants, constants);

    const int index = CullIndex (cull);
    context->SetPipelineState (impl_->gBufferPso[index]);
    context->CommitShaderResources (impl_->gBufferSrb[index], Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    impl_->drawCalls = 0;

    const SurfaceMaterial defaultMaterial;
    auto uploadMaterial = [&] (const SurfaceMaterial& material, const SurfacePreset& preset) {
        constants.baseColor[0] = material.r;
        constants.baseColor[1] = material.g;
        constants.baseColor[2] = material.b;
        constants.baseColor[3] = material.alpha;
        constants.outlineParams[3] = preset.roughness;
        constants.materialParams[0] = preset.reflectance;
        constants.materialParams[2] = preset.metallic;
        UploadConstants (context, impl_->constants, constants);
    };

    auto drawOpaqueRanges = [&] (const Entry& e, bool consultMaterials) {
        if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
            return;
        BindMesh (context, e);
        for (const MaterialRange& range : e.ranges) {
            if (range.indexCount == 0)
                continue;
            const SurfaceMaterial& material =
                consultMaterials ? impl_->materials.Lookup (range.material) : defaultMaterial;
            if (consultMaterials && material.alpha < kOpaqueAlpha)
                continue;
            const SurfacePreset preset = consultMaterials ? PresetFor (material) : SurfacePreset {};
            uploadMaterial (material, preset);
            DrawEntryRange (context, e, range);
            ++impl_->drawCalls;
        }
    };

    for (const Entry& e : impl_->elements)
        drawOpaqueRanges (e, true);
    for (const Entry& e : impl_->staticMeshes)
        drawOpaqueRanges (e, false);

    Diligent::ITexture* motionTexture = impl_->gBuffer->GetBuffer (kGBufferMotion);
    Diligent::ITextureView* motionRtv = motionTexture->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    const float zeroMotion[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTarget (motionRtv, zeroMotion, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Inputs cannot remain outputs while PostFXContext consumes them.
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);

    Diligent::ITextureView* normalSrv =
        impl_->gBuffer->GetBuffer (kGBufferNormal)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    Diligent::ITextureView* depthSrv =
        impl_->gBuffer->GetBuffer (kGBufferDepth)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    // How deep this frame really is, for the depth view's ramp. ⚠️ ONLY FOR THAT
    // VIEW: the other two never read the buffer, and a dispatch over every pixel
    // to feed a shader branch that is not taken is a cost with no picture behind
    // it. The buffer stays bound regardless -- it is seeded empty, which the
    // shader reads as "fall back to the frustum".
    if (debugView == int (DiligentDebugView::GBufferDepth))
        impl_->depthRange.Execute (context, depthSrv, impl_->viewportWidth, impl_->viewportHeight);

    Diligent::ITextureView* ambientOcclusionSrv = nullptr;
    if (debugView == int (DiligentDebugView::AmbientOcclusion)) {
        impl_->ambientOcclusion.Init (impl_->device);
        Diligent::ITextureView* motionSrv = motionTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        ambientOcclusionSrv = impl_->ambientOcclusion.Execute (
            impl_->device, context, normalSrv, depthSrv, motionSrv, impl_->viewportWidth, impl_->viewportHeight,
            frameIndex, view, proj, viewProj, eye, nearClip, farClip, focusDistance);
        if (ambientOcclusionSrv == nullptr)
            return;
        impl_->ambientOcclusionDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_ambientOcclusion")
            ->Set (ambientOcclusionSrv);
        impl_->ambientOcclusionDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth")
            ->Set (depthSrv);
    }

    Diligent::ITextureView* target = colorTarget;
    context->SetRenderTargets (1, &target, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetViewports (1, nullptr, 0, 0);

    constants.debugView = float (debugView);
    constants.shadowParams[0] = nearClip;
    constants.shadowParams[1] = farClip;
    // z carried for the AO pass's own use only -- the depth view's ramp is
    // logarithmic over [near, far] and deliberately does NOT read it. See
    // kArchVizGBufferDebugPS for why a focus-derived window went black.
    constants.shadowParams[2] = focusDistance;
    constants.shadowParams[3] = perspective ? 1.0f : 0.0f;
    UploadConstants (context, impl_->constants, constants);
    Diligent::IPipelineState* debugPso = debugView == int (DiligentDebugView::AmbientOcclusion)
                                             ? impl_->ambientOcclusionDebugPso.RawPtr ()
                                             : impl_->gBufferDebugPso.RawPtr ();
    Diligent::IShaderResourceBinding* debugSrb = debugView == int (DiligentDebugView::AmbientOcclusion)
                                                     ? impl_->ambientOcclusionDebugSrb.RawPtr ()
                                                     : impl_->gBufferDebugSrb.RawPtr ();
    context->SetPipelineState (debugPso);
    context->CommitShaderResources (debugSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DrawAttribs draw;
    draw.NumVertices = 3;
    draw.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
    context->Draw (draw);
    ++impl_->drawCalls;
}

void DiligentScene::Draw (Diligent::IDeviceContext* context, const float viewProj[16], const float eye[3],
                          CullMode cull, int debugView)
{
    if (context == nullptr || !impl_->ready)
        return;

    // ⚠️ (1, nullptr, 0, 0), NOT (0, ...). Zero means "bind no viewports at
    // all" and every triangle is clipped away while the clear still happens --
    // which reads exactly like a renderer that draws nothing. It cost
    // TransparencyProbe a full run; see PatternRenderer.cpp -> Render.
    context->SetViewports (1, nullptr, 0, 0);

    const int index = CullIndex (cull);
    impl_->drawCalls = 0;

    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);
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

    // ---- the sky behind the model, before anything else ----------------------
    //
    // ⚠️ FINAL VIEW ONLY. Every other debug view answers a question about the
    // model, and a photographic sky behind a normals or roughness read makes it
    // harder, not easier, to see what the view exists to show.
    if (envActive && impl_->environmentBackground && debugView == 0 && impl_->envBackgroundPso != nullptr) {
        UploadConstants (context, impl_->constants, constants);
        context->SetPipelineState (impl_->envBackgroundPso);
        context->CommitShaderResources (impl_->envBackgroundSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
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
    constants.materialParams[1] = impl_->sunWithSkyWeight;
    constants.gradeParams[0] = impl_->exposure;
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
        Diligent::IPipelineState* pso = blended ? impl_->blendPso[index] : impl_->opaquePso[index];
        Diligent::IShaderResourceBinding* srb = blended ? impl_->blendSrb[index] : impl_->opaqueSrb[index];
        context->SetPipelineState (pso);
        context->CommitShaderResources (srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        BindMesh (context, e);
        DrawEntryRange (context, e, r);
        ++impl_->drawCalls;
    };

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
