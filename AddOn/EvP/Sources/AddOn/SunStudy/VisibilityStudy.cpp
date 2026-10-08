#include "SunStudy/VisibilityStudy.hpp"

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/CpuTraversalWork.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>

namespace evp::sunstudy {

namespace {

constexpr size_t kRayBatch = 65536;

struct AimPoint {
    double p[3] = { 0.0, 0.0, 0.0 };
    size_t mesh = 0;
};

double DistanceSquared (const double a[3], const double b[3])
{
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

bool Normalize (double v[3])
{
    const double length = std::sqrt (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1.0e-12) || !std::isfinite (length))
        return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

std::set<std::string> CanonicalSet (const std::vector<std::string>& guids)
{
    std::set<std::string> result;
    for (const auto& guid : guids)
        if (!guid.empty ())
            result.insert (CanonicalGuid (guid));
    return result;
}

std::vector<uint8_t> MaskOf (const geomsrv::Snapshot& snapshot, const std::set<std::string>& selected, size_t& matches)
{
    std::vector<uint8_t> mask (snapshot.meshes.size (), 0);
    matches = 0;
    for (size_t i = 0; i < snapshot.meshes.size (); ++i) {
        if (selected.find (CanonicalGuid (snapshot.meshes[i].guid)) != selected.end ()) {
            mask[i] = 1;
            ++matches;
        }
    }
    return mask;
}

bool Cancelled (const std::function<bool ()>& isCancelled)
{
    return isCancelled && isCancelled ();
}

std::vector<AimPoint> TargetCandidates (const geomsrv::Snapshot& snapshot, const std::vector<uint8_t>& mask,
                                        const std::function<bool ()>& isCancelled)
{
    std::vector<AimPoint> candidates;
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        if (m >= mask.size () || mask[m] == 0)
            continue;
        const auto& mesh = snapshot.meshes[m];
        for (size_t face = 0; face + 2 < mesh.triangles.size (); face += 3) {
            if (face % 3072 == 0 && Cancelled (isCancelled))
                return {};
            const uint32_t ia = mesh.triangles[face], ib = mesh.triangles[face + 1], ic = mesh.triangles[face + 2];
            if (3 * static_cast<size_t> ((std::max) (ia, (std::max) (ib, ic))) + 2 >= mesh.vertices.size ())
                continue;
            AimPoint point;
            point.mesh = m;
            for (int axis = 0; axis < 3; ++axis)
                point.p[axis] =
                    (mesh.vertices[3 * ia + axis] + mesh.vertices[3 * ib + axis] + mesh.vertices[3 * ic + axis]) / 3.0;
            candidates.push_back (point);
        }
    }
    return candidates;
}

// Deterministic farthest-point reduction, seeded once per target element so a
// small object cannot disappear merely because a large neighbour owns more
// triangles.
std::vector<AimPoint> ReduceAimPoints (const std::vector<AimPoint>& candidates, size_t limit,
                                       const std::function<bool ()>& isCancelled)
{
    if (candidates.size () <= limit)
        return candidates;
    std::vector<size_t> chosen;
    std::set<size_t> represented;
    for (size_t i = 0; i < candidates.size () && chosen.size () < limit; ++i) {
        if (i % 1024 == 0 && Cancelled (isCancelled))
            return {};
        if (represented.insert (candidates[i].mesh).second)
            chosen.push_back (i);
    }
    std::vector<double> nearest (candidates.size (), std::numeric_limits<double>::infinity ());
    const auto include = [&] (size_t index) {
        for (size_t i = 0; i < candidates.size (); ++i) {
            if (i % 1024 == 0 && Cancelled (isCancelled))
                return false;
            nearest[i] = (std::min) (nearest[i], DistanceSquared (candidates[i].p, candidates[index].p));
        }
        return true;
    };
    for (const size_t index : chosen) {
        if (!include (index))
            return {};
        nearest[index] = -1.0;
    }
    while (chosen.size () < limit) {
        size_t best = 0;
        for (size_t i = 1; i < nearest.size (); ++i)
            if (nearest[i] > nearest[best])
                best = i;
        chosen.push_back (best);
        nearest[best] = -1.0;
        if (!include (best))
            return {};
    }
    std::vector<AimPoint> result;
    result.reserve (chosen.size ());
    for (const size_t index : chosen)
        result.push_back (candidates[index]);
    return result;
}

void ClassifyFirstHitTargets (const geomsrv::QueryEngine& engine, const std::vector<OcclusionRay>& rays,
                              const std::vector<uint8_t>& targetMask, std::vector<uint8_t>& visible, size_t maxParallel,
                              const std::function<bool ()>& isCancelled)
{
    visible.assign (rays.size (), 0);
    const auto body = [&] (size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            // Raycast has no tmin argument. Move its origin along the unit ray
            // so the first-hit query obeys the same lower bound as occlusion.
            double origin[3];
            for (int axis = 0; axis < 3; ++axis)
                origin[axis] = rays[i].origin[axis] + rays[i].dir[axis] * rays[i].tmin;
            const auto hit = engine.Raycast (origin, rays[i].dir, rays[i].tmax - rays[i].tmin);
            visible[i] = hit.hit && hit.meshIndex < targetMask.size () && targetMask[hit.meshIndex] != 0 ? 1 : 0;
        }
    };
    RunCpuTraversal (rays.size (), maxParallel, isCancelled, body);
}

} // namespace

