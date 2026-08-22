#ifndef EVP_ARCHVIZ_DILIGENTTEMPORALANTIALIASING_HPP
#define EVP_ARCHVIZ_DILIGENTTEMPORALANTIALIASING_HPP

#include <cstdint>
#include <memory>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

// Owns the concrete DiligentFX TAA and PostFXContext types while keeping them
// out of DiligentScene's public header.
class DiligentTemporalAntiAliasing final {
  public:
    DiligentTemporalAntiAliasing ();
    ~DiligentTemporalAntiAliasing ();
    DiligentTemporalAntiAliasing (const DiligentTemporalAntiAliasing&) = delete;
    DiligentTemporalAntiAliasing& operator= (const DiligentTemporalAntiAliasing&) = delete;

    void Init (Diligent::IRenderDevice* device);
    void Shutdown ();
    void ResetHistory ();

    // Prepare resources before the camera matrix is built, then return the
    // exact projection and jitter DiligentFX expects for this frame.
    bool Prepare (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                  uint32_t width, uint32_t height, uint32_t frameIndex,
                  const float projection[16], float jitteredProjection[16], float jitter[2]);

    Diligent::ITextureView* Execute (
        Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
        Diligent::ITextureView* color, Diligent::ITextureView* depth,
        Diligent::ITextureView* motion, uint32_t width, uint32_t height,
        uint32_t frameIndex, const float view[16], const float proj[16],
        const float viewProj[16], const float eye[3], float nearClip,
        float farClip, float focusDistance, const float jitter[2], float stability);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
