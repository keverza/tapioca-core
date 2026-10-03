#ifndef EVP_ARCHVIZ_SUNSTUDYDISPLAYASSEMBLER_HPP
#define EVP_ARCHVIZ_SUNSTUDYDISPLAYASSEMBLER_HPP

#include "ArchViz/SunStudyOverlay.hpp"
#include "Geometry/Mesh.hpp"

#include <functional>
#include <mutex>

namespace evp::sunstudy {
struct StudyRecord;
struct StudyDisplayData;
} // namespace evp::sunstudy

namespace geomsrv::archviz {

struct SunStudyDisplayOptions {
    double hoursMax = 0.0;
    uint32_t debug = 0, depth = 0;
    bool preview = false;
};

// Pure worker-side producer: no SDK, gate rendezvous, or GPU allocation. Weak
// histories reuse immutable maps/images without retaining orphaned studies.
class SunStudyDisplayAssembler {
  public:
    std::unique_ptr<SunStudyAtlasUpload> Prepare (const evp::sunstudy::StudyRecord& record, const Snapshot& snapshot,
                                                  const SunStudyDisplayOptions& options, std::string& error,
                                                  const std::function<bool ()>& isCancelled = {});

  private:
    std::mutex mutex_;
    std::weak_ptr<const evp::sunstudy::StudyDisplayData> source_;
    std::weak_ptr<const std::vector<SunStudyElementMap>> maps_[2];
    std::weak_ptr<const std::vector<float>> previousImage_;
    std::weak_ptr<const std::vector<uint32_t>> previousSteps_;
    uint32_t width_ = 0, height_ = 0, words_ = 0;
};

} // namespace geomsrv::archviz
#endif
