#ifndef EVP_SUNSTUDY_VIEWPOINTSTUDY_HPP
#define EVP_SUNSTUDY_VIEWPOINTSTUDY_HPP

#include "Geometry/Mesh.hpp"
#include <array>
#include <cstdint>
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
    // Per ray, aligned with `perimeter`: the snapshot mesh index of the Context
    // element that stopped it, or -1 when it reached the radius.
    std::vector<int64_t> rayMesh;
    size_t collisionCount = 0;
    double area = 0.0;
};

// One Context element that stops viewpoint rays.
struct ViewpointBlocker {
    size_t mesh = 0;      // snapshot mesh index
    std::string guid;     // its element GUID as captured
    size_t rays = 0;      // rays it stopped
    double nearest = 0.0; // horizontal distance of its nearest stop, metres
};

// The elements that stopped rays, most rays first (ties: nearer, then mesh
// order). Empty when nothing was hit or `result` is not valid for `snapshot`.
std::vector<ViewpointBlocker> ViewpointBlockers (const ViewpointStudyResult& result, const geomsrv::Snapshot& snapshot);

// A run of consecutive rays with the same outcome: stopped by `mesh`, or open
// (`mesh == -1`). Runs wrap: a run crossing ray 0 is reported once, starting
// at its first ray, so `first + count` may exceed the ray count.
struct ViewpointArc {
    size_t first = 0;
    size_t count = 0;
    int64_t mesh = -1;
};
std::vector<ViewpointArc> ViewpointArcs (const std::vector<int64_t>& rayMesh);

// Add `guid` to `context`, or remove it when present (GUIDs compared in
// canonical form; order otherwise kept). Returns true when it was added.
bool ToggleViewpointContext (std::vector<std::string>& context, const std::string& guid);

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled = {});

// RunViewpointStudy on the core TAPIOCA_SUNSTUDY_CORE selects (P5); the
// Rust policy returns the same result through the same C++ query index.
ViewpointStudyResult RunSelectedViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                                const std::vector<std::string>& context,
                                                const ViewpointOptions& options,
                                                const std::function<bool ()>& isCancelled = {});

} // namespace evp::sunstudy
#endif
