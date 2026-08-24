#ifndef EVP_ARCHVIZ_DILIGENTEPIPOLARLIGHTSCATTERING_HPP
#define EVP_ARCHVIZ_DILIGENTEPIPOLARLIGHTSCATTERING_HPP

#include <cstdint>
#include <memory>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

class DiligentShadowMap;

class DiligentEpipolarLightScattering final {
  public:
    DiligentEpipolarLightScattering ();
    ~DiligentEpipolarLightScattering ();
    DiligentEpipolarLightScattering (const DiligentEpipolarLightScattering&) = delete;
    DiligentEpipolarLightScattering& operator= (const DiligentEpipolarLightScattering&) = delete;

    void Shutdown ();

    Diligent::ITextureView* Execute (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                     Diligent::ITextureView* sourceColor, Diligent::ITextureView* sourceDepth,
                                     const DiligentShadowMap& shadowMap, uint32_t width, uint32_t height,
                                     uint32_t frameIndex, const float view[16], const float proj[16],
                                     const float viewProj[16], const float eye[3], const float towardSun[3],
                                     float nearClip, float farClip, float siteAltitudeMetres, float intensity,
                                     bool lightShafts, bool lightingOnly);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
