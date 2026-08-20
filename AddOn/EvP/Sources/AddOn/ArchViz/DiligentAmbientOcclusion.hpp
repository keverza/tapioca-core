#ifndef EVP_ARCHVIZ_DILIGENTAMBIENTOCCLUSION_HPP
#define EVP_ARCHVIZ_DILIGENTAMBIENTOCCLUSION_HPP

#include <cstdint>
#include <memory>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

// Private owner for the concrete DiligentFX post-process structures. Keeping
// them here preserves DiligentScene.hpp's Diligent-free interface.
class DiligentAmbientOcclusion final {
  public:
    DiligentAmbientOcclusion ();
    ~DiligentAmbientOcclusion ();
    DiligentAmbientOcclusion (const DiligentAmbientOcclusion&) = delete;
    DiligentAmbientOcclusion& operator= (const DiligentAmbientOcclusion&) = delete;

    void Init (Diligent::IRenderDevice* device);
    void Shutdown ();

    Diligent::ITextureView* Execute (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                     Diligent::ITextureView* normal, Diligent::ITextureView* depth,
                                     Diligent::ITextureView* zeroMotion, uint32_t width, uint32_t height,
                                     uint32_t frameIndex, const float view[16], const float proj[16],
                                     const float viewProj[16], const float eye[3], float nearClip, float farClip,
                                     float focusDistance);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
