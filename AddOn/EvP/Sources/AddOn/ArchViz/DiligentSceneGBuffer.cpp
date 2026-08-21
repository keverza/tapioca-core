// ArchViz/DiligentSceneGBuffer — the DEFERRED half of the scene: the G-buffer
// prepass, the ambient-occlusion pass built on it, and the debug views that
// display its channels.
//
// It is a separate translation unit because `DiligentSceneDraw.cpp` crossed the
// ~1,000-line cap, and this is the seam that was already there rather than a
// place to cut. What is left in that file is the FORWARD frame: the shadow map,
// the sky, the model, the wireframe, the outline and the grading that finishes
// them. Everything here writes to or reads from the G-buffer instead, and none
// of it draws the model as the user sees it.
//
// ⚠️ EVERY PASS IN THIS FILE REBINDS THE RENDER TARGETS, AND THAT IS THE ONE
// THING TO KNOW ABOUT IT. `RenderGBufferGeometry` binds the G-buffer's MRTs;
// `PrepareAmbientOcclusion` ends with SetRenderTargets(0, ...) so those targets
// can be sampled; `DrawGBufferDebug` binds the caller's colour view at the end
// of its own resolve. DiligentScene::Draw therefore binds its targets itself
// rather than inheriting them -- it did inherit them once, and the result was a
// grey viewport with no geometry, no error and no message (2026-08-21).
//
// ⚠️ RENDER THREAD ONLY, like everything else in the Diligent viewport.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"   // ArchVizLog -- a failed bind must say so, not crash
#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/SurfaceClassifier.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace geomsrv {
namespace archviz {

bool DiligentScene::EnsureGBufferTargets ()
{
    if (impl_->gBufferWidth != impl_->viewportWidth || impl_->gBufferHeight != impl_->viewportHeight) {
        impl_->gBuffer->Resize (impl_->device, impl_->viewportWidth, impl_->viewportHeight);
        // ⚠️ EVERY BIND BELOW IS CHECKED, AND THIS BLOCK IS WHY ARCHICAD CRASHED
        // ON 2026-08-21. It used to be a column of
        //
        //     srb->GetVariableByName (stage, "name")->Set (view);
        //
        // with no check on either half. `GetVariableByName` returns NULL for a
        // variable that is not on the SRB, and a texture is not on the SRB
        // whenever it is STATIC rather than DYNAMIC -- which is what a shader
        // texture MISSING FROM debugVariables in DiligentScene::Init silently
        // becomes. So adding `g_gbufferMotion` to the shader and forgetting the
        // one-line table entry produced a null dereference inside Archicad's
        // process, on the first frame that had geometry to draw, several seconds
        // after the only clue: a single "No resource is assigned to static
        // shader variable" line in archviz.log.
        //
        // ⚠️ IT REPORTS RATHER THAN SKIPPING QUIETLY. A bind that silently did
        // nothing would leave the debug view black with no way to tell that from
        // an empty G-buffer, and the whole point of these views is to be
        // trustworthy about what they show.
        bool bindsOk = true;
        const auto bindDebug = [&] (const char* name, Diligent::Uint32 buffer) {
            Diligent::ITexture* texture = impl_->gBuffer->GetBuffer (buffer);
            Diligent::IShaderResourceVariable* variable =
                impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, name);
            if (texture == nullptr || variable == nullptr) {
                ArchVizLog (std::string ("ArchViz G-buffer debug: cannot bind `") + name + "` -- " +
                            (texture == nullptr
                                 ? "the target was not allocated"
                                 : "the shader variable is not on the SRB, which means it is STATIC rather "
                                   "than DYNAMIC: add it to `debugVariables` in DiligentScene::Init"));
                bindsOk = false;
                return;
            }
            variable->Set (texture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
        };

        bindDebug ("g_gbufferNormal", kGBufferNormal);
        bindDebug ("g_gbufferDepth", kGBufferDepth);
        bindDebug ("g_gbufferAlbedo", kGBufferAlbedo);
        bindDebug ("g_gbufferRoughness", kGBufferRoughness);
        bindDebug ("g_gbufferMaterialData", kGBufferMaterialData);
        bindDebug ("g_gbufferMotion", kGBufferMotion);

        if (Diligent::IShaderResourceVariable* depthRangeVar =
                impl_->gBufferDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_depthRange")) {
            depthRangeVar->Set (impl_->depthRange.BufferView ());
        } else {
            ArchVizLog ("ArchViz G-buffer debug: cannot bind `g_depthRange` -- it is not on the SRB");
            bindsOk = false;
        }

        // ⚠️ THE SIZE IS RECORDED ONLY ON SUCCESS. Recording it after a failed
        // bind would make the next frame think the targets are already current
        // and skip the retry, turning one bad frame into a permanently dead
        // G-buffer.
        if (!bindsOk)
            return false;
        impl_->gBufferWidth = impl_->viewportWidth;
        impl_->gBufferHeight = impl_->viewportHeight;
        // ⚠️ A RESIZE IS A DISCONTINUITY, NOT A MOTION. Every pixel is somewhere
        // else and the motion vectors describe none of it, so any reprojected
        // history is drawn from wherever the old, differently-shaped image
        // happened to have content. The AO pass detects this for itself as well;
        // saying it here too costs nothing and means the scene does not depend
        // on that being true.
        //
        // ⚠️ WHAT IS NOT COVERED: a camera TELEPORT. Adopting Archicad's camera
        // moves the eye without any intervening frames, and the vectors for that
        // frame are as wrong as a resize's. It is one frame of smeared occlusion
        // on an event the user just triggered, which is why it is recorded here
        // rather than guessed at with a heuristic on matrix distance.
        impl_->ambientOcclusion.ResetHistory ();
    }

    return true;
}

// The opaque geometry into the G-buffer's MRTs.
//
// ⚠️ ONE COPY, SHARED BY THE DEBUG VIEWS AND BY THE OCCLUSION PREPASS. Two
// copies would be two descriptions of the same scene, free to disagree about
// which ranges are opaque or which preset a material gets -- and the symptom
// would be occlusion that does not line up with the shading it darkens, which
// reads as a projection bug rather than as a duplicated loop.
//
// ⚠️ IT LEAVES THE G-BUFFER BOUND AS A RENDER TARGET. Every caller has to unbind
// before sampling those same textures as SRVs; D3D11 resolves the conflict by
// silently dropping the SRV, which produces a black result and no message.
void DiligentScene::RenderGBufferGeometry (Diligent::IDeviceContext* context, const float viewProj[16], CullMode cull)
{
    context->SetViewports (1, nullptr, 0, 0);
    // ⚠️ THE FOURTH ARGUMENT CLEARS AND THE FIFTH REMAPS. The motion target is
    // cleared with the rest -- a pixel no triangle covers has no motion, and
    // leaving last frame's vectors there would reproject the sky from wherever
    // the building used to be. See kGBufferRTIndices for the remap.
    impl_->gBuffer->Bind (context, kGBufferGeometryMask, nullptr, kGBufferGeometryMask, kGBufferRTIndices);
    // Binding a different framebuffer resets the viewport on D3D11. Set it
    // again after the G-buffer bind so every MRT receives the full scene.
    context->SetViewports (1, nullptr, 0, 0);

    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);
    // ⚠️ RE51.C2. On the first frame this IS the current matrix, so the motion
    // vectors come out exactly zero rather than enormous -- see
    // DiligentSceneConstants::prevViewProj.
    std::memcpy (constants.prevViewProj, impl_->havePrevViewProj ? impl_->prevViewProj : viewProj,
                 sizeof (float) * 16);
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

}

