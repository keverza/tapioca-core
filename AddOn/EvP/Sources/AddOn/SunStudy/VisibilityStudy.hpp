#ifndef EVP_SUNSTUDY_VISIBILITYSTUDY_HPP
#define EVP_SUNSTUDY_VISIBILITYSTUDY_HPP

// Geometric visibility over the SunStudy square-cell sampling domain. This is
// deliberately a policy over the existing snapshot BVH and atlas, not another
// scene index or renderer-specific analysis.

#include "Geometry/Mesh.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudySampler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::sunstudy {

enum class VisibilityOrigin : uint8_t { Surfaces = 0, Point = 1 };

struct VisibilityStudyOptions {
    VisibilityOrigin origin = VisibilityOrigin::Surfaces;
    double spacing = 2.0;
    double normalOffset = 0.05;
    double tmin = 0.001;
    size_t maxSamples = 4000000;
    size_t maxAimPoints = 32;
    size_t maxRays = 16000000;
    size_t maxParallel = 0;
    double point[3] = { 0.0, 0.0, 0.0 };
    double direction[3] = { 1.0, 0.0, 0.0 };
    double coneDegrees = 90.0;
};

struct VisibilityStudyResult {
    bool valid = false;
    std::string error;
    std::string id;
    std::shared_ptr<const geomsrv::Snapshot> snapshot;
    uint64_t snapshotId = 0;
    VisibilityOrigin origin = VisibilityOrigin::Surfaces;
    double spacing = 0.0;
    std::vector<std::string> fromElements;
    std::vector<std::string> toElements;
    // The surfaces carrying `values`: FROM in surface mode, TO in point mode.
    std::vector<std::string> displayElements;
    SampleGrid grid;
    SunStudyAtlas atlas;
    std::vector<double> values;
    size_t aimPointCount = 0;
    size_t rayCount = 0;
    size_t visibleSamples = 0;
    double meanVisibility = 0.0;
    double analysedArea = 0.0;
    double analysisMilliseconds = 0.0;
};

VisibilityStudyResult RunVisibilityStudy (std::shared_ptr<const geomsrv::Snapshot> snapshot,
                                          const std::vector<std::string>& fromElements,
                                          const std::vector<std::string>& toElements,
                                          const VisibilityStudyOptions& options = VisibilityStudyOptions (),
                                          const std::function<bool ()>& isCancelled = {});

} // namespace evp::sunstudy

#endif
