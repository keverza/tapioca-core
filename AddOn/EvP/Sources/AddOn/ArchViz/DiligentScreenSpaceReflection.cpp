#include "ArchViz/DiligentScreenSpaceReflection.hpp"

#include <windows.h>
#include <d3d11.h>

#include <RefCntAutoPtr.hpp>
#include <Texture.h>
#include <TextureView.h>

#include "PostProcess/Common/interface/PostFXContext.hpp"
#include "PostProcess/ScreenSpaceReflection/interface/ScreenSpaceReflection.hpp"

namespace Diligent {
namespace HLSL {

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
    bool haveHistory = false;
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
    impl_->haveHistory = false;
}

void DiligentScreenSpaceReflection::ResetHistory ()
{
    if (impl_ != nullptr)
        impl_->haveHistory = false;
}

Diligent::ITextureView* DiligentScreenSpaceReflection::Execute (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
    Diligent::ITextureView* color, Diligent::ITextureView* depth,
    Diligent::ITextureView* normal, Diligent::ITextureView* material,
    Diligent::ITextureView* motion, uint32_t width, uint32_t height,
    uint32_t frameIndex, const float view[16], const float proj[16],
    const float viewProj[16], const float eye[3], float nearClip, float farClip,
    float focusDistance, float intensity, float roughnessThreshold)
{
    if (device == nullptr || context == nullptr || color == nullptr || depth == nullptr ||
        normal == nullptr || material == nullptr || motion == nullptr ||
        width == 0 || height == 0 || impl_->postFx == nullptr || impl_->ssr == nullptr)
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
        impl_->haveHistory = false;
    }
    if (impl_->prevColor == nullptr || impl_->prevDepth == nullptr)
        return nullptr;

    Diligent::ITextureView* prevColorSrv =
        impl_->prevColor->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    Diligent::ITextureView* prevDepthSrv =
        impl_->prevDepth->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);

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
    postFxAttributes.pPrevDepthBufferSRV = impl_->haveHistory ? prevDepthSrv : depth;
    postFxAttributes.pMotionVectorsSRV = motion;
    postFxAttributes.pCurrCamera = &camera;
    postFxAttributes.pPrevCamera = impl_->haveHistory ? &impl_->prevCamera : &camera;
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
    // ⚠️ RESET ACCUMULATION WHEN THERE IS NO HISTORY. The temporal pass ghosts
    // if it reprojects from a frame that does not exist.
    settings.TemporalRadianceStabilityFactor = impl_->haveHistory ? 1.0f : 0.0f;
    settings.TemporalVarianceStabilityFactor = impl_->haveHistory ? 0.9f : 0.0f;
    settings.AlphaInterpolation = intensity;

    // ⚠️ THE COLOUR INPUT IS THE PREVIOUS FRAME'S when history exists, because
    // SSR's temporal mode looks up what the reflected pixel showed LAST frame
    // and reprojects it. On the first frame it is the current frame's, which is
    // the only honest thing to do when no previous frame exists.
    Diligent::ITextureView* colorInput = impl_->haveHistory ? prevColorSrv : color;

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

    // ⚠️ COPY CURRENT COLOUR AND DEPTH FOR NEXT FRAME, after everything has
    // read the old ones. Same ordering as the AO's prevDepth copy.
    Diligent::CopyTextureAttribs copyColor;
    copyColor.pSrcTexture = color->GetTexture ();
    copyColor.pDstTexture = impl_->prevColor;
    copyColor.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copyColor.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copyColor);

    Diligent::CopyTextureAttribs copyDepth;
    copyDepth.pSrcTexture = depth->GetTexture ();
    copyDepth.pDstTexture = impl_->prevDepth;
    copyDepth.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copyDepth.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture (copyDepth);

    impl_->prevCamera = camera;
    impl_->haveHistory = true;

    return impl_->ssr->GetSSRRadianceSRV ();
}

} // namespace archviz
} // namespace geomsrv