void DiligentScene::SetAmbientOcclusion (bool enabled, float intensity, float radiusMetres)
{
    if (impl_ == nullptr)
        return;
    impl_->aoEnabled = enabled;
    impl_->aoIntensity = intensity < 0.0f ? 0.0f : (intensity > 2.0f ? 2.0f : intensity);
    impl_->aoRadius = radiusMetres;
}

// RE51.C2. Called once per frame, AFTER every pass that used this frame's
// camera. ⚠️ NOT AT THE TOP OF THE NEXT FRAME, which is the same instant and is
// where it would be easy to put it: the AO prepass and the shading pass both run
// inside one frame and both must see the SAME previous matrix, so the handover
// has to happen where nothing else is left to read it.
void DiligentScene::AdvanceFrame (const float viewProj[16])
{
    if (impl_ == nullptr || viewProj == nullptr)
        return;
    std::memcpy (impl_->prevViewProj, viewProj, sizeof (float) * 16);
    impl_->havePrevViewProj = true;
}

void DiligentScene::ClearAmbientOcclusion ()
{
    if (impl_ != nullptr)
        impl_->aoView = nullptr;
}

// RE51.C3. See the header for why the forward path now pays for a second
// geometry pass, and why this has to run BEFORE Draw rather than after.
// The occlusion radius in world metres. See the header for why there is exactly
// one of these.
//
// ⚠️ DERIVED FROM THE MODEL, BECAUSE A FIXED RADIUS CANNOT SERVE BOTH A KITCHEN
// AND A MASTERPLAN. DiligentFX's default is 1.0 world unit; on the thirty-metre
// massing this renderer was first judged against that is a contact band a few
// pixels wide, which is exactly what the live report "AO darkens whole scene,
// but soft contact shadow is not visible" describes.
//
// ⚠️ 5% OF THE BOUNDING DIAGONAL IS A HEURISTIC AND IS LABELLED AS ONE. There is
// no measurement behind it -- only the observation that a contact gradient has
// to be a noticeable fraction of the thing it grounds before the eye reads it as
// contact rather than as dirt. The HUD slider is the instrument that settles the
// real value; this is only what it starts from, and the value in force is
// reported in the stats every frame so a live run can say what it was.
//
// ⚠️ CLAMPED AT BOTH ENDS. A single-element scene can be centimetres across and
// a georeferenced site can be kilometres; neither should drive the radius to a
// value where GTAO samples one texel or the whole screen.
float DiligentScene::AmbientOcclusionRadius () const
{
    if (impl_ == nullptr)
        return 1.0f;
    if (impl_->aoRadius > 0.0f) {
        impl_->aoRadiusInUse = impl_->aoRadius;
        return impl_->aoRadius;
    }

    float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
    float radius = 1.0f;
    if (SceneBounds (boundsMin, boundsMax)) {
        const float dx = boundsMax[0] - boundsMin[0];
        const float dy = boundsMax[1] - boundsMin[1];
        const float dz = boundsMax[2] - boundsMin[2];
        const float diagonal = std::sqrt (dx * dx + dy * dy + dz * dz);
        if (diagonal > 0.0f)
            radius = 0.05f * diagonal;
    }
    radius = radius < 0.25f ? 0.25f : (radius > 10.0f ? 10.0f : radius);
    impl_->aoRadiusInUse = radius;
    return radius;
}

