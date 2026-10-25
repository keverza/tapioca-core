#include "SunStudy/ViewpointStudy.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustViews.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    result.rayMesh.reserve (options.rays);
    for (size_t ray = 0; ray < options.rays; ++ray) {
        if (cancelled ()) {
            result.perimeter.clear ();
            result.rayMesh.clear ();
            result.error = "viewpoint calculation cancelled";
            return result;
        }
        const double angle = 6.28318530717958647692 * double (ray) / double (options.rays);
        const double direction[3] = { std::cos (angle), std::sin (angle), 0.0 };
        double distance = options.radius;
        int64_t blocker = -1;
        if (engine != nullptr) {
            const auto hit = engine->RaycastMasked (options.point.data (), direction, options.radius, mask);
            if (hit.hit) {
                distance = (std::max) (0.0, (std::min) (distance, hit.t));
                ++result.collisionCount;
                blocker = static_cast<int64_t> (hit.meshIndex);
            }
        }
        result.rayMesh.push_back (blocker);
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

std::vector<ViewpointBlocker> ViewpointBlockers (const ViewpointStudyResult& result, const geomsrv::Snapshot& snapshot)
{
    std::vector<ViewpointBlocker> blockers;
    if (!result.valid || result.snapshotId != snapshot.id || result.rayMesh.size () != result.perimeter.size ())
        return blockers;
    std::vector<size_t> slot (snapshot.meshes.size (), SIZE_MAX);
    for (size_t ray = 0; ray < result.rayMesh.size (); ++ray) {
        const int64_t mesh = result.rayMesh[ray];
        if (mesh < 0 || static_cast<size_t> (mesh) >= snapshot.meshes.size ())
            continue;
        const auto& end = result.perimeter[ray];
        const double distance = std::hypot (end[0] - result.options.point[0], end[1] - result.options.point[1]);
        size_t& index = slot[static_cast<size_t> (mesh)];
        if (index == SIZE_MAX) {
            index = blockers.size ();
            blockers.push_back (
                { static_cast<size_t> (mesh), snapshot.meshes[static_cast<size_t> (mesh)].guid, 0, distance });
        }
        ViewpointBlocker& blocker = blockers[index];
        ++blocker.rays;
        blocker.nearest = (std::min) (blocker.nearest, distance);
    }
    std::stable_sort (blockers.begin (), blockers.end (), [] (const ViewpointBlocker& a, const ViewpointBlocker& b) {
        if (a.rays != b.rays)
            return a.rays > b.rays;
        if (a.nearest != b.nearest)
            return a.nearest < b.nearest;
        return a.mesh < b.mesh;
    });
    return blockers;
}

std::vector<ViewpointArc> ViewpointArcs (const std::vector<int64_t>& rayMesh)
{
    std::vector<ViewpointArc> arcs;
    const size_t count = rayMesh.size ();
    if (count == 0)
        return arcs;
    // Start at a ray whose predecessor differs, so a run crossing ray 0 stays whole.
    size_t start = 0;
    while (start < count && rayMesh[start] == rayMesh[(start + count - 1) % count])
        ++start;
    if (start == count)
        return { { 0, count, rayMesh[0] } }; // one outcome all the way round
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t ray = (start + offset) % count;
        if (arcs.empty () || arcs.back ().mesh != rayMesh[ray])
            arcs.push_back ({ ray, 0, rayMesh[ray] });
        ++arcs.back ().count;
    }
    return arcs;
}

bool ToggleViewpointContext (std::vector<std::string>& context, const std::string& guid)
{
    const std::string key = CanonicalGuid (guid);
    const auto kept = std::remove_if (context.begin (), context.end (),
                                      [&] (const std::string& entry) { return CanonicalGuid (entry) == key; });
    if (kept != context.end ()) {
        context.erase (kept, context.end ()); // every spelling of the same element
        return false;
    }
    context.push_back (guid);
    return true;
}

ViewpointStudyResult RunSelectedViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                                const std::vector<std::string>& context,
                                                const ViewpointOptions& options,
                                                const std::function<bool ()>& isCancelled)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (SelectedSunStudyCore () == SunStudyCoreKind::Rust)
        return rust::RunViewpointStudy (snapshot, context, options, isCancelled);
#endif
    return RunViewpointStudy (snapshot, context, options, isCancelled);
}

} // namespace evp::sunstudy
