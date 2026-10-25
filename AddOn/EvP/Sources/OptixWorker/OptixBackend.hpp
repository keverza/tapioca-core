#ifndef EVP_OPTIXWORKER_OPTIXBACKEND_HPP
#define EVP_OPTIXWORKER_OPTIXBACKEND_HPP

// OptixWorker/OptixBackend -- the RayBackend that traces the sun study's
// occlusion rays with NVIDIA OptiX (HANDOFF-SunStudyRust.md §7.1 G3).
//
// It answers exactly what MakeCpuReferenceBackend and the DXR backend answer,
// behind the same wire; the CPU reference is its oracle. The CPU side (shared
// origin, double back-face culling, clear cases, bit packing) is
// RayWorkerBatch, shared with DXR; this file owns only CUDA and OptiX.
//
// ⚠️ IT RUNS IN TapiocaOptixWorker.exe, NEVER INSIDE ARCHICAD. CUDA and OptiX
// come from the display driver (nvcuda.dll, nvoptix.dll); the toolkit and the
// SDK are build-time only. One GAS over all triangles, resident across sun-only
// requests; a new scene is a full rebuild.

#include "RayWorker/RayWorkerService.hpp"

#include <memory>

namespace evp::optixworker {

struct OptixBackendOptions {
    // Rays per optixLaunch; cancellation is polled between launches.
    uint32_t raysPerLaunch = 1u << 22;
};

// Never null: without a CUDA device or a compatible driver the backend still
// answers Hello with the reason, and refuses scenes and runs.
std::unique_ptr<rayworker::RayBackend> MakeOptixBackend (const OptixBackendOptions& options = {});

} // namespace evp::optixworker

#endif
