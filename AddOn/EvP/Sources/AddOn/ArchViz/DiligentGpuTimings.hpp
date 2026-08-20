#ifndef EVP_ARCHVIZ_DILIGENTGPUTIMINGS_HPP
#define EVP_ARCHVIZ_DILIGENTGPUTIMINGS_HPP

#include <cstdint>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv::archviz {

enum class GpuTimingStage : uint8_t {
    VisibilityGBuffer,
    Shading,
    Post,
};

// D3D11 timestamp results are available only after the frame is finished. The
// bounded ring keeps polling non-blocking and avoids allocating a query pair on
// every frame.
class DiligentGpuTimings final {
  public:
    DiligentGpuTimings ();
    ~DiligentGpuTimings ();
    DiligentGpuTimings (const DiligentGpuTimings&) = delete;
    DiligentGpuTimings& operator= (const DiligentGpuTimings&) = delete;

    // Opt in with TAPIOCA_ARCHVIZ_BENCHMARK=1 before starting Archicad.
    bool Initialize (Diligent::IRenderDevice* device);
    void Shutdown ();
    bool Enabled () const;

    void BeginFrame (uint64_t frame);
    void Begin (Diligent::IDeviceContext* context, GpuTimingStage stage);
    void End (Diligent::IDeviceContext* context, GpuTimingStage stage);
    void EndFrame (uint64_t frame, uint32_t sampleCount);
    void Collect ();

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace geomsrv::archviz

#endif
