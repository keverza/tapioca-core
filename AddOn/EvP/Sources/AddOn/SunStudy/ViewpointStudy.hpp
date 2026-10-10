#ifndef EVP_SUNSTUDY_VIEWPOINTSTUDY_HPP
#define EVP_SUNSTUDY_VIEWPOINTSTUDY_HPP

#include "Geometry/Mesh.hpp"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::sunstudy {

struct ViewpointOptions {
    std::array<double, 3> point { 0.0, 0.0, 1.6 };
    double radius = 20.0; // metres, horizontal XY section at point.z
    size_t rays = 720;
};

struct ViewpointStudyResult {
    bool valid = false;
    std::string error;
    uint64_t snapshotId = 0, captureStamp = 0;
    ViewpointOptions options;
    // Counterclockwise, without duplicate closing vertex. Fan triangulation
    // about options.point preserves concave isovists without a convex hull.
    std::vector<std::array<double, 3>> perimeter;
    size_t collisionCount = 0;
    double area = 0.0;
};

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled = {});

} // namespace evp::sunstudy
#endif
