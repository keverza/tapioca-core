#include "ArchViz/DiligentScreenSpaceReflection.hpp"
#include "ArchViz/DiligentPostFxCamera.hpp"

#include <windows.h>
#include <d3d11.h>

#include <APIInfo.h>
#include <RefCntAutoPtr.hpp>
#include <Texture.h>
#include <TextureView.h>

#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/ScreenSpaceReflection/interface/ScreenSpaceReflection.hpp"

namespace Diligent {
namespace HLSL {

#include "Shaders/Common/public/BasicStructures.fxh"
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/ScreenSpaceReflection/public/ScreenSpaceReflectionStructures.fxh"

} // namespace HLSL
} // namespace Diligent

namespace geomsrv {
namespace archviz {

struct DiligentScreenSpaceReflection::Impl {
    std::unique_ptr<Diligent::PostFXContext> postFx;
    std::unique_ptr<Diligent::ScreenSpaceReflection> ssr;

    // The previous frame's colour and depth, kept for temporal reprojection.
    // ⚠️ SSR's FEATURE_FLAG_PREVIOUS_FRAME mode needs the previous frame's
    // colour to look up what the reflected pixel showed last frame, and the
    // previous depth to reject disocclusions. The G-buffer holds only one of
    // each, so the effect owns its own copies -- same shape as the AO's
    // prevDepth, extended with a prevColor.
    Diligent::RefCntAutoPtr<Diligent::ITexture> prevColor;
    Diligent::RefCntAutoPtr<Diligent::ITexture> prevDepth;
    uint32_t prevWidth = 0;
    uint32_t prevHeight = 0;
    Diligent::HLSL::CameraAttribs prevCamera {};
    // ⚠️ TWO HISTORIES, AND CONFLATING THEM COST A DAY. They are stored at
    // different points in the frame and govern different things:
    //
    //   haveDepthHistory  set at the END OF Execute, when prevDepth and
    //                     prevCamera are written. Governs what PostFXContext is
    //                     told about the previous frame.
    //   haveColorHistory  set in RememberFrame, at the END OF THE FRAME, once
    //                     the accumulated radiance exists. Governs only which
    //                     texture SSR samples its reflections FROM.
    //
    // ⚠️ NEITHER OF THEM GATES DILIGENTFX'S OWN TEMPORAL ACCUMULATION any more.
    // A single flag used to, and when RememberFrame took over setting it the
    // denoiser silently switched off for good: TemporalRadianceStabilityFactor
    // pinned at 0 means `lerp(current, history, 0)` -- the history is computed
    // and discarded every frame, which is precisely "the jitter is very large
    // and does not settle". SSR runs its own reprojection and disocclusion test
    // (SSR_ComputeTemporalAccumulation's ComputeReprojection); second-guessing
    // it from out here was never this code's business.
    bool haveDepthHistory = false;
    bool haveColorHistory = false;
};

DiligentScreenSpaceReflection::DiligentScreenSpaceReflection () : impl_ (std::make_unique<Impl> ())
{
}

DiligentScreenSpaceReflection::~DiligentScreenSpaceReflection ()
{
    Shutdown ();
}

void DiligentScreenSpaceReflection::Init (Diligent::IRenderDevice* device)
{
    if (device == nullptr || impl_->postFx != nullptr)
        return;

    Diligent::PostFXContext::CreateInfo postFxInfo;
    postFxInfo.EnableAsyncCreation = false;
    postFxInfo.PackMatrixRowMajor = true;
    impl_->postFx = std::make_unique<Diligent::PostFXContext> (device, postFxInfo);

    Diligent::ScreenSpaceReflection::CreateInfo ssrInfo;
    ssrInfo.EnableAsyncCreation = false;
    impl_->ssr = std::make_unique<Diligent::ScreenSpaceReflection> (device, ssrInfo);
}

void DiligentScreenSpaceReflection::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->ssr.reset ();
    impl_->postFx.reset ();
    impl_->prevColor.Release ();
    impl_->prevDepth.Release ();
    impl_->prevWidth = 0;
    impl_->prevHeight = 0;
    impl_->haveDepthHistory = false;
    impl_->haveColorHistory = false;
}

void DiligentScreenSpaceReflection::ResetHistory ()
{
    if (impl_ != nullptr) {
        impl_->haveDepthHistory = false;
        impl_->haveColorHistory = false;
    }
}

Diligent::ITextureView* DiligentScreenSpaceReflection::Execute (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* color,
    Diligent::ITextureView* depth, Diligent::ITextureView* normal, Diligent::ITextureView* material,
    Diligent::ITextureView* motion, uint32_t width, uint32_t height, uint32_t frameIndex, const float view[16],
    const float proj[16], const float viewProj[16], const float eye[3], const float jitter[2], float nearClip,
    float farClip, float focusDistance, float roughnessThreshold)
{
    if (device == nullptr || context == nullptr || color == nullptr || depth == nullptr || normal == nullptr ||
        material == nullptr || motion == nullptr || width == 0 || height == 0 || impl_->postFx == nullptr ||
        impl_->ssr == nullptr)
        return nullptr;

    // ⚠️ PREVIOUS-FRAME COLOUR AND DEPTH, reallocated on resize. The SSR's
    // temporal pass reprojectes the previous frame's radiance using the motion
    // vectors, so without a genuine previous colour it would be reprojecting
    // from nothing. Same pattern as the AO's prevDepth, extended to carry the
    // colour as well.
    if (impl_->prevColor == nullptr || impl_->prevWidth != width || impl_->prevHeight != height) {
        impl_->prevColor.Release ();
        Diligent::TextureDesc colorDesc;
        colorDesc.Name = "ArchViz previous-frame colour (SSR reprojection)";
        colorDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        colorDesc.Width = width;
        colorDesc.Height = height;
        colorDesc.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        colorDesc.MipLevels = 1;
        colorDesc.Usage = Diligent::USAGE_DEFAULT;
        colorDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_RENDER_TARGET;
        device->CreateTexture (colorDesc, nullptr, &impl_->prevColor);

        impl_->prevDepth.Release ();
        Diligent::TextureDesc depthDesc;
        depthDesc.Name = "ArchViz previous-frame depth (SSR reprojection)";
        depthDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.Format = Diligent::TEX_FORMAT_D32_FLOAT;
        depthDesc.MipLevels = 1;
        depthDesc.Usage = Diligent::USAGE_DEFAULT;
        depthDesc.BindFlags = Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE;
        device->CreateTexture (depthDesc, nullptr, &impl_->prevDepth);

        impl_->prevWidth = width;
        impl_->prevHeight = height;
        impl_->haveDepthHistory = false;
        impl_->haveColorHistory = false;
    }
    if (impl_->prevColor == nullptr || impl_->prevDepth == nullptr)
        return nullptr;

    Diligent::ITextureView* prevColorSrv = impl_->prevColor->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    Diligent::ITextureView* prevDepthSrv = impl_->prevDepth->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);

