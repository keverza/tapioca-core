#include "ArchViz/DiligentTemporalAntiAliasing.hpp"

#include <windows.h>
#include <d3d11.h>

#include <RefCntAutoPtr.hpp>
#include <Texture.h>
#include <TextureView.h>

#include <cstring>

#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/TemporalAntiAliasing/interface/TemporalAntiAliasing.hpp"

namespace Diligent {
namespace HLSL {

#include "Shaders/Common/public/BasicStructures.fxh"
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/TemporalAntiAliasing/public/TemporalAntiAliasingStructures.fxh"

} // namespace HLSL
} // namespace Diligent

namespace geomsrv {
namespace archviz {

struct DiligentTemporalAntiAliasing::Impl {
    std::unique_ptr<Diligent::PostFXContext> postFx;
    std::unique_ptr<Diligent::TemporalAntiAliasing> taa;
    Diligent::RefCntAutoPtr<Diligent::ITexture> prevDepth;
    uint32_t prevWidth = 0;
    uint32_t prevHeight = 0;
    uint32_t preparedFrame = ~0u;
    uint32_t lastExecutedFrame = ~0u;
    Diligent::HLSL::CameraAttribs prevCamera {};
    bool haveHistory = false;
};

DiligentTemporalAntiAliasing::DiligentTemporalAntiAliasing () : impl_ (std::make_unique<Impl> ())
{
}

DiligentTemporalAntiAliasing::~DiligentTemporalAntiAliasing ()
{
    Shutdown ();
}

void DiligentTemporalAntiAliasing::Init (Diligent::IRenderDevice* device)
{
    if (device == nullptr || impl_->postFx != nullptr)
        return;

    Diligent::PostFXContext::CreateInfo postFxInfo;
    postFxInfo.EnableAsyncCreation = false;
    postFxInfo.PackMatrixRowMajor = true;
    impl_->postFx = std::make_unique<Diligent::PostFXContext> (device, postFxInfo);

    Diligent::TemporalAntiAliasing::CreateInfo taaInfo;
    taaInfo.EnableAsyncCreation = false;
    impl_->taa = std::make_unique<Diligent::TemporalAntiAliasing> (device, taaInfo);
}

void DiligentTemporalAntiAliasing::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->taa.reset ();
    impl_->postFx.reset ();
    impl_->prevDepth.Release ();
    impl_->prevWidth = 0;
    impl_->prevHeight = 0;
    impl_->preparedFrame = ~0u;
    impl_->lastExecutedFrame = ~0u;
    impl_->haveHistory = false;
}

void DiligentTemporalAntiAliasing::ResetHistory ()
{
    if (impl_ != nullptr) {
        impl_->haveHistory = false;
        impl_->lastExecutedFrame = ~0u;
    }
}

bool DiligentTemporalAntiAliasing::Prepare (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
    uint32_t width, uint32_t height, uint32_t frameIndex,
    const float projection[16], float jitteredProjection[16], float jitter[2])
{
    if (projection != nullptr && jitteredProjection != nullptr)
        std::memcpy (jitteredProjection, projection, sizeof (float) * 16);
    if (jitter != nullptr) {
        jitter[0] = 0.0f;
        jitter[1] = 0.0f;
    }
    if (device == nullptr || context == nullptr || projection == nullptr || jitteredProjection == nullptr ||
        jitter == nullptr || width == 0 || height == 0 || impl_->postFx == nullptr || impl_->taa == nullptr)
        return false;
    impl_->preparedFrame = ~0u;

    if (impl_->prevDepth == nullptr || impl_->prevWidth != width || impl_->prevHeight != height) {
        impl_->prevDepth.Release ();
        Diligent::TextureDesc td;
        td.Name = "ArchViz previous-frame depth (TAA reprojection)";
        td.Type = Diligent::RESOURCE_DIM_TEX_2D;
        td.Width = width;
        td.Height = height;
        td.Format = Diligent::TEX_FORMAT_D32_FLOAT;
        td.MipLevels = 1;
        td.Usage = Diligent::USAGE_DEFAULT;
        td.BindFlags = Diligent::BIND_DEPTH_STENCIL | Diligent::BIND_SHADER_RESOURCE;
        device->CreateTexture (td, nullptr, &impl_->prevDepth);
        impl_->prevWidth = width;
        impl_->prevHeight = height;
        impl_->haveHistory = false;
    }
    if (impl_->prevDepth == nullptr)
        return false;

    Diligent::PostFXContext::FrameDesc frame;
    frame.Index = frameIndex;
    frame.Width = width;
    frame.Height = height;
    frame.OutputWidth = width;
    frame.OutputHeight = height;
    impl_->postFx->PrepareResources (device, frame, Diligent::PostFXContext::FEATURE_FLAG_NONE);

    const auto flags = Diligent::TemporalAntiAliasing::FEATURE_FLAG_BICUBIC_FILTER;
    impl_->taa->PrepareResources (device, context, impl_->postFx.get (), flags);
    if (impl_->taa->GetAccumulatedFrameSRV () == nullptr)
        return false;
    const Diligent::float2 offset = impl_->taa->GetJitterOffset ();
    const Diligent::float4x4 base = Diligent::float4x4::MakeMatrix (projection);
    const Diligent::float4x4 jittered = Diligent::TemporalAntiAliasing::GetJitteredProjMatrix (base, offset);
    std::memcpy (jitteredProjection, jittered.Data (), sizeof (float) * 16);
    jitter[0] = offset.x;
    jitter[1] = offset.y;
    impl_->preparedFrame = frameIndex;
    return true;
}

Diligent::ITextureView* DiligentTemporalAntiAliasing::Execute (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
    Diligent::ITextureView* color, Diligent::ITextureView* depth,
    Diligent::ITextureView* motion, uint32_t width, uint32_t height,
    uint32_t frameIndex, const float view[16], const float proj[16],
    const float viewProj[16], const float eye[3], float nearClip,
    float farClip, float focusDistance, const float jitter[2], float stability)
{
    if (device == nullptr || context == nullptr || color == nullptr || depth == nullptr || motion == nullptr ||
        width == 0 || height == 0 || view == nullptr || proj == nullptr || viewProj == nullptr || eye == nullptr ||
        jitter == nullptr || impl_->postFx == nullptr || impl_->taa == nullptr || impl_->preparedFrame != frameIndex)
        return nullptr;

    if (impl_->lastExecutedFrame != ~0u && frameIndex != impl_->lastExecutedFrame + 1)
        impl_->haveHistory = false;

    Diligent::HLSL::CameraAttribs camera {};
    camera.f4Position = Diligent::float4 { eye[0], eye[1], eye[2], 1.0f };
    camera.f4ViewportSize =
        Diligent::float4 { float (width), float (height), 1.0f / float (width), 1.0f / float (height) };
    camera.SetClipPlanes (nearClip, farClip);
    camera.fHandness = 1.0f;
    camera.uiFrameIndex = frameIndex;
    camera.fFocusDistance = focusDistance;
    camera.f2Jitter = Diligent::float2 { jitter[0], jitter[1] };
    camera.mView = Diligent::float4x4::MakeMatrix (view);
    camera.mProj = Diligent::float4x4::MakeMatrix (proj);
    camera.mViewProj = Diligent::float4x4::MakeMatrix (viewProj);
    camera.mViewInv = camera.mView.Inverse ();
    camera.mProjInv = camera.mProj.Inverse ();
    camera.mViewProjInv = camera.mViewProj.Inverse ();

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

    Diligent::HLSL::TemporalAntiAliasingAttribs settings {};
    settings.TemporalStabilityFactor = stability < 0.0f ? 0.0f : (stability > 1.0f ? 1.0f : stability);
    settings.ResetAccumulation = impl_->haveHistory ? FALSE : TRUE;

    Diligent::TemporalAntiAliasing::RenderAttributes attributes;
    attributes.pDevice = device;
    attributes.pDeviceContext = context;
    attributes.pPostFXContext = impl_->postFx.get ();
    attributes.pColorBufferSRV = color;
    attributes.pTAAAttribs = &settings;
    impl_->taa->Execute (attributes);

    Diligent::CopyTextureAttribs copy;
    copy.pSrcTexture = depth->GetTexture ();
    copy.pDstTexture = impl_->prevDepth;
    copy.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copy);

    impl_->prevCamera = camera;
    impl_->haveHistory = true;
    impl_->lastExecutedFrame = frameIndex;
    return impl_->taa->GetAccumulatedFrameSRV ();
}

} // namespace archviz
} // namespace geomsrv
