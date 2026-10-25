#ifndef EVP_DXRWORKER_DXRBACKEND_HPP
#define EVP_DXRWORKER_DXRBACKEND_HPP

// DxrWorker/DxrBackend -- the RayBackend that traces the sun study's
// occlusion rays with D3D12 hardware ray tracing (DXR Tier 1.0, DispatchRays).
//
// It answers exactly what MakeCpuReferenceBackend answers, behind the same
// wire, so the CPU reference is its oracle (G2/G5): the result is the
// accumulator's lit bitset, sample-major, bit `step % 64`.
//
// What it keeps from the CPU path, deliberately:
// - back-facing samples are culled in DOUBLE on the CPU (incidence <= 0, the
//   accumulator's own test), so the GPU never decides a self-shadowed sample;
// - empty scenes, zero directions and empty [tmin, tmax] intervals are clear,
//   as QueryEngine::Occluded returns them, without a dispatch;
// - every triangle is opaque and two-sided; any hit inside the interval
//   occludes.
// What it changes: traversal runs in f32 in a local frame (f64 world minus one
// shared origin). That can flip a ray that grazes an edge; G5 measures it and
// decides CPU correction or fallback. No tolerance is introduced here.
//
// The scene stays resident across requests (sun-only changes reuse the BLAS);
// a new scene is a full rebuild.

#include "RayWorker/RayWorkerService.hpp"

#include <memory>

namespace evp::dxrworker {

struct DxrBackendOptions {
    // Rays per DispatchRays; cancellation is polled between launches.
    uint32_t raysPerLaunch = 1u << 22;
    // Fence wait deadline per submission.
    uint32_t submitTimeoutMs = 20000;
};

// Never null: on a machine without DXR the backend still answers Hello with
// the reason (dxr_unsupported), and refuses scenes and runs.
std::unique_ptr<rayworker::RayBackend> MakeDxrBackend (const DxrBackendOptions& options = {});

} // namespace evp::dxrworker

#endif
