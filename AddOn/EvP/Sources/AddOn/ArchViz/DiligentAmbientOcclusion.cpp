#include "ArchViz/DiligentAmbientOcclusion.hpp"

#include <windows.h>
#include <d3d11.h>

#include <RefCntAutoPtr.hpp>
#include <Texture.h>
#include <TextureView.h>

#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/ScreenSpaceAmbientOcclusion/interface/ScreenSpaceAmbientOcclusion.hpp"

namespace Diligent {
namespace HLSL {

#include "Shaders/Common/public/BasicStructures.fxh"
#include "Shaders/PostProcess/ScreenSpaceAmbientOcclusion/public/ScreenSpaceAmbientOcclusionStructures.fxh"

} // namespace HLSL
} // namespace Diligent

namespace geomsrv {
namespace archviz {

struct DiligentAmbientOcclusion::Impl {
    std::unique_ptr<Diligent::PostFXContext> postFx;
    std::unique_ptr<Diligent::ScreenSpaceAmbientOcclusion> ssao;

    // ---- RE51.C2: the previous frame, kept here because nowhere else has it --
    // The G-buffer holds ONE depth buffer; reprojection needs last frame's as
    // well, to tell "this pixel moved" from "this pixel is now behind
    // something". Owned by the effect that consumes it.
    Diligent::RefCntAutoPtr<Diligent::ITexture> prevDepth;
    uint32_t prevDepthWidth = 0;
    uint32_t prevDepthHeight = 0;
    Diligent::HLSL::CameraAttribs prevCamera {};
    bool haveHistory = false;
};

DiligentAmbientOcclusion::DiligentAmbientOcclusion () : impl_ (std::make_unique<Impl> ())
{
}

DiligentAmbientOcclusion::~DiligentAmbientOcclusion ()
{
    Shutdown ();
}

void DiligentAmbientOcclusion::Init (Diligent::IRenderDevice* device)
{
    if (device == nullptr || impl_->postFx != nullptr)
        return;

    Diligent::PostFXContext::CreateInfo postFxInfo;
    postFxInfo.EnableAsyncCreation = false;
    postFxInfo.PackMatrixRowMajor = true;
    impl_->postFx = std::make_unique<Diligent::PostFXContext> (device, postFxInfo);

    Diligent::ScreenSpaceAmbientOcclusion::CreateInfo ssaoInfo;
    ssaoInfo.EnableAsyncCreation = false;
    impl_->ssao = std::make_unique<Diligent::ScreenSpaceAmbientOcclusion> (device, ssaoInfo);
}

void DiligentAmbientOcclusion::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->ssao.reset ();
    impl_->postFx.reset ();
    impl_->prevDepth.Release ();
    impl_->prevDepthWidth = 0;
    impl_->prevDepthHeight = 0;
    impl_->haveHistory = false;
}

void DiligentAmbientOcclusion::ResetHistory ()
{
    if (impl_ != nullptr)
        impl_->haveHistory = false;
}

Diligent::ITextureView* DiligentAmbientOcclusion::Execute (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* normal,
    Diligent::ITextureView* depth, Diligent::ITextureView* motion, uint32_t width, uint32_t height,
    uint32_t frameIndex, const float view[16], const float proj[16], const float viewProj[16], const float eye[3],
    float nearClip, float farClip, float focusDistance)
{
    if (device == nullptr || context == nullptr || normal == nullptr || depth == nullptr || motion == nullptr ||
        width == 0 || height == 0 || impl_->postFx == nullptr || impl_->ssao == nullptr)
        return nullptr;

    // ---- RE51.C2: this frame's depth, kept for the next one -----------------
    //
    // ⚠️ PostFXContext WANTS THE PREVIOUS FRAME'S DEPTH AS WELL AS THE MOTION,
    // and the G-buffer only ever holds ONE. Reprojection needs to know whether
    // the surface the motion vector points at is the same surface -- without the
    // old depth it cannot reject a pixel that moved BEHIND something, and the
    // occlusion bleeds through the occluder. So a private copy is kept here.
    //
    // ⚠️ SAME FORMAT AND SAME SIZE, OR CopyTexture SILENTLY DOES NOTHING. It is
    // reallocated whenever the viewport changes size, and the history is
    // declared invalid on that frame -- an old depth at a different resolution
    // is not a smaller version of this one.
    if (impl_->prevDepth == nullptr || impl_->prevDepthWidth != width || impl_->prevDepthHeight != height) {
        impl_->prevDepth.Release ();
        Diligent::TextureDesc td;
        td.Name = "ArchViz previous-frame depth (AO reprojection)";
        td.Type = Diligent::RESOURCE_DIM_TEX_2D;
        td.Width = width;
        td.Height = height;
        td.Format = Diligent::TEX_FORMAT_D32_FLOAT;
        td.MipLevels = 1;
        td.Usage = Diligent::USAGE_DEFAULT;
        td.BindFlags = Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE;
        device->CreateTexture (td, nullptr, &impl_->prevDepth);
        impl_->prevDepthWidth = width;
        impl_->prevDepthHeight = height;
        impl_->haveHistory = false;
    }
    if (impl_->prevDepth == nullptr)
        return nullptr;

    Diligent::HLSL::CameraAttribs camera {};
    camera.f4Position = Diligent::float4 { eye[0], eye[1], eye[2], 1.0f };
    camera.f4ViewportSize =
        Diligent::float4 { float (width), float (height), 1.0f / float (width), 1.0f / float (height) };
    camera.SetClipPlanes (nearClip, farClip);
    camera.fHandness = 1.0f;
    camera.uiFrameIndex = frameIndex;
    camera.fFocusDistance = focusDistance;
    camera.f2Jitter = Diligent::float2 { 0.0f, 0.0f };
    camera.mView = Diligent::float4x4::MakeMatrix (view);
    camera.mProj = Diligent::float4x4::MakeMatrix (proj);
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
    impl_->ssao->PrepareResources (device, context, impl_->postFx.get (),
                                   Diligent::ScreenSpaceAmbientOcclusion::FEATURE_FLAG_NONE);

    // ⚠️ WITHOUT HISTORY, BOTH SIDES ARE THIS FRAME, and that is the ONLY case
    // where the old always-reset behaviour is still correct: the first frame,
    // and the first frame after a resize or a reset. Anything else feeds
    // reprojection a depth buffer that describes a different image.
    Diligent::ITextureView* prevDepthSrv =
        impl_->prevDepth->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);

