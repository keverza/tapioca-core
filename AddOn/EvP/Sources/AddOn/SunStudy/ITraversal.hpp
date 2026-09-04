#ifndef EVP_SUNSTUDY_ITRAVERSAL_HPP
#define EVP_SUNSTUDY_ITRAVERSAL_HPP

// SunStudy/ITraversal — the one thing the analysis asks of a ray tracer.
//
// Every sun and shadow question this engine answers reduces to "is anything in
// the way", so that is the entire seam. It stays this narrow ON PURPOSE: a
// backend is then a few hundred lines rather than a port, and the analysis above
// it cannot start depending on one tracer's incidental behaviour.
//
// ⚠️ THE POINT OF THE SEAM IS THAT THE MEASUREMENT DECIDES THE BACKEND, not the
// other way round. The CPU backend over the existing snapshot BVH is the
// baseline because it costs nothing new, works on every machine, runs headless,
// and serves every consumer. A GPU backend is worth building only once a
// measured run says the baseline is insufficient — and behind this interface,
// finding that out costs one class rather than a rewrite.
//
// ⚠️ IMPLEMENTATIONS MUST BE SAFE FOR CONCURRENT CALLS AND MUST NOT MUTATE. The
// analysis shards its own work across threads and every backend is handed the
// same immutable scene, so a backend that caches into itself turns a correct
// result into a racy one that is right most of the time.

#include <cstddef>
#include <cstdint>

namespace evp::sunstudy {

// One occlusion query. `dir` need not be normalised; `tmin` and `tmax` are true
// distances in metres either way, and `tmax <= 0` means unbounded.
struct OcclusionRay {
    double origin[3] = { 0.0, 0.0, 0.0 };
    double dir[3] = { 0.0, 0.0, 1.0 };
    double tmin = 0.0;
    double tmax = 0.0;
};

class ITraversal {
  public:
    virtual ~ITraversal () = default;

    // THE HOT PATH: many origins, ONE shared direction. A sunlight-hours run is
    // exactly this, once per timestep.
    //
    // ⚠️ IT TAKES A DIRECTION RATHER THAN A RAY ARRAY BECAUSE THE RAY ARRAY IS
    // THE COST. A day at fifteen-minute steps over a hundred thousand samples is
    // several million queries; materialising a struct per query is hundreds of
    // megabytes written and read for a value that is the same in every element.
    //
    // `origins` is xyz-interleaved, `count` positions. `out` receives `count`
    // bytes: 1 occluded, 0 clear. Both must be sized by the caller.
    //
    // `maxParallel` is 0 for "decide", 1 to run inline on the calling thread.
    // ⚠️ IT IS A MEASUREMENT KNOB, NOT A TUNING ONE — running the same query at
    // 1 and at N is how the parallel speedup is demonstrated rather than
    // asserted, and both arms have to be otherwise identical.
    virtual void OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin, double tmax,
                                     uint8_t* out, size_t maxParallel = 0) const = 0;

    // The general form: every query carries its own direction. Needed by the
    // hemisphere probe, where each sample fires a fan rather than one ray, and
    // by any second-bounce query.
    virtual void OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out, size_t maxParallel = 0) const = 0;

    // Changes when the geometry does, and never otherwise. The analysis folds it
    // into its cache key, so a stale accumulator cannot outlive the model it
    // describes.
    //
    // ⚠️ ZERO MEANS "NO SCENE", NOT "UNCHANGED". A backend with nothing loaded
    // must report zero so an empty scene is distinguishable from an unmodified
    // one; treating them alike is how a study of nothing gets cached as a
    // result.
    virtual uint64_t SceneVersion () const = 0;
};

} // namespace evp::sunstudy

#endif
