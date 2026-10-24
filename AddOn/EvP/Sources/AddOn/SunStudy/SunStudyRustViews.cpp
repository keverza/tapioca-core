#include "SunStudy/SunStudyRustViews.hpp"
#include "SunStudy/SunStudyRustDetail.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/CpuTraversalWork.hpp"

#include <algorithm>
#include <chrono>
#include <memory>

namespace evp::sunstudy::rust {

using namespace detail;

static_assert (sizeof (void*) != 8 || sizeof (TapiocaFirstHitRay) == 56);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaRayHit) == 16);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaBoundedRay) == 64);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaRayQueryVtable) == 40);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaViewpointRequest) == 80);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaViewpointOutput) == 32);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaVisibilityOptions) == 120);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaVisibilityRequest) == 192);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaVisibilitySummary) == 88);
static_assert (sizeof (void*) != 8 || sizeof (TapiocaVisibilityReadOutput) == 112);

namespace {

// The C++ side of the ray queries: the snapshot's shared query index,
// acquired on `prepare`, and the caller's cancel check.
struct RayShim {
    std::shared_ptr<const geomsrv::Snapshot> snapshot;
    std::shared_ptr<const geomsrv::QueryEngine> engine;
    const std::function<bool ()>* isCancelled = nullptr;
};

uint8_t Prepare (void* context)
{
    try {
        auto& shim = *static_cast<RayShim*> (context);
        if (shim.engine == nullptr)
            shim.engine = geomsrv::QueryIndexCache::Get ().For (shim.snapshot);
        return shim.engine != nullptr ? 1 : 2;
    }
    catch (...) {
        return 2;
    }
}

uint8_t Cancelled (void* context)
{
    try {
        const auto& shim = *static_cast<const RayShim*> (context);
        return shim.isCancelled != nullptr && *shim.isCancelled && (*shim.isCancelled) () ? 1 : 0;
    }
    catch (...) {
        return 1;
    }
}

uint8_t FirstHits (void* context, const TapiocaFirstHitRay* rays, size_t count, const uint8_t* mask, size_t maskCount,
                   TapiocaRayHit* out, size_t maxParallel)
{
    try {
        auto& shim = *static_cast<RayShim*> (context);
        if (Prepare (context) != 1)
            return 2;
        const std::vector<uint8_t> meshMask (mask, mask + maskCount);
        const auto body = [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const auto hit =
                    mask != nullptr ? shim.engine->RaycastMasked (rays[i].origin, rays[i].dir, rays[i].length, meshMask)
                                    : shim.engine->Raycast (rays[i].origin, rays[i].dir, rays[i].length);
                out[i] = { hit.t, static_cast<uint32_t> (hit.meshIndex), static_cast<uint8_t> (hit.hit ? 1 : 0) };
            }
        };
        static const std::function<bool ()> kNever;
        const std::function<bool ()>& cancel = shim.isCancelled != nullptr ? *shim.isCancelled : kNever;
        RunCpuTraversal (count, maxParallel, cancel, body);
        return cancel && cancel () ? 0 : 1;
    }
    catch (...) {
        return 2;
    }
}

uint8_t OccludeRays (void* context, const TapiocaBoundedRay* rays, size_t count, uint8_t* out, size_t maxParallel)
{
    try {
        auto& shim = *static_cast<RayShim*> (context);
        if (Prepare (context) != 1)
            return 2;
        std::vector<OcclusionRay> converted (count);
        for (size_t i = 0; i < count; ++i) {
            std::copy (rays[i].origin, rays[i].origin + 3, converted[i].origin);
            std::copy (rays[i].dir, rays[i].dir + 3, converted[i].dir);
            converted[i].tmin = rays[i].tmin;
            converted[i].tmax = rays[i].tmax;
        }
        CpuTraversal (shim.engine).OccludeRays (converted.data (), count, out, maxParallel);
        return 1;
    }
    catch (...) {
        return 2;
    }
}

TapiocaRayQueryVtable VtableOf (RayShim& shim)
{
    return { &shim, &Prepare, &FirstHits, &OccludeRays, &Cancelled };
}

struct VisibilityDeleter {
    void operator() (TapiocaVisibilityResult* handle) const noexcept
    {
        ErrorBuffer buffer {};
        tapioca_sunstudy_visibility_destroy (handle, buffer.data (), buffer.size ());
    }
};

