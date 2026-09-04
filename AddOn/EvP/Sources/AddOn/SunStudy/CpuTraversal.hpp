#ifndef EVP_SUNSTUDY_CPUTRAVERSAL_HPP
#define EVP_SUNSTUDY_CPUTRAVERSAL_HPP

// SunStudy/CpuTraversal — the baseline backend, over the snapshot's own BVH.
//
// It adds no acceleration structure and no geometry copy. Geometry/QueryEngine
// already builds a triangle BVH per snapshot, is immutable once built, is safe
// for concurrent reads, and is cached by QueryIndexCache; this class is the
// thread sharding and nothing else.
//
// ⚠️ WHY THE BASELINE IS A CPU BACKEND AT ALL, given the analysis is GPU-shaped:
// the previous implementation of this study issued ONE COMMAND-BUS ROUND TRIP
// PER TIMESTEP and asked for a full sorted all-hits raycast when a yes/no answer
// was wanted. It measured 33 s. Neither cost is the ray tracer, and both vanish
// here. Whether anything faster is needed after that is a measurement, not an
// assumption — see ITraversal.
//
// ⚠️ IT HOLDS THE ENGINE BY shared_ptr, DELIBERATELY. A study runs across many
// frames while the user keeps editing, and a snapshot can be replaced mid-run.
// Holding a reference would leave the analysis reading freed geometry; holding
// ownership means the in-flight run finishes against the scene it started on and
// the version check retires it afterwards.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/ITraversal.hpp"

#include <memory>

namespace evp::sunstudy {

class CpuTraversal final : public ITraversal {
  public:
    explicit CpuTraversal (std::shared_ptr<const geomsrv::QueryEngine> engine);

    void OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin, double tmax,
                             uint8_t* out, size_t maxParallel = 0) const override;

    void OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out, size_t maxParallel = 0) const override;

    uint64_t SceneVersion () const override;

  private:
    std::shared_ptr<const geomsrv::QueryEngine> engine_;
};

// Threads to use for `count` queries when the caller said "decide": hardware
// concurrency, never more than there is work for, never zero.
//
// ⚠️ SMALL BATCHES RUN INLINE. Spawning threads costs more than a few thousand
// occlusion queries take, and a coarse progressive rung is exactly that size --
// so the first, most latency-sensitive rung must not pay for a thread pool it
// cannot fill.
size_t ChooseThreadCount (size_t count, size_t maxParallel);

} // namespace evp::sunstudy

#endif