VisibilityStudyResult RunVisibilityStudy (std::shared_ptr<const geomsrv::Snapshot> snapshot,
                                          const std::vector<std::string>& fromElements,
                                          const std::vector<std::string>& toElements,
                                          const VisibilityStudyOptions& options,
                                          const std::function<bool ()>& isCancelled)
{
    VisibilityStudyResult result;
    result.snapshot = std::move (snapshot);
    result.fromElements = fromElements;
    result.toElements = toElements;
    result.origin = options.origin;
    result.spacing = options.spacing;
    if (result.snapshot == nullptr || result.snapshot->meshes.empty ()) {
        result.error = "visibility study needs a live non-empty snapshot";
        return result;
    }
    result.snapshotId = result.snapshot->id;
    if (!(options.spacing > 0.0) || !std::isfinite (options.spacing)) {
        result.error = "visibility grid spacing must be positive";
        return result;
    }
    if (!std::isfinite (options.normalOffset) || options.normalOffset < 0.0 || !std::isfinite (options.tmin) ||
        options.tmin < 0.0 || options.maxAimPoints == 0 || options.maxSamples == 0 || options.maxRays == 0 ||
        (options.origin != VisibilityOrigin::Surfaces && options.origin != VisibilityOrigin::Point)) {
        result.error = "visibility options contain invalid offsets, limits or origin mode";
        return result;
    }
    if (options.origin == VisibilityOrigin::Point) {
        double view[3] = { options.direction[0], options.direction[1], options.direction[2] };
        if (!std::isfinite (options.point[0]) || !std::isfinite (options.point[1]) ||
            !std::isfinite (options.point[2]) || !Normalize (view) || !std::isfinite (options.coneDegrees) ||
            options.coneDegrees < 1.0 || options.coneDegrees > 179.0) {
            result.error = "visibility point needs finite coordinates, a non-zero direction and a 1..179 degree cone";
            return result;
        }
    }
    const auto from = CanonicalSet (fromElements), to = CanonicalSet (toElements);
    if (to.empty ()) {
        result.error = "visibility study needs at least one TO element";
        return result;
    }
    if (options.origin == VisibilityOrigin::Surfaces && from.empty ()) {
        result.error = "surface visibility needs at least one FROM element";
        return result;
    }
    for (const auto& guid : from) {
        if (to.find (guid) != to.end ()) {
            result.error = "FROM and TO element sets must not overlap";
            return result;
        }
    }

    size_t fromMatches = 0, toMatches = 0;
    const auto fromMask = MaskOf (*result.snapshot, from, fromMatches);
    const auto toMask = MaskOf (*result.snapshot, to, toMatches);
    std::set<std::string> present;
    for (const auto& mesh : result.snapshot->meshes)
        present.insert (CanonicalGuid (mesh.guid));
    const auto missing = [&present] (const std::set<std::string>& role) {
        return std::any_of (role.begin (), role.end (),
                            [&present] (const std::string& guid) { return present.find (guid) == present.end (); });
    };
    if (toMatches == 0 || missing (to) ||
        (options.origin == VisibilityOrigin::Surfaces && (fromMatches == 0 || missing (from)))) {
        result.error = "visibility role elements are absent from the live snapshot";
        return result;
    }
    const auto& displayMask = options.origin == VisibilityOrigin::Point ? toMask : fromMask;
    result.displayElements = options.origin == VisibilityOrigin::Point ? toElements : fromElements;

    SurfaceSamplingOptions samplingOptions;
    samplingOptions.domain = SamplingDomain::TriangleLegacy;
    samplingOptions.spacing = options.spacing;
    samplingOptions.normalOffset = options.normalOffset;
    samplingOptions.maxSamples = options.maxSamples;
    auto sampling = BuildSurfaceSampling (*result.snapshot, displayMask, samplingOptions, nullptr, isCancelled);
    if (Cancelled (isCancelled)) {
        result.error = "visibility study cancelled";
        return result;
    }
    if (!sampling.valid || !sampling.triangles.valid || sampling.triangles.Count () == 0) {
        result.error = "visibility square-cell sampling was refused or produced no cells";
        return result;
    }
    result.grid = std::move (sampling.triangles);
    if (result.grid.Count () > options.maxRays) {
        result.error = "visibility ray budget cannot represent every square cell at least once";
        return result;
    }
    result.atlas = BuildSunStudyAtlas (result.grid);
    if (!result.atlas.valid) {
        result.error = "visibility square-cell atlas could not be packed";
        return result;
    }
    result.values.assign (result.grid.Count (), 0.0);
    for (const double area : result.grid.areas)
        result.analysedArea += area;

    const auto engine = geomsrv::QueryIndexCache::Get ().For (result.snapshot);
    if (engine == nullptr) {
        result.error = "visibility study could not acquire the snapshot query index";
        return result;
    }
    CpuTraversal traversal (engine);
    const auto started = std::chrono::steady_clock::now ();

    if (options.origin == VisibilityOrigin::Point) {
        double view[3] = { options.direction[0], options.direction[1], options.direction[2] };
        if (!Normalize (view)) {
            result.error = "visibility point needs a non-zero view direction";
            return result;
        }
        const double cone = (std::max) (1.0, (std::min) (179.0, options.coneDegrees));
        const double cosHalf = std::cos (cone * 0.5 * 3.14159265358979323846 / 180.0);
        for (size_t first = 0; first < result.grid.Count (); first += kRayBatch) {
            if (Cancelled (isCancelled)) {
                result.error = "visibility study cancelled";
                return result;
            }
            const size_t count = (std::min) (kRayBatch, result.grid.Count () - first);
            std::vector<OcclusionRay> rays;
            std::vector<size_t> samples;
            rays.reserve (count);
            samples.reserve (count);
            for (size_t local = 0; local < count; ++local) {
                const size_t sample = first + local;
                double target[3], dir[3];
                for (int axis = 0; axis < 3; ++axis) {
                    target[axis] = result.grid.positions[3 * sample + axis] -
                                   result.grid.normals[3 * sample + axis] * options.normalOffset;
                    dir[axis] = target[axis] - options.point[axis];
                }
                const double distance = std::sqrt (DistanceSquared (target, options.point));
                if (!(distance > options.tmin) || !Normalize (dir) ||
                    dir[0] * view[0] + dir[1] * view[1] + dir[2] * view[2] < cosHalf)
                    continue;
                OcclusionRay ray;
                std::copy (options.point, options.point + 3, ray.origin);
                std::copy (dir, dir + 3, ray.dir);
                ray.tmin = options.tmin;
                ray.tmax = (std::max) (options.tmin, distance - (std::max) (options.tmin, 1.0e-6));
                rays.push_back (ray);
                samples.push_back (sample);
            }
            std::vector<uint8_t> blocked (rays.size (), 1);
            if (!rays.empty ())
                traversal.OccludeRays (rays.data (), rays.size (), blocked.data (), options.maxParallel);
            if (Cancelled (isCancelled)) {
                result.error = "visibility study cancelled";
                return result;
            }
            result.rayCount += rays.size ();
            for (size_t i = 0; i < rays.size (); ++i)
                result.values[samples[i]] = blocked[i] == 0 ? 1.0 : 0.0;
        }
        result.aimPointCount = result.grid.Count ();
    }
    else {
        auto candidates = TargetCandidates (*result.snapshot, toMask, isCancelled);
        if (Cancelled (isCancelled)) {
            result.error = "visibility study cancelled";
            return result;
        }
        if (candidates.empty ()) {
            result.error = "TO elements contain no valid target triangles";
            return result;
        }
        std::set<size_t> represented;
        for (const auto& candidate : candidates)
            represented.insert (candidate.mesh);
        if (represented.size () != toMatches) {
            result.error = "a TO element contains no valid target triangles";
            return result;
        }
        const size_t rayBudgetAim = options.maxRays / result.grid.Count ();
        // Every selected TO object owns at least one deterministic aim point.
        // Silently dropping small targets because a large neighbour exhausted
        // the cap would make set membership depend on tessellation density.
        const size_t requestedAim = (std::max) (options.maxAimPoints, toMatches);
        const size_t aimLimit = (std::min) (requestedAim, rayBudgetAim);
        if (aimLimit < toMatches) {
            result.error = "visibility ray budget cannot represent every TO element at least once";
            return result;
        }
        const auto aims = ReduceAimPoints (candidates, aimLimit, isCancelled);
        if (aims.empty () || Cancelled (isCancelled)) {
            result.error = "visibility study cancelled";
            return result;
        }
        result.aimPointCount = aims.size ();
        for (size_t first = 0; first < result.grid.Count ();
             first += (std::max) (size_t (1), kRayBatch / aims.size ())) {
            if (Cancelled (isCancelled)) {
                result.error = "visibility study cancelled";
                return result;
            }
            const size_t sampleCount =
                (std::min) ((std::max) (size_t (1), kRayBatch / aims.size ()), result.grid.Count () - first);
            std::vector<OcclusionRay> rays;
            std::vector<size_t> samples;
            rays.reserve (sampleCount * aims.size ());
            samples.reserve (sampleCount * aims.size ());
            for (size_t local = 0; local < sampleCount; ++local) {
                const size_t sample = first + local;
                const double* origin = result.grid.positions.data () + 3 * sample;
                const double* normal = result.grid.normals.data () + 3 * sample;
                for (const AimPoint& aim : aims) {
                    double dir[3] = { aim.p[0] - origin[0], aim.p[1] - origin[1], aim.p[2] - origin[2] };
                    const double distance = std::sqrt (DistanceSquared (aim.p, origin));
                    if (!(distance > options.tmin) || !Normalize (dir) ||
                        dir[0] * normal[0] + dir[1] * normal[1] + dir[2] * normal[2] <= 0.0)
                        continue;
                    OcclusionRay ray;
                    std::copy (origin, origin + 3, ray.origin);
                    std::copy (dir, dir + 3, ray.dir);
                    ray.tmin = options.tmin;
                    // Include the aimed surface. Visibility is first-hit TO
                    // membership, so another triangle of the same target is a
                    // success rather than an occluder in front of a rear aim.
                    ray.tmax = distance + (std::max) (options.tmin, 1.0e-6);
                    rays.push_back (ray);
                    samples.push_back (sample);
                }
            }
            std::vector<uint8_t> visible;
            if (!rays.empty ())
                ClassifyFirstHitTargets (*engine, rays, toMask, visible, options.maxParallel, isCancelled);
            if (Cancelled (isCancelled)) {
                result.error = "visibility study cancelled";
                return result;
            }
            result.rayCount += rays.size ();
            for (size_t i = 0; i < rays.size (); ++i)
                if (visible[i] != 0)
                    result.values[samples[i]] += 1.0;
        }
        for (double& value : result.values)
            value /= static_cast<double> (aims.size ());
    }

    if (Cancelled (isCancelled)) {
        result.error = "visibility study cancelled";
        return result;
    }
    for (const double value : result.values) {
        result.meanVisibility += value;
        if (value > 0.0)
            ++result.visibleSamples;
    }
    result.meanVisibility /= static_cast<double> (result.values.size ());
    result.analysisMilliseconds =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    result.valid = true;
    return result;
}

} // namespace evp::sunstudy
