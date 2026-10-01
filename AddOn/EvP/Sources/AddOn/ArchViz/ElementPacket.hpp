#ifndef EVP_ARCHVIZ_ELEMENTPACKET_HPP
#define EVP_ARCHVIZ_ELEMENTPACKET_HPP

#include "Geometry/Mesh.hpp"
#include "ArchViz/SceneCmdQueue.hpp"

namespace geomsrv::archviz {

struct CapturedMeshPacket {
    Mesh mesh;
    std::chrono::steady_clock::time_point capturedAt;
    double captureMilliseconds = 0.0;
};

// SDK-free conversion on the extraction worker, not the host or render thread.
std::unique_ptr<ElementUpload> MakeElementPacket (const CapturedMeshPacket& packet);

} // namespace geomsrv::archviz
#endif
