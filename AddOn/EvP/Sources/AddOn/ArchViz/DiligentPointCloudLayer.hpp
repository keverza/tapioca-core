#ifndef EVP_ARCHVIZ_DILIGENTPOINTCLOUDLAYER_HPP
#define EVP_ARCHVIZ_DILIGENTPOINTCLOUDLAYER_HPP

#include "ArchViz/SceneCmdQueue.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv {
namespace archviz {

struct DiligentPointCloudStats {
    size_t layers = 0;
    size_t nodes = 0;
    size_t points = 0;
    size_t visiblePoints = 0;
    size_t gpuBytes = 0;
    size_t drawCalls = 0;
};

// Render-thread-owned point-cloud resources. Architectural batches never enter
// this class, so replacing the model cannot accidentally clear survey layers.
class DiligentPointCloudLayer final {
  public:
    DiligentPointCloudLayer ();
    ~DiligentPointCloudLayer ();
    DiligentPointCloudLayer (const DiligentPointCloudLayer&) = delete;
    DiligentPointCloudLayer& operator= (const DiligentPointCloudLayer&) = delete;

    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
               std::string& error);
    void Shutdown ();

    void BeginLayer (const PointLayerUpload& upload);
    void ClearLayer (const std::string& layerId);
    bool UpsertNode (Diligent::IRenderDevice* device, const PointNodeUpload& upload);
    void EndLayer (const std::string& layerId);

    // The caller has already bound the current scene RTV and the main DSV.
    // `viewProj` is the visible camera; projection scale and `eye` are stable
    // inputs used by LOD selection rather than the jittered matrix.
    size_t Draw (Diligent::IDeviceContext* context, const float viewProj[16], const float projection[16],
                 const float eye[3], uint32_t viewportWidth, uint32_t viewportHeight, bool hdr, uint32_t frameIndex);

    DiligentPointCloudStats Stats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archviz
} // namespace geomsrv

#endif
