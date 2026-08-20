#include "ArchViz/DiligentAmbientOcclusion.hpp"

#include <windows.h>
#include <d3d11.h>

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
}

Diligent::ITextureView* DiligentAmbientOcclusion::Execute (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* normal,
    Diligent::ITextureView* depth, Diligent::ITextureView* zeroMotion, uint32_t width, uint32_t height,
    uint32_t frameIndex, const float view[16], const float proj[16], const float viewProj[16], const float eye[3],
    float nearClip, float farClip, float focusDistance)
{
    if (device == nullptr || context == nullptr || normal == nullptr || depth == nullptr || zeroMotion == nullptr ||
        width == 0 || height == 0 || impl_->postFx == nullptr || impl_->ssao == nullptr)
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

    Diligent::PostFXContext::RenderAttributes postFxAttributes;
    postFxAttributes.pDevice = device;
    postFxAttributes.pDeviceContext = context;
    postFxAttributes.pCurrDepthBufferSRV = depth;
    postFxAttributes.pPrevDepthBufferSRV = depth;
    postFxAttributes.pMotionVectorsSRV = zeroMotion;
    postFxAttributes.pCurrCamera = &camera;
    postFxAttributes.pPrevCamera = &camera;
    impl_->postFx->Execute (postFxAttributes);

    Diligent::HLSL::ScreenSpaceAmbientOcclusionAttribs settings {};
    settings.ResetAccumulation = TRUE;
    Diligent::ScreenSpaceAmbientOcclusion::RenderAttributes ssaoAttributes;
    ssaoAttributes.pDevice = device;
    ssaoAttributes.pDeviceContext = context;
    ssaoAttributes.pPostFXContext = impl_->postFx.get ();
    ssaoAttributes.pDepthBufferSRV = depth;
    ssaoAttributes.pNormalBufferSRV = normal;
    ssaoAttributes.pSSAOAttribs = &settings;
    impl_->ssao->Execute (ssaoAttributes);
    return impl_->ssao->GetAmbientOcclusionSRV ();
}

} // namespace archviz
} // namespace geomsrv
