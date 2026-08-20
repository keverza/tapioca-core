#ifndef EVP_ARCHVIZ_DILIGENTDEPTHRANGE_HPP
#define EVP_ARCHVIZ_DILIGENTDEPTHRANGE_HPP

// ArchViz/DiligentDepthRange — the nearest and farthest depth actually present
// in this frame, reduced on the GPU into two uints the depth debug view then
// normalises against.
//
// ⚠️ IT EXISTS BECAUSE A FIXED RAMP CANNOT WORK HERE. The frustum is
// 0.05 m to 20 km (Camera::NearClip/FarClip), and a building occupies a sliver
// of it: any mapping anchored to the frustum paints the whole model one shade
// of grey, and any mapping anchored to the camera's orbit distance goes black
// as soon as the model sits outside that band. Both were tried and both were
// reported as a broken depth buffer. Normalising against the depths that are
// really on screen is the only version that cannot be defeated by where the
// camera happens to be.
//
// The result never leaves the GPU: the reduction writes a two-element buffer
// and the debug pixel shader reads it in the same frame. No readback, so no
// stall and no frame of latency.

#include <cstdint>
#include <memory>
#include <string>

namespace Diligent {
struct IBufferView;
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

class DiligentDepthRange final {
  public:
    DiligentDepthRange ();
    ~DiligentDepthRange ();
    DiligentDepthRange (const DiligentDepthRange&) = delete;
    DiligentDepthRange& operator= (const DiligentDepthRange&) = delete;

    // Builds the compute pipeline and the two-element buffer. Reports why it
    // could not rather than leaving a silently dead reduction behind.
    bool Init (Diligent::IRenderDevice* device, std::string& error);
    void Shutdown ();

    // The SRV the debug pixel shader reads: element 0 = nearest depth, element
    // 1 = farthest, both as raw float bits. ⚠️ VALID FROM Init ONWARDS, even
    // with no reduction run — it is seeded EMPTY (min > max), which is the
    // signal the shader falls back to the frustum on. A dynamic shader
    // variable has to be bound on every view, not only the one that dispatches.
    Diligent::IBufferView* BufferView () const;

    // One dispatch over the G-buffer's depth, reducing per thread group and
    // then once per group into the buffer.
    void Execute (Diligent::IDeviceContext* context, Diligent::ITextureView* depth, uint32_t width, uint32_t height);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