void DiligentScene::PrepareAmbientOcclusion (Diligent::IDeviceContext* context, const float view[16],
                                             const float proj[16], const float viewProj[16], const float eye[3],
                                             float nearClip, float farClip, float focusDistance, uint32_t frameIndex,
                                             CullMode cull)
{
    ClearAmbientOcclusion ();
    if (context == nullptr || !impl_->ready || impl_->gBuffer == nullptr || !impl_->aoEnabled ||
        impl_->aoIntensity <= 0.0f || impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return;
    if (!EnsureGBufferTargets ())
        return;

    RenderGBufferGeometry (context, viewProj, cull);

    // ⚠️ FINDING F5 IS CLOSED HERE (RE51.C2). This used to clear the motion
    // texture and reset the accumulation every frame, deliberately, because
    // there were no real vectors and a temporal effect fed invented history
    // ghosts. The prepass above now writes genuine per-pixel motion, so the
    // occlusion can accumulate across frames -- which is what takes the noise
    // out of the contact shadows RE51.C3 just switched on.
    Diligent::ITexture* motionTexture = impl_->gBuffer->GetBuffer (kGBufferMotion);

    // ⚠️ UNBIND BEFORE SAMPLING. The G-buffer's textures are render targets
    // until this line and shader resources after it; D3D11 resolves the overlap
    // by dropping the SRV, so an occlusion pass that skipped this would read
    // black and darken nothing, with no error anywhere.
    context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);

    impl_->ambientOcclusion.Init (impl_->device);
    Diligent::ITextureView* normalSrv =
        impl_->gBuffer->GetBuffer (kGBufferNormal)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    Diligent::ITextureView* depthSrv =
        impl_->gBuffer->GetBuffer (kGBufferDepth)->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    // ⚠️ REAL VECTORS NOW, NOT A CLEARED TEXTURE. The argument on the other side
    // is still named for what it used to be until C2 landed; see
    // DiligentAmbientOcclusion::Execute.
    Diligent::ITextureView* motionSrv = motionTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);

    impl_->aoView = impl_->ambientOcclusion.Execute (
        impl_->device, context, normalSrv, depthSrv, motionSrv, impl_->viewportWidth, impl_->viewportHeight,
        frameIndex, view, proj, viewProj, eye, nearClip, farClip, focusDistance, AmbientOcclusionRadius ());
}