struct SurfaceDeleter {
    void operator() (TapiocaSurfaceResult* handle) const noexcept
    {
        ErrorBuffer buffer {};
        tapioca_sunstudy_surface_destroy (handle, buffer.data (), buffer.size ());
    }
};

} // namespace

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled)
{
    ViewpointStudyResult result;
    result.options = options;
    if (snapshot == nullptr) {
        result.error = "viewpoint needs a snapshot, finite point, positive radius and 32..1440 rays";
        return result;
    }
    RayShim shim { snapshot, nullptr, &isCancelled };
    const TapiocaRayQueryVtable vtable = VtableOf (shim);
    const std::vector<TapiocaReuseMeshView> meshes = MeshViews (*snapshot);
    const std::vector<TapiocaTextView> picked = TextViews (context);
    TapiocaViewpointRequest request {};
    request.meshes = { meshes.data (), meshes.size () };
    request.context = ArrayView (picked);
    std::copy (options.point.begin (), options.point.end (), request.point);
    request.radius = options.radius;
    request.rays = options.rays;
    request.queries = &vtable;
    std::vector<std::array<double, 3>> perimeter (options.rays);
    size_t counts[2] = { 0, 0 };
    double area = 0.0;
    const TapiocaViewpointOutput output { reinterpret_cast<double (*)[3]> (perimeter.data ()), perimeter.size (),
                                          &counts, &area };
    static_assert (sizeof (std::array<double, 3>) == sizeof (double[3]));
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_viewpoint (&request, &output, m, n); },
                 result.error))
        return result;
    result.snapshotId = snapshot->id;
    result.captureStamp = snapshot->captureStamp;
    perimeter.resize (counts[0]);
    result.perimeter = std::move (perimeter);
    result.collisionCount = counts[1];
    result.area = area;
    result.valid = true;
    return result;
}

VisibilityStudyResult RunVisibilityStudy (std::shared_ptr<const geomsrv::Snapshot> snapshot,
                                          const std::vector<std::string>& fromElements,
                                          const std::vector<std::string>& toElements,
                                          const VisibilityStudyOptions& options,
                                          const std::function<bool ()>& isCancelled)
{
    const auto started = std::chrono::steady_clock::now ();
    VisibilityStudyResult result;
    result.origin = options.origin;
    result.domain = options.domain;
    result.spacing = options.spacing;
    if (snapshot == nullptr || snapshot->meshes.empty ()) {
        result.error = "visibility study needs a live non-empty snapshot";
        return result;
    }
    result.snapshotId = snapshot->id;
    RayShim shim { snapshot, nullptr, &isCancelled };
    const TapiocaRayQueryVtable vtable = VtableOf (shim);
    const std::vector<TapiocaReuseMeshView> meshes = MeshViews (*snapshot);
    const auto from = TextViews (fromElements), to = TextViews (toElements),
               context = TextViews (options.contextElements);
    TapiocaVisibilityRequest request {};
    request.meshes = { meshes.data (), meshes.size () };
    request.roles[0] = ArrayView (from);
    request.roles[1] = ArrayView (to);
    request.roles[2] = ArrayView (context);
    TapiocaVisibilityOptions& o = request.options;
    o.reals[0] = options.spacing;
    o.reals[1] = options.normalOffset;
    o.reals[2] = options.tmin;
    o.reals[3] = options.coneDegrees;
    o.limits[0] = options.maxSamples;
    o.limits[1] = options.maxAimPoints;
    o.limits[2] = options.maxRays;
    o.limits[3] = options.maxParallel;
    std::copy (options.point, options.point + 3, o.point);
    std::copy (options.direction, options.direction + 3, o.direction);
    o.origin = static_cast<uint8_t> (options.origin);
    o.domain = static_cast<uint8_t> (options.domain);
    o.explicitContext = options.explicitContext ? 1 : 0;
    request.queries = &vtable;

    TapiocaVisibilityResult* raw = nullptr;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_visibility_run (&request, &raw, m, n); },
                 result.error))
        return result;
    const std::unique_ptr<TapiocaVisibilityResult, VisibilityDeleter> handle (raw);

    TapiocaVisibilitySummary summary {};
    if (!Invoke (
            [&] (char* m, size_t n) { return tapioca_sunstudy_visibility_summary (handle.get (), &summary, m, n); },
            result.error))
        return result;
    TapiocaSurfaceResult* gridsRaw = nullptr;
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_visibility_grids (handle.get (), &gridsRaw, m, n); },
                 result.error))
        return result;
    const std::unique_ptr<TapiocaSurfaceResult, SurfaceDeleter> grids (gridsRaw);
    SurfaceSamplingResult sampling;
    if (!Read (grids.get (), sampling, result.error))
        return result;
    result.grid = std::move (sampling.triangles);
    result.patchGrid = std::move (sampling.patches);

    result.values.assign (summary.sizes[0], 0.0);
    AtlasBuffers atlas;
    Prepare (atlas, result.grid);
    StateBuffers state;
    Prepare (state, summary.sizes[6], summary.sizes[7]);
    const TapiocaVisibilityReadOutput output { result.values.data (), result.values.size (), atlas.output,
                                               state.output };
    if (!Invoke ([&] (char* m, size_t n) { return tapioca_sunstudy_visibility_read (handle.get (), &output, m, n); },
                 result.error))
        return result;
    result.atlas = AtlasOf (atlas);
    result.patchAtlas = SunStudyPatchAtlas::FromState (StateOf (state));
    result.aimPointCount = summary.sizes[1];
    result.rayCount = summary.sizes[2];
    result.visibleSamples = summary.sizes[3];
    result.meanVisibility = summary.reals[0];
    result.analysedArea = summary.reals[1];
    result.displayElements = options.origin == VisibilityOrigin::Point ? toElements : fromElements;
    result.analysisMilliseconds =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    result.valid = true;
    return result;
}

} // namespace evp::sunstudy::rust