    Diligent::PostFXContext::RenderAttributes postFxAttributes;
    postFxAttributes.pDevice = device;
    postFxAttributes.pDeviceContext = context;
    postFxAttributes.pCurrDepthBufferSRV = depth;
    postFxAttributes.pPrevDepthBufferSRV = impl_->haveHistory ? prevDepthSrv : depth;
    postFxAttributes.pMotionVectorsSRV = motion;
    postFxAttributes.pCurrCamera = &camera;
    postFxAttributes.pPrevCamera = impl_->haveHistory ? &impl_->prevCamera : &camera;
    impl_->postFx->Execute (postFxAttributes);

    Diligent::HLSL::ScreenSpaceAmbientOcclusionAttribs settings {};
    // ⚠️ FINDING F5 IS CLOSED HERE. This was an unconditional TRUE, deliberately,
    // for as long as the motion texture was a cleared one -- a temporal effect
    // fed invented history ghosts, and a spatial-only proof that could not was
    // the honest first increment. Now it resets only when there is genuinely no
    // history to trust.
    settings.ResetAccumulation = impl_->haveHistory ? FALSE : TRUE;
    Diligent::ScreenSpaceAmbientOcclusion::RenderAttributes ssaoAttributes;
    ssaoAttributes.pDevice = device;
    ssaoAttributes.pDeviceContext = context;
    ssaoAttributes.pPostFXContext = impl_->postFx.get ();
    ssaoAttributes.pDepthBufferSRV = depth;
    ssaoAttributes.pNormalBufferSRV = normal;
    ssaoAttributes.pSSAOAttribs = &settings;
    impl_->ssao->Execute (ssaoAttributes);

    // ⚠️ THE COPY IS LAST, AFTER EVERYTHING HAS READ THE OLD ONE. Copying at the
    // top of the next frame would be the same instant and would also work; doing
    // it here keeps the whole history in one function, so there is no way to add
    // a second reader later and have it silently see the new depth.
    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = depth->GetTexture ();
    copy.pDstTexture = impl_->prevDepth;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copy);
    impl_->prevCamera = camera;
    impl_->haveHistory = true;

    return impl_->ssao->GetAmbientOcclusionSRV ();
}

} // namespace archviz
} // namespace geomsrv
