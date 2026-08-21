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

    // ⚠️ `motion` WAS `zeroMotion` UNTIL RE51.C2, and the rename is the whole
    // change: this used to be handed a cleared texture and told to reset its
    // accumulation every frame, so it was a SPATIAL effect wearing a temporal
    // effect's API (finding F5). It now receives genuine per-pixel vectors and
    // keeps its own copy of the previous frame's depth, so the occlusion
    // accumulates -- which is what takes the noise out of it.
    Diligent::ITextureView* Execute (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                                     Diligent::ITextureView* normal, Diligent::ITextureView* depth,
                                     Diligent::ITextureView* motion, uint32_t width, uint32_t height,
                                     uint32_t frameIndex, const float view[16], const float proj[16],
                                     const float viewProj[16], const float eye[3], float nearClip, float farClip,
                                     float focusDistance);

    // Throw the history away. ⚠️ CALL IT WHENEVER THE SCENE JUMPS RATHER THAN
    // MOVES -- a camera teleport, a rebuilt model, a resize. Motion vectors
    // describe a CONTINUOUS change, and reprojecting across a discontinuity
    // pulls occlusion from wherever the old pixels happened to be.
    void ResetHistory ();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