    Diligent::HLSL::CameraAttribs camera {};
    camera.f4Position = Diligent::float4 { eye[0], eye[1], eye[2], 1.0f };
    camera.f4ViewportSize =
        Diligent::float4 { float (width), float (height), 1.0f / float (width), 1.0f / float (height) };
#if DILIGENT_API_VERSION >= 256020
    camera.SetClipPlanes (nearClip, farClip);
#else
    camera.fNearPlaneZ = nearClip;
    camera.fFarPlaneZ = farClip;
#endif
    // ⚠️ -1 IS "LEFT-HANDED" (BasicStructures.fxh). The matrices below are
    // converted, so this now describes them honestly. Nothing under PostProcess/
    // actually reads it -- which is exactly why the mismatch it should have
    // caught went unnoticed for as long as it did.
    camera.fHandness = -1.0f;
    camera.uiFrameIndex = frameIndex;
    camera.fFocusDistance = focusDistance;
    camera.f2Jitter = Diligent::float2 { jitter != nullptr ? jitter[0] : 0.0f, jitter != nullptr ? jitter[1] : 0.0f };
    // ⚠️ CONVERTED TO DILIGENTFX'S LEFT-HANDED CONVENTION, which is not this
    // renderer's. See DiligentPostFxCamera.hpp for what breaks without it.
    // mViewProj is deliberately NOT converted: the conversion leaves the
    // view-projection bit-for-bit identical, which is the whole reason it is
    // safe to make here rather than in the camera.
    camera.mView = PostFxViewMatrix (view);
    camera.mProj = PostFxProjMatrix (proj);
    camera.mViewProj = Diligent::float4x4::MakeMatrix (viewProj);
    camera.mViewInv = camera.mView.Inverse ();
    camera.mProjInv = camera.mProj.Inverse ();
    camera.mViewProjInv = camera.mViewProj.Inverse ();

