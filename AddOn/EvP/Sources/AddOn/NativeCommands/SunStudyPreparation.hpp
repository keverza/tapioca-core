#ifndef EVP_NATIVECOMMANDS_SUNSTUDYPREPARATION_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYPREPARATION_HPP

#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include <chrono>

namespace geomsrv {

bool SunStudySnapshotBounds (const Snapshot& snapshot, double min[3], double max[3]);
void LogSunStudySurfaceSampling (uint64_t snapshot, const evp::sunstudy::SurfaceSamplingResult& result);
void LogSunStudyCylinderMembership (uint64_t snapshot, size_t analysis, const std::string& entered,
                                    const std::string& left);
std::shared_ptr<const evp::sunstudy::SunStudyOccluders>
PrepareSunStudyOccluders (std::shared_ptr<const Snapshot> snapshot, const evp::sunstudy::ElementRoles& roles,
                          const evp::sunstudy::StudyRecord* previous, const std::function<bool ()>& isCancelled);
// Attach the Rust core when TAPIOCA_SUNSTUDY_CORE=rust selects it (P5), then
// sync whichever core runs the study. A refused core is logged and the study
// runs on the C++ core.
void StartSunStudyCore (evp::sunstudy::StudyRecord& record, const Snapshot& snapshot,
                        const evp::sunstudy::StudyRecord* reuseSource, const evp::sunstudy::StudyInputs& inputs);
void FinishSunStudyPreparation (evp::sunstudy::StudyRecord& record, std::shared_ptr<const Snapshot> snapshot,
                                const evp::sunstudy::StudyRecord* reuseSource, const std::atomic<bool>* cancelled,
                                std::shared_ptr<const QueryEngine> occluders);

// Adjacent phase durations, not cumulative timings; payload bytes are used
// array bytes, not allocator capacity or GPU-completion time.
class SunStudyPreparationTrace {
  public:
    explicit SunStudyPreparationTrace (uint64_t snapshot);
    void Mark (const char* stage, size_t payloadBytes = 0);

  private:
    uint64_t snapshot_;
    std::chrono::steady_clock::time_point last_;
};

} // namespace geomsrv
#endif
