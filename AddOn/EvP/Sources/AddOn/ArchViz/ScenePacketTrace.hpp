#ifndef EVP_ARCHVIZ_SCENEPACKETTRACE_HPP
#define EVP_ARCHVIZ_SCENEPACKETTRACE_HPP

#include "ArchViz/SceneCmdQueue.hpp"
#include "Geometry/Mesh.hpp"

namespace geomsrv::archviz {

// Measures CPU/API submission time only. No fence, readback, or GPU stall is
// introduced to obtain a timing; wallMs must not be called GPU execution time.
class ScenePacketTrace {
  public:
    explicit ScenePacketTrace (const SceneCmd& command);
    ~ScenePacketTrace ();
    void CpuPrepared ();
    void Applied (size_t gpuBytes = 0);

  private:
    std::chrono::steady_clock::time_point started_, prepared_;
    std::string fields_;
    double queueWaitMs_ = 0.0, captureAgeMs_ = 0.0;
    size_t gpuBytes_ = 0;
    bool applied_ = false;
};

void LogSnapshotCapture (const Snapshot& snapshot, double wallMs);
void LogSunStudyDisplay (const SunStudyAtlasUpload& study, uint64_t snapshot, double wallMs);
size_t SunStudyPayloadBytes (const SunStudyAtlasUpload& study);

} // namespace geomsrv::archviz
#endif
