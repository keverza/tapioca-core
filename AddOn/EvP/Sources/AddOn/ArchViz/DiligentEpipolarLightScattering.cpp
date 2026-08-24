#include "ArchViz/DiligentEpipolarLightScattering.hpp"

#include "ArchViz/DiligentPostFxCamera.hpp"
#include "ArchViz/DiligentShadowMap.hpp"

#include "PostProcess/EpipolarLightScattering/interface/EpipolarLightScattering.hpp"

#include <Texture.h>

#include <algorithm>
#include <cmath>
#include <exception>

namespace geomsrv {
namespace archviz {

struct DiligentEpipolarLightScattering::Impl {
    std::unique_ptr<Diligent::EpipolarLightScattering> effect;
    Diligent::RefCntAutoPtr<Diligent::ITexture> color;
    Diligent::RefCntAutoPtr<Diligent::ITexture> depth;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initFailed = false;

    bool EnsureResources (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, uint32_t requestedWidth,
                          uint32_t requestedHeight)
    {
        if (device == nullptr || context == nullptr || requestedWidth == 0 || requestedHeight == 0 || initFailed)
            return false;

        if (effect == nullptr) {
            try {
                Diligent::EpipolarLightScattering::CreateInfo createInfo;
                createInfo.pDevice = device;
                createInfo.pContext = context;
                createInfo.BackBufferFmt = Diligent::TEX_FORMAT_RGBA16_FLOAT;
                createInfo.DepthBufferFmt = Diligent::TEX_FORMAT_D32_FLOAT;
                createInfo.OffscreenBackBuffer = Diligent::TEX_FORMAT_RGBA16_FLOAT;
                createInfo.PackMatrixRowMajor = true;
                effect = std::make_unique<Diligent::EpipolarLightScattering> (createInfo);
            }
            catch (const std::exception&) {
                initFailed = true;
                return false;
            }
        }

        if (width == requestedWidth && height == requestedHeight && color != nullptr && depth != nullptr)
            return true;

        color.Release ();
        depth.Release ();
        width = 0;
        height = 0;

        Diligent::TextureDesc colorDesc;
        colorDesc.Name = "ArchViz epipolar atmosphere HDR";
        colorDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        colorDesc.Width = requestedWidth;
        colorDesc.Height = requestedHeight;
        colorDesc.Format = Diligent::TEX_FORMAT_RGBA16_FLOAT;
        colorDesc.BindFlags = Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
        device->CreateTexture (colorDesc, nullptr, &color);

        Diligent::TextureDesc depthDesc;
        depthDesc.Name = "ArchViz epipolar atmosphere scratch depth";
        depthDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        depthDesc.Width = requestedWidth;
        depthDesc.Height = requestedHeight;
        depthDesc.Format = Diligent::TEX_FORMAT_D32_FLOAT;
        depthDesc.BindFlags = Diligent::BIND_DEPTH_STENCIL;
        device->CreateTexture (depthDesc, nullptr, &depth);
        if (color == nullptr || depth == nullptr) {
            color.Release ();
            depth.Release ();
            return false;
        }

        width = requestedWidth;
        height = requestedHeight;
        effect->OnWindowResize (device, width, height);
        return true;
    }
};

DiligentEpipolarLightScattering::DiligentEpipolarLightScattering () : impl_ (std::make_unique<Impl> ())
{
}

DiligentEpipolarLightScattering::~DiligentEpipolarLightScattering ()
{
    Shutdown ();
}

void DiligentEpipolarLightScattering::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->color.Release ();
    impl_->depth.Release ();
    impl_->effect.reset ();
    impl_->width = 0;
    impl_->height = 0;
    impl_->initFailed = false;
}

Diligent::ITextureView* DiligentEpipolarLightScattering::Execute (
    Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* sourceColor,
    Diligent::ITextureView* sourceDepth, const DiligentShadowMap& shadowMap, uint32_t width, uint32_t height,
    uint32_t frameIndex, const float view[16], const float proj[16], const float viewProj[16], const float eye[3],
    const float towardSun[3], float nearClip, float farClip, float siteAltitudeMetres, float intensity,
    bool lightShafts, bool lightingOnly)
{
    if (impl_ == nullptr || sourceColor == nullptr || sourceDepth == nullptr || view == nullptr || proj == nullptr ||
        viewProj == nullptr || eye == nullptr || towardSun == nullptr ||
        !impl_->EnsureResources (device, context, width, height))
        return nullptr;

    Diligent::CameraAttribs camera {};
    camera.f4Position = Diligent::float4 { eye[0], eye[1], eye[2], 1.0f };
    camera.f4ViewportSize =
        Diligent::float4 { float (width), float (height), 1.0f / float (width), 1.0f / float (height) };
    camera.SetClipPlanes (nearClip, farClip * 0.999999f);
    camera.fHandness = -1.0f;
    camera.uiFrameIndex = frameIndex;
    camera.mView = PostFxViewMatrix (view);
    camera.mProj = PostFxProjMatrix (proj);
    camera.mViewProj = Diligent::float4x4::MakeMatrix (viewProj);
    camera.mViewInv = camera.mView.Inverse ();
    camera.mProjInv = camera.mProj.Inverse ();
    camera.mViewProjInv = camera.mViewProj.Inverse ();

    const float sunLength =
        std::sqrt (towardSun[0] * towardSun[0] + towardSun[1] * towardSun[1] + towardSun[2] * towardSun[2]);
    if (!(sunLength > 1e-6f))
        return nullptr;

    Diligent::LightAttribs light {};
    light.f4Direction =
        Diligent::float4 { -towardSun[0] / sunLength, -towardSun[1] / sunLength, -towardSun[2] / sunLength, 0.0f };
    const float radiance = (std::max) (0.0f, intensity);
    light.f4Intensity = Diligent::float4 { radiance, radiance, radiance, radiance };
    shadowMap.CopyAttribs (light.ShadowAttribs);

    Diligent::EpipolarLightScatteringAttribs attributes;
    const uint32_t shadowResolution = shadowMap.Resolution ();
    const int cascadeCount = static_cast<int> (shadowMap.CascadeCount ());
    attributes.iNumCascades = cascadeCount;
    attributes.iFirstCascadeToRayMarch = (std::max) (0, (std::min) (2, cascadeCount - 1));
    attributes.fMaxShadowMapStep = float (shadowResolution / 4u);
    attributes.f2ShadowMapTexelSize =
        shadowResolution > 0 ? Diligent::float2 { 1.0f / float (shadowResolution), 1.0f / float (shadowResolution) }
                             : Diligent::float2 {};
    attributes.uiMaxSamplesOnTheRay = shadowResolution;
    attributes.uiMinMaxShadowMapResolution = shadowResolution;
    attributes.bEnableLightShafts = lightShafts && shadowMap.IsFitted () ? 1 : 0;
    attributes.bShowLightingOnly = lightingOnly ? 1 : 0;
    // Archicad geometry is Z-up and project zero sits at the extracted site
    // altitude. The sample is Y-up at sea level, so both the axis and origin
    // must move; otherwise a high-altitude project is treated as sea level.
    attributes.f4EarthCenter = Diligent::float4 { 0.0f, 0.0f, -(6371000.0f + siteAltitudeMetres), 0.0f };
    attributes.ToneMapping.iToneMappingMode = TONE_MAPPING_MODE_NONE;
    attributes.ToneMapping.bAutoExposure = 0;
    attributes.ToneMapping.bLightAdaptation = 0;

    Diligent::EpipolarLightScattering::FrameAttribs frame;
    frame.pDevice = device;
    frame.pDeviceContext = context;
    frame.pLightAttribs = &light;
    frame.pCameraAttribs = &camera;
    frame.ptex2DSrcColorBufferSRV = sourceColor;
    frame.ptex2DSrcDepthBufferSRV = sourceDepth;
    frame.ptex2DDstColorBufferRTV = impl_->color->GetDefaultView (Diligent::TEXTURE_VIEW_RENDER_TARGET);
    frame.ptex2DDstDepthBufferDSV = impl_->depth->GetDefaultView (Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
    frame.ptex2DShadowMapSRV = attributes.bEnableLightShafts ? shadowMap.ShaderView () : nullptr;

    try {
        impl_->effect->PrepareForNewFrame (frame, attributes);
        impl_->effect->PerformPostProcessing ();
    }
    catch (const std::exception&) {
        return nullptr;
    }
    return impl_->color->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
}

} // namespace archviz
} // namespace geomsrv
