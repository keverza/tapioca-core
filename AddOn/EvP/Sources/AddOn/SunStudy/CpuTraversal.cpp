#include "SunStudy/CpuTraversal.hpp"

#include <algorithm>
#include <thread>
#include <vector>

namespace evp::sunstudy {

namespace {

// Below this, a thread costs more than the work it would take away. Measured in
// queries, not in samples: a coarse progressive rung is a few thousand.
constexpr size_t kInlineThreshold = 4096;

// The least work that justifies a thread. Measured, not guessed: see the note in
// ChooseThreadCount.
constexpr size_t kMinRaysPerThread = 2048;

// Shard [0, count) across `threads` workers and join. `threads <= 1` runs inline
// on the caller.
//
// ⚠️ THE CALLER RUNS THE LAST SHARD RATHER THAN WAITING FOR IT. One fewer thread
// is created and the submitting thread is not idle while the pool works, which
// matters because this is called once per timestep in a loop.
template <typename Body> void ShardAndJoin (size_t count, size_t threads, const Body& body)
{
    if (threads <= 1) {
        body (size_t { 0 }, count);
        return;
    }

    const size_t chunk = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    pool.reserve (threads - 1);

    size_t begin = chunk; // shard 0 is the caller's, run last
    for (size_t t = 1; t < threads && begin < count; ++t) {
        const size_t end = std::min (count, begin + chunk);
        pool.emplace_back (body, begin, end);
        begin = end;
    }

    body (size_t { 0 }, std::min (chunk, count));

    for (std::thread& worker : pool)
        worker.join ();
}

} // namespace

size_t ChooseThreadCount (size_t count, size_t maxParallel)
{
    if (count == 0)
        return 1;
    if (maxParallel == 1)
        return 1;
    if (count < kInlineThreshold)
        return 1;

    size_t threads = maxParallel;
    if (threads == 0) {
        const unsigned hardware = std::thread::hardware_concurrency ();
        threads = (hardware == 0) ? 1u : hardware;

        // ⚠️ A THREAD IS ONLY WORTH SPAWNING IF IT HAS REAL WORK TO DO, and the
        // first live run is what taught this. A 9,403-sample study is roughly
        // 4,700 front-facing rays a timestep -- just over the inline threshold,
        // so it fanned out to every core and gave each about 290 rays. Creating
        // the thread cost more than tracing them, and the study measured
        // 1.24 M rays/s. The same engine on 176,106 samples, where each thread
        // got thousands of rays, measured 7.14 M -- SIX TIMES FASTER PER RAY on
        // identical code and identical geometry.
        //
        // So the fan-out is bounded by work, not just by cores. This is only
        // applied to the "you decide" case: an explicit `maxParallel` is a
        // measurement knob and must be obeyed exactly, or the serial-versus-
        // parallel comparison it exists for silently stops being a comparison.
        const size_t byWork = count / kMinRaysPerThread;
        threads = std::min (threads, std::max<size_t> (1, byWork));
    }

    // Never more workers than there is work for, and never zero.
    threads = std::min (threads, count);
    return std::max<size_t> (1, threads);
}

CpuTraversal::CpuTraversal (std::shared_ptr<const geomsrv::QueryEngine> engine) : engine_ (std::move (engine))
{
}

void CpuTraversal::OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin,
                                       double tmax, uint8_t* out, size_t maxParallel) const
{
    if (out == nullptr || count == 0)
        return;

    // ⚠️ CLEAR RATHER THAN SHADOWED IS THE SAFE DEFAULT for a missing scene or a
    // null input. "Lit" is visible in a result and invites a look; "shadowed"
    // reads exactly like a real occluder and hides the fault.
    if (origins == nullptr || dir == nullptr || engine_ == nullptr) {
        std::fill (out, out + count, uint8_t { 0 });
        return;
    }

    const geomsrv::QueryEngine& engine = *engine_;
    const double direction[3] = { dir[0], dir[1], dir[2] };

    const auto body = [&engine, origins, &direction, tmin, tmax, out] (size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            out[i] = engine.Occluded (&origins[i * 3], direction, tmin, tmax) ? uint8_t { 1 } : uint8_t { 0 };
    };

    ShardAndJoin (count, ChooseThreadCount (count, maxParallel), body);
}

void CpuTraversal::OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out, size_t maxParallel) const
{
    if (out == nullptr || count == 0)
        return;

    if (rays == nullptr || engine_ == nullptr) {
        std::fill (out, out + count, uint8_t { 0 });
        return;
    }

    const geomsrv::QueryEngine& engine = *engine_;

    const auto body = [&engine, rays, out] (size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const OcclusionRay& ray = rays[i];
            out[i] = engine.Occluded (ray.origin, ray.dir, ray.tmin, ray.tmax) ? uint8_t { 1 } : uint8_t { 0 };
        }
    };

    ShardAndJoin (count, ChooseThreadCount (count, maxParallel), body);
}

uint64_t CpuTraversal::SceneVersion () const
{
    // The snapshot id IS the version, and reusing it rather than counting
    // separately is what makes it impossible for the two to disagree: a
    // QueryEngine is immutable and one exists per snapshot, so "the geometry
    // changed" and "the snapshot id changed" are the same event.
    //
    // Zero means no scene, which is what QueryEngine reports for an empty one.
    return (engine_ == nullptr) ? 0u : engine_->SnapshotId ();
}

} // namespace evp::sunstudy