void DiligentScene::DrawGBufferDebug (Diligent::IDeviceContext* context, Diligent::ITextureView* colorTarget,
                                      const float view[16], const float proj[16], const float viewProj[16],
                                      const float eye[3], float nearClip, float farClip, float focusDistance,
                                      bool perspective, uint32_t frameIndex, CullMode cull, int debugView)
{
    if (context == nullptr || colorTarget == nullptr || !impl_->ready || impl_->gBuffer == nullptr ||
        impl_->viewportWidth == 0 || impl_->viewportHeight == 0)
        return;

    if (!EnsureGBufferTargets ())
        return;
    impl_->drawCalls = 0;
    RenderGBufferGeometry (context, viewProj, cull);

    // ⚠️ A SECOND, FRESH CONSTANT BLOCK. The prepass above uploaded its own and
    // left the last MATERIAL's values in the buffer; the debug resolve below
    // needs frame values, not whatever range happened to be drawn last.
    DiligentSceneConstants constants;
    std::memcpy (constants.viewProj, viewProj, sizeof (float) * 16);

    // ⚠️ THE MOTION TARGET IS NO LONGER CLEARED HERE AND NO LONGER ZERO
    // (RE51.C2). It is a real render target of the prepass now, cleared by
    // GBuffer::Bind and written by kArchVizGBufferPS. The explicit clear that
    // used to stand here existed only to guarantee zeros for a temporal input
    // that had no vectors to offer; keeping it would erase them.
    Diligent::ITexture* motionTexture = impl_->gBuffer->GetBuffer (kGBufferMotion);

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
        // ⚠️ THE SAME RADIUS THE SHADED PATH USES. A debug view that exists to
        // diagnose the occlusion, showing a DIFFERENT occlusion from the one
        // being applied, is the worst possible way for a debug view to be
        // wrong -- it would send the next investigation somewhere else entirely.
        ambientOcclusionSrv = impl_->ambientOcclusion.Execute (
            impl_->device, context, normalSrv, depthSrv, motionSrv, impl_->viewportWidth, impl_->viewportHeight,
            frameIndex, view, proj, viewProj, eye, nearClip, farClip, focusDistance, AmbientOcclusionRadius ());
        if (ambientOcclusionSrv == nullptr)
            return;
        // Checked, for the reason the G-buffer binds above are: a variable that
        // is not on the SRB is NULL here, and dereferencing it takes Archicad
        // down rather than the viewport.
        Diligent::IShaderResourceVariable* aoVar =
            impl_->ambientOcclusionDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_ambientOcclusion");
        Diligent::IShaderResourceVariable* aoDepthVar =
            impl_->ambientOcclusionDebugSrb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_gbufferDepth");
        if (aoVar == nullptr || aoDepthVar == nullptr) {
            ArchVizLog ("ArchViz ambient occlusion debug: a shader variable is not on the SRB; add it to "
                        "`ambientOcclusionVariables` in DiligentScene::Init");
            return;
        }
        aoVar->Set (ambientOcclusionSrv);
        aoDepthVar->Set (depthSrv);
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

} // namespace archviz
} // namespace geomsrv
