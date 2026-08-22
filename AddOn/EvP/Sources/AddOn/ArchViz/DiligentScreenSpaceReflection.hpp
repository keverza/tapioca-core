#ifndef EVP_ARCHVIZ_DILIGENTSPACEREFLECTION_HPP
#define EVP_ARCHVIZ_DILIGENTSPACEREFLECTION_HPP

#include <cstdint>
#include <memory>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

// Private owner for DiligentFX's ScreenSpaceReflection post-process, modelled
// on DiligentAmbientOcclusion. Keeping the concrete DiligentFX structures here
// preserves DiligentScene.hpp's Diligent-free interface.
//
// ⚠️ THIS IS A PORT, NOT AN IMPLEMENTATION. DiligentFX already ships the effect
// (PostProcess/ScreenSpaceReflection) on the same PostFXContext the AO port
// already uses. This wrapper owns its OWN PostFXContext -- separate from the
// AO's -- because each effect manages its own frame history and the two cannot
// share temporal state without one clobbering the other's previous-frame data.
class DiligentScreenSpaceReflection final {
  public:
    DiligentScreenSpaceReflection ();
    ~DiligentScreenSpaceReflection ();
    DiligentScreenSpaceReflection (const DiligentScreenSpaceReflection&) = delete;
    DiligentScreenSpaceReflection& operator= (const DiligentScreenSpaceReflection&) = delete;

    void Init (Diligent::IRenderDevice* device);
    void Shutdown ();

    // ⚠️ FEATURE_FLAG_PREVIOUS_FRAME is the mode that uses the motion vectors
    // C2 landed and the previous frame's colour. Without it the effect is
    // spatial-only and produces noisier reflections.
    Diligent::ITextureView* Execute (
        Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
        Diligent::ITextureView* color, Diligent::ITextureView* depth,
        Diligent::ITextureView* normal, Diligent::ITextureView* material,
        Diligent::ITextureView* motion, uint32_t width, uint32_t height,
        uint32_t frameIndex, const float view[16], const float proj[16],
        const float viewProj[16], const float eye[3], float nearClip, float farClip,
        float focusDistance, float intensity, float roughnessThreshold);

    // Throw the history away on a discontinuity -- camera teleport, rebuilt
    // model, resize. Same contract as DiligentAmbientOcclusion::ResetHistory.
    void ResetHistory ();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
