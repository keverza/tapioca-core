#ifndef EVP_ARCHVIZ_SUNSTUDYGPUTRAVERSAL_HPP
#define EVP_ARCHVIZ_SUNSTUDYGPUTRAVERSAL_HPP

#include "SunStudy/SunStudyOccluders.hpp"

#include <memory>
#include <string>

namespace geomsrv::archviz {

struct SunStudyGpuStats {
    bool attempted = false;
    bool available = false;
    bool deviceReused = false;
    bool contextReused = false;
    size_t sceneUploadedBytes = 0;
    size_t contextUploadedBytes = 0;
    uint64_t dispatches = 0;
    uint64_t gpuRays = 0;
    uint64_t cpuFallbackRays = 0;
    uint64_t validationRays = 0;
    double submitMilliseconds = 0.0;
    double readbackMilliseconds = 0.0; // includes GPU queue/execution waiting, not a GPU timestamp
    double cpuCheckMilliseconds = 0.0;
    double pollSleepMilliseconds = 0.0; // overlaps result waiting
    double computeMilliseconds = 0.0;   // valid GPU timestamp pairs only
    uint64_t timedDispatches = 0;
    std::string adapter;
    std::string error;
};

// Independent hardware-only D3D11 compute context, NOT the renderer's context.
// Lazily initialised by Advance on its calculation worker. FP64 watertight
// any-hit traversal exports the CPU BVH without changing the study's samples.
// Uncertain boundary rays, work-limit exhaustion and failed parity checks fall
// back to the original CPU tracer. General non-directional queries stay CPU.
class SunStudyGpuTraversal final : public evp::sunstudy::ITraversal {
  public:
    explicit SunStudyGpuTraversal (std::shared_ptr<const QueryEngine> engine,
                                   std::shared_ptr<const QueryEngine> context = nullptr,
                                   const SunStudyGpuTraversal* previous = nullptr);
    ~SunStudyGpuTraversal () override;
    void OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin, double tmax,
                             uint8_t* out, size_t maxParallel = 0) const override;
    bool OccludeDirectionalCancellable (const double* origins, size_t count, const double dir[3], double tmin,
                                        double tmax, uint8_t* out, size_t maxParallel,
                                        const std::function<bool ()>& isCancelled) const override;
    void OccludeRays (const evp::sunstudy::OcclusionRay* rays, size_t count, uint8_t* out,
                      size_t maxParallel = 0) const override;
    uint64_t SceneVersion () const override;
    SunStudyGpuStats Stats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geomsrv::archviz

#endif