    Diligent::PostFXContext::FrameDesc frame;
    frame.Index = frameIndex;
    frame.Width = width;
    frame.Height = height;
    frame.OutputWidth = width;
    frame.OutputHeight = height;
    impl_->postFx->PrepareResources (device, frame, Diligent::PostFXContext::FEATURE_FLAG_NONE);

    // ⚠️ PREVIOUS_FRAME mode uses the motion vectors C2 landed to reproject
    // last frame's colour. Without it the effect is spatial-only and noisier.
    const Diligent::ScreenSpaceReflection::FEATURE_FLAGS flags =
        Diligent::ScreenSpaceReflection::FEATURE_FLAG_PREVIOUS_FRAME;

    impl_->ssr->PrepareResources (device, context, impl_->postFx.get (), flags);

    Diligent::PostFXContext::RenderAttributes postFxAttributes;
    postFxAttributes.pDevice = device;
    postFxAttributes.pDeviceContext = context;
    postFxAttributes.pCurrDepthBufferSRV = depth;
    // ⚠️ WITHOUT HISTORY, BOTH SIDES ARE THIS FRAME. Reprojecting from the
    // current frame is the only honest thing to do on the first frame or after
    // a resize -- invented history ghosts. Same contract as the AO pass.
    postFxAttributes.pPrevDepthBufferSRV = impl_->haveDepthHistory ? prevDepthSrv : depth;
    postFxAttributes.pMotionVectorsSRV = motion;
    postFxAttributes.pCurrCamera = &camera;
    postFxAttributes.pPrevCamera = impl_->haveDepthHistory ? &impl_->prevCamera : &camera;
    impl_->postFx->Execute (postFxAttributes);

    Diligent::HLSL::ScreenSpaceReflectionAttribs settings {};
    settings.DepthBufferThickness = 0.025f;
    // ⚠️ THE ROUGHNESS THRESHOLD GATES WHICH SURFACES SPAWN RAYS. A surface
    // rougher than this gets no SSR, which is correct: a rough surface's
    // reflection is a diffuse blur that screen-space rays cannot represent.
    // The HUD exposes this so the user can widen or narrow the effect.
    settings.RoughnessThreshold = roughnessThreshold;
    settings.IsRoughnessPerceptual = TRUE;
    settings.RoughnessChannel = 0;
    settings.MaxTraversalIntersections = 128;
    // ⚠️ DILIGENTFX'S DEFAULTS, UNCONDITIONALLY, AND THE "UNCONDITIONALLY" IS
    // THE FIX. These used to be zeroed whenever this class thought it had no
    // history -- but 0 does not mean "reset", it means `lerp(current, history,
    // 0)`: the accumulation still runs and its result is thrown away every
    // frame. SSR then shows the raw one-ray-per-pixel trace forever, which on a
    // near-mirror surface is violent, because a reflection MAGNIFIES the
    // sub-pixel jitter of the surface it came off.
    //
    // ⚠️ THE FIRST FRAME NEEDS NO SPECIAL CASE. SSR_ComputeTemporalAccumulation
    // reprojects and tests disocclusion itself (ComputeReprojection ->
    // SSR_DISOCCLUSION_THRESHOLD) and falls back to the current radiance when
    // that fails, which is exactly what a first frame is.
    settings.TemporalRadianceStabilityFactor = 1.0f;
    settings.TemporalVarianceStabilityFactor = 0.9f;
    // Intensity belongs to the composition pass. AlphaInterpolation scales the
    // effect's confidence output; values above one make a later lerp extrapolate
    // past the reflected radiance and produce bright streaks.
    settings.AlphaInterpolation = 1.0f;

