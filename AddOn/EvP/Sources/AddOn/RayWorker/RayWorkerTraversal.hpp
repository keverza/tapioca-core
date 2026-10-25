#ifndef EVP_RAYWORKER_RAYWORKERTRAVERSAL_HPP
#define EVP_RAYWORKER_RAYWORKERTRAVERSAL_HPP

// RayWorker/RayWorkerTraversal -- the sun study's ITraversal over an
// out-of-process GPU worker, behind the developer override
// TAPIOCA_SUNSTUDY_TRAVERSAL=dxr (HANDOFF-SunStudyRust.md §7.1 G1/G4: "first
// integrate through the existing one-direction callback").
//
// Each OccludeDirectional call is one single-step request: the origins as
// samples, no normals (the accumulator already culled back faces), the step's
// direction, tmin/tmax unchanged. The worker's lit bit is the inverse of the
// occluded byte. Correct first, not fast: positions cross the segment every
// step until G4's resident multi-step batches.
//
// ⚠️ ANY FAILURE GOES TO THE CPU, FOR GOOD. A refused, crashed, timed-out or
// invalid worker answer makes this traversal answer that call -- and every
// later one -- with its CPU traversal, and says why once in the log. A
// cancelled call returns false (the step stays uncommitted) without fallback.

#include "RayWorker/RayWorkerClient.hpp"
#include "RayWorker/RayWorkerWin32.hpp"
#include "SunStudy/ITraversal.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace geomsrv {
class QueryEngine;
}

namespace evp::rayworker {

// One worker conversation shared by every study: one supervisor, one process.
// Run() is serialised; the caller's thread pumps the client until an outcome.
class RayWorkerChannel final {
  public:
    explicit RayWorkerChannel (std::wstring executable, win32::WorkerProcess::InProcessWorker inProcess = {});
    ~RayWorkerChannel ();

    // `superseded` set and `published` false means the caller cancelled.
    RayWorkerOutcome Run (uint64_t sceneEpoch, std::shared_ptr<const packet::Scene> scene, packet::Request request,
                          const std::function<bool ()>& isCancelled, int64_t timeoutMs);
    // Stop the worker and join the host's threads; later Runs relaunch.
    void Shutdown ();
    std::string Describe ();

  private:
    std::mutex mutex_;
    win32::WorkerProcess host_;
    RayWorkerClient client_;
    uint64_t generation_ = 0;
};

class RayWorkerTraversal final : public sunstudy::ITraversal {
  public:
    // `log` receives the one fallback line (the caller's pipeline log); may be empty.
    RayWorkerTraversal (std::shared_ptr<const sunstudy::ITraversal> cpu, std::shared_ptr<RayWorkerChannel> channel,
                        std::shared_ptr<const packet::Scene> scene, std::string backendName,
                        std::function<void (const std::string&)> log = {});

    void OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin, double tmax,
                             uint8_t* out, size_t maxParallel = 0) const override;
    bool OccludeDirectionalCancellable (const double* origins, size_t count, const double dir[3], double tmin,
                                        double tmax, uint8_t* out, size_t maxParallel,
                                        const std::function<bool ()>& isCancelled) const override;
    // Arbitrary rays are not on the worker wire yet (G0 item 2): CPU.
    void OccludeRays (const sunstudy::OcclusionRay* rays, size_t count, uint8_t* out,
                      size_t maxParallel = 0) const override;
    uint64_t SceneVersion () const override;

    // "dxr", or "cpu" once a failure moved this traversal to the CPU.
    std::string ActualBackend () const;
    GpuReason FallbackReason () const;
    uint64_t GpuCalls () const
    {
        return gpuCalls_.load ();
    }

  private:
    std::shared_ptr<const sunstudy::ITraversal> cpu_;
    std::shared_ptr<RayWorkerChannel> channel_;
    std::shared_ptr<const packet::Scene> scene_;
    std::string backendName_;
    std::function<void (const std::string&)> log_;
    uint64_t epoch_;
    mutable std::atomic<bool> onCpu_ { false };
    mutable std::atomic<uint32_t> reason_ { 0 };
    mutable std::atomic<uint64_t> gpuCalls_ { 0 };
};

// TAPIOCA_SUNSTUDY_TRAVERSAL, lower-cased, when it names a worker backend:
// "dxr" or "optix". Anything else, or unset, is "" and leaves the study's own
// traversal.
std::string RequestedWorkerTraversal ();

// The scene a worker uploads: the analysis index's snapshot, then the context
// index's (occlusion is the OR of both, as SunStudyPartitionTraversal does).
std::shared_ptr<const packet::Scene> SceneFromIndices (const geomsrv::QueryEngine* analysis,
                                                       const geomsrv::QueryEngine* context);

// The add-on's channel for `backend` ("dxr" -> TapiocaDxrWorker.exe, "optix"
// -> TapiocaOptixWorker.exe, beside the .apx), created on first use; null for
// any other name. One worker per backend per session.
std::shared_ptr<RayWorkerChannel> WorkerChannel (const std::string& backend);
// Quit/FreeData: stop every worker before the module goes away. Idempotent.
void ShutdownRayWorkers ();

} // namespace evp::rayworker

#endif
