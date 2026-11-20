#include "SunStudy/ViewpointStudy.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustViews.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <unordered_map>

namespace evp::sunstudy {

std::vector<std::array<double, 2>> ViewpointPlanOutline (const ViewpointStudyResult& result)
{
    std::vector<std::array<double, 2>> outline;
    if (!result.valid || result.options.mode == ViewpointMode::Sphere)
        return outline;
    const auto add = [&] (double x, double y) {
        if (!outline.empty () && std::hypot (x - outline.back ()[0], y - outline.back ()[1]) < 1.0e-3)
            return;
        outline.push_back ({ x, y });
    };
    if (!result.Closed ())
        add (result.options.point[0], result.options.point[1]);
    for (const auto& end : result.perimeter)
        add (end[0], end[1]);
    while (outline.size () > 1 &&
           std::hypot (outline.front ()[0] - outline.back ()[0], outline.front ()[1] - outline.back ()[1]) < 1.0e-3)
        outline.pop_back ();
    double twiceArea = 0.0;
    for (size_t i = 0; i < outline.size (); ++i) {
        const auto& a = outline[i];
        const auto& b = outline[(i + 1) % outline.size ()];
        twiceArea += (a[0] - outline[0][0]) * (b[1] - outline[0][1]) - (b[0] - outline[0][0]) * (a[1] - outline[0][1]);
    }
    if (outline.size () < 3 || std::abs (twiceArea) < 1.0e-9)
        outline.clear ();
    return outline;
}

ViewpointPlanContour ViewpointBakeContour (const ViewpointStudyResult& result, double straightMetres)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kLongestArc = kPi * 0.999; // a fill edge's arc may not exceed pi
    ViewpointPlanContour contour;
    auto ring = ViewpointPlanOutline (result);
    const size_t n = ring.size ();
    if (n < 3)
        return contour;
    const double cx = result.options.point[0], cy = result.options.point[1], radius = result.options.radius;
    const double onCircle = 1.0e-4 + radius * 1.0e-9;
    const auto circular = [&] (const std::array<double, 2>& p) {
        return std::abs (std::hypot (p[0] - cx, p[1] - cy) - radius) <= onCircle;
    };
    // Start where the ring must have a vertex: the point of a field of view, else
    // the first vertex off the circle after one on it, else (closed in by
    // Context) the sharpest corner -- so neither an arc nor a straight run wraps
    // index 0.
    size_t start = 0;
    bool wholeCircle = false;
    if (result.Closed ()) {
        size_t off = 0;
        while (off < n && circular (ring[off]))
            ++off;
        wholeCircle = off == n;
        start = n;
        for (size_t i = 0; i < n && start == n && !wholeCircle; ++i)
            if (!circular (ring[i]) && circular (ring[(i + n - 1) % n]))
                start = i;
        if (start == n) {
            start = 0;
            double sharpest = -1.0;
            for (size_t i = 0; i < n && !wholeCircle; ++i) {
                const auto& a = ring[(i + n - 1) % n];
                const auto& b = ring[(i + 1) % n];
                const double dx = b[0] - a[0], dy = b[1] - a[1], length = std::hypot (dx, dy);
                const double bend =
                    length < 1.0e-12 ? 0.0 : std::abs (dx * (ring[i][1] - a[1]) - dy * (ring[i][0] - a[0])) / length;
                if (bend > sharpest) {
                    sharpest = bend;
                    start = i;
                }
            }
        }
    }
    if (!wholeCircle)
        std::rotate (ring.begin (), ring.begin () + static_cast<std::ptrdiff_t> (start), ring.end ());
    std::vector<uint8_t> onRadius (n);
    for (size_t i = 0; i < n; ++i)
        onRadius[i] = circular (ring[i]) ? 1 : 0;
    // The signed angle about the point from ring[i] to ring[j].
    const auto sweep = [&] (size_t i, size_t j) {
        const double ax = ring[i][0] - cx, ay = ring[i][1] - cy, bx = ring[j][0] - cx, by = ring[j][1] - cy;
        return std::atan2 (ax * by - ay * bx, ax * bx + ay * by);
    };
    // Edge i -> i + 1 is an arc when both its ends reached the radius; `kept`
    // marks the vertices that stay, `angle` the arc leaving each kept vertex.
    std::vector<uint8_t> kept (n, 0);
    std::vector<double> angle (n, 0.0);
    const auto arcEdge = [&] (size_t i) { return onRadius[i] && onRadius[(i + 1) % n]; };
    for (size_t i = 0; i < n;) {
        if (!arcEdge (i)) {
            ++i;
            continue;
        }
        // A run of arc edges from i; split wherever it would turn past kLongestArc.
        size_t from = i;
        double turned = 0.0;
        kept[from] = 1;
        size_t j = i;
        do {
            const size_t next = (j + 1) % n;
            const double step = sweep (j, next);
            if (std::abs (turned + step) > kLongestArc) {
                angle[from] = turned;
                from = j;
                kept[from] = 1;
                turned = 0.0;
            }
            turned += step;
            j = next;
        } while (j != 0 && arcEdge (j));
        angle[from] = turned;
        kept[j] = 1;
        if (j == 0)
            break;
        i = j;
    }
    // A vertex not on an arc stays when it ends a run of ray ends along one line:
    // walk from each kept vertex as far as every vertex passed lies within
    // straightMetres of the chord.
    const auto offChord = [&] (size_t a, size_t b, size_t k) {
        const double dx = ring[b][0] - ring[a][0], dy = ring[b][1] - ring[a][1];
        const double length = std::hypot (dx, dy);
        const double px = ring[k][0] - ring[a][0], py = ring[k][1] - ring[a][1];
        if (length < 1.0e-12)
            return std::hypot (px, py);
        return std::abs (dx * py - dy * px) / length;
    };
    kept[0] = 1; // off the circle (the point, for a field of view), or the whole circle's start
    for (size_t anchor = 0; !wholeCircle && anchor < n;) {
        if (arcEdge (anchor)) { // skip to where this arc ends
            do
                ++anchor;
            while (anchor < n && !kept[anchor]);
            continue;
        }
        size_t end = anchor + 1; // n stands for vertex 0, closing the ring
        for (size_t candidate = anchor + 2; candidate <= n && !kept[candidate - 1]; ++candidate) {
            bool straight = true;
            for (size_t k = anchor + 1; k < candidate && straight; ++k)
                straight = offChord (anchor, candidate % n, k) <= straightMetres;
            if (!straight)
                break;
            end = candidate;
        }
        if (end < n)
            kept[end] = 1;
        anchor = end;
    }
    for (size_t i = 0; i < n; ++i)
        if (kept[i]) {
            contour.points.push_back (ring[i]);
            contour.arcAngles.push_back (angle[i]);
        }
    return contour;
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
        const double distance = std::hypot (end[0] - result.options.point[0], end[1] - result.options.point[1],
                                            end[2] - result.options.point[2]);
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
    return rust::RunViewpointStudy (snapshot, context, options, isCancelled);
#else
    (void) snapshot, (void) context, (void) isCancelled;
    ViewpointStudyResult refused;
    refused.options = options;
    refused.error = "this build has no sun-study core";
    return refused;
#endif
}

} // namespace evp::sunstudy
