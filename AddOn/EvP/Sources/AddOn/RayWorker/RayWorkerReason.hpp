#ifndef EVP_RAYWORKER_RAYWORKERREASON_HPP
#define EVP_RAYWORKER_RAYWORKERREASON_HPP

// RayWorker/RayWorkerReason -- why GPU ray queries are not available, as a
// stable code a person and a script can both act on.
//
// ⚠️ THE CODES ARE CONTRACT. They reach the Python log, the API response and
// the Debug tab unchanged (raycast decision 18), and scripts compare them as
// strings. Add new codes; never rename, renumber or reuse one. The numeric
// value travels on the worker wire, the text everywhere else.

#include <cstdint>
#include <string>

namespace evp::rayworker {

enum class GpuReason : uint32_t {
    None = 0,
    // The worker process died (or stopped answering) while it held work.
    WorkerCrashed = 1,
    // The installed driver is older than the pinned runtime requires.
    DriverIncompatible = 2,
    // No device that can run the hardware ray-tracing pipeline.
    GpuUnsupported = 3,
    // The device ran out of memory building or tracing the scene.
    OutOfMemory = 4,
    // The bundled worker executable is not where the package puts it.
    WorkerMissing = 5,
    // Additions after the first five (decision 18 allows extension):
    // The worker speaks another protocol version (stale worker beside a newer add-on).
    ProtocolMismatch = 6,
    // Repeated crashes disabled GPU execution for this session.
    DisabledForSession = 7,
    // The worker refused or failed a request for a reason not listed above.
    WorkerFailed = 8,
    // G0 additions (HANDOFF-SunStudyRust.md §7.1 G1 reasons):
    // A D3D12 device exists but no adapter offers DXR Tier 1.0.
    DxrUnsupported = 9,
    // The worker or the GPU missed a startup, heartbeat or dispatch deadline.
    WorkerTimeout = 10,
    // The graphics device was removed or reset during the work.
    DeviceRemoved = 11,
    // The device cannot meet the precision contract for this scene.
    PrecisionUnsupported = 12,
    // A result failed validation (corrupt, stale or inconsistent).
    ValidationFailed = 13,
};

// The stable text, e.g. "worker_crashed". Unknown values map to "unknown".
const char* GpuReasonCode (GpuReason reason);

// A sentence for the notice ("GPU unavailable: ... -- running on the CPU").
std::string DescribeGpuReason (GpuReason reason);

// Whether a value read from the wire names a known reason.
bool IsKnownGpuReason (uint32_t value);

} // namespace evp::rayworker

#endif
