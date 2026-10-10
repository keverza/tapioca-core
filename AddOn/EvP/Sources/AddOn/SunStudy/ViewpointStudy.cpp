#include "SunStudy/ViewpointStudy.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace evp::sunstudy {

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled)
{
    ViewpointStudyResult result;
    result.options = options;
    const auto cancelled = [&] { return isCancelled && isCancelled (); };
    if (snapshot == nullptr || !std::isfinite (options.radius) || options.radius <= 0.0 || options.rays < 32 ||
        options.rays > 1440 || !std::all_of (options.point.begin (), options.point.end (), [] (double value) {
            return std::isfinite (value);
        })) {
        result.error = "viewpoint needs a snapshot, finite point, positive radius and 32..1440 rays";
        return result;
    }
    result.snapshotId = snapshot->id;
    result.captureStamp = snapshot->captureStamp;
    std::set<std::string> selected;
    for (const auto& guid : context)
        selected.insert (CanonicalGuid (guid));
    auto missing = selected;
    std::vector<uint8_t> mask (snapshot->meshes.size (), 0);
    for (size_t mesh = 0; mesh < snapshot->meshes.size (); ++mesh) {
        const auto guid = CanonicalGuid (snapshot->meshes[mesh].guid);
        mask[mesh] = selected.find (guid) != selected.end ();
        missing.erase (guid);
    }
    if (!missing.empty ()) {
        result.error = "viewpoint Context contains objects absent from the capture";
        return result;
    }
    const auto engine = context.empty () ? nullptr : geomsrv::QueryIndexCache::Get ().For (snapshot);
    if (!context.empty () && engine == nullptr) {
        result.error = "viewpoint could not acquire the shared snapshot query index";
        return result;
    }
    result.perimeter.reserve (options.rays);
    for (size_t ray = 0; ray < options.rays; ++ray) {
        if (cancelled ()) {
            result.perimeter.clear ();
            result.error = "viewpoint calculation cancelled";
            return result;
        }
        const double angle = 6.28318530717958647692 * double (ray) / double (options.rays);
        const double direction[3] = { std::cos (angle), std::sin (angle), 0.0 };
        double distance = options.radius;
        if (engine != nullptr) {
            const auto hit = engine->RaycastMasked (options.point.data (), direction, options.radius, mask);
            if (hit.hit) {
                distance = (std::max) (0.0, (std::min) (distance, hit.t));
                ++result.collisionCount;
            }
        }
        result.perimeter.push_back ({ options.point[0] + distance * direction[0],
                                      options.point[1] + distance * direction[1], options.point[2] });
    }
    for (size_t i = 0; i < result.perimeter.size (); ++i) {
        const auto& a = result.perimeter[i];
        const auto& b = result.perimeter[(i + 1) % result.perimeter.size ()];
        result.area += ((a[0] - options.point[0]) * (b[1] - options.point[1]) -
                        (a[1] - options.point[1]) * (b[0] - options.point[0])) *
                       0.5;
    }
    result.valid = true;
    return result;
}

} // namespace evp::sunstudy
