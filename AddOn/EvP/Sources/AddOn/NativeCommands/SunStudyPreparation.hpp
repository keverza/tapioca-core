#ifndef EVP_NATIVECOMMANDS_SUNSTUDYPREPARATION_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYPREPARATION_HPP

#include "SunStudy/SunStudyStore.hpp"
#include <chrono>

namespace geomsrv {

bool SunStudySnapshotBounds (const Snapshot& snapshot, double min[3], double max[3]);
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