    // ⚠️ THE COLOUR INPUT IS THE PREVIOUS FRAME'S when history exists, because
    // SSR's temporal mode looks up what the reflected pixel showed LAST frame
    // and reprojects it. On the first frame it is the current frame's, which is
    // the only honest thing to do when no previous frame exists.
    Diligent::ITextureView* colorInput = impl_->haveColorHistory ? prevColorSrv : color;

    Diligent::ScreenSpaceReflection::RenderAttributes ssrAttributes;
    ssrAttributes.pDevice = device;
    ssrAttributes.pDeviceContext = context;
    ssrAttributes.pPostFXContext = impl_->postFx.get ();
    ssrAttributes.pColorBufferSRV = colorInput;
    ssrAttributes.pDepthBufferSRV = depth;
    ssrAttributes.pNormalBufferSRV = normal;
    ssrAttributes.pMaterialBufferSRV = material;
    ssrAttributes.pMotionVectorsSRV = motion;
    ssrAttributes.pSSRAttribs = &settings;
    impl_->ssr->Execute (ssrAttributes);

    // ⚠️ THE COLOUR COPY IS NOT HERE ANY MORE -- SEE RememberFrame. It used to
    // copy this frame's RAW HDR target, which is the JITTERED, un-accumulated
    // image, so the radiance every reflection was drawn from shimmered before
    // the ray even hit it. Tutorial27_PostProcessing calls UpdateSSRSourceColor
    // with the TAA-ACCUMULATED frame, after TAA has run; RememberFrame is that
    // call, and it cannot happen here because TAA has not run yet.
    //
    // ⚠️ THE DEPTH COPY STAYS, because depth is not accumulated by anything and
    // this is still the right moment for it: after every reader of the old one.
    Diligent::CopyTextureAttribs copyDepth;
    copyDepth.pSrcTexture = depth->GetTexture ();
    copyDepth.pDstTexture = impl_->prevDepth;
    copyDepth.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copyDepth.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copyDepth);

    impl_->prevCamera = camera;
    // The depth and camera for next frame are stored NOW; the colour is not,
    // and cannot be -- it does not exist until TAA has run. See RememberFrame.
    impl_->haveDepthHistory = true;

    return impl_->ssr->GetSSRRadianceSRV ();
}

// The radiance next frame's rays will read, remembered at the END of the frame.
//
// ⚠️ IT MUST BE THE ACCUMULATED IMAGE, NOT THE RAW ONE. SSR's
// FEATURE_FLAG_PREVIOUS_FRAME mode looks up what the reflected pixel showed last
// frame; handing it the raw jittered target means every reflection samples a
// shimmering source and the effect's own denoiser spends itself fighting noise
// the renderer introduced. Tutorial27_PostProcessing feeds UpdateSSRSourceColor
// the post-TAA frame for exactly this reason.
//
// ⚠️ IT SETS ONLY THE COLOUR HISTORY. A frame that resolves but never gets here
// -- the pass failed, the view was null -- must not claim a colour it never
// stored, or the next frame reflects whatever the uninitialised copy held. It
// must equally NOT hold back the depth history or the denoiser, which is the
// mistake the two-flag split above exists to prevent.
void DiligentScreenSpaceReflection::RememberFrame (Diligent::IDeviceContext* context, Diligent::ITextureView* resolved)
{
    if (context == nullptr || resolved == nullptr || impl_->prevColor == nullptr)
        return;

    Diligent::ITexture* src = resolved->GetTexture ();
    if (src == nullptr)
        return;
    const Diligent::TextureDesc& desc = src->GetDesc ();
    if (desc.Width != impl_->prevWidth || desc.Height != impl_->prevHeight)
        return;

    Diligent::CopyTextureAttribs copyColor;
    copyColor.pSrcTexture = src;
    copyColor.pDstTexture = impl_->prevColor;
    copyColor.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copyColor.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copyColor);

    impl_->haveColorHistory = true;
}

} // namespace archviz
} // namespace geomsrv
