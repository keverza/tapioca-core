#include "RayWorker/RayWorkerReason.hpp"

namespace evp::rayworker {

const char* GpuReasonCode (GpuReason reason)
{
    switch (reason) {
        case GpuReason::None:
            return "none";
        case GpuReason::WorkerCrashed:
            return "worker_crashed";
        case GpuReason::DriverIncompatible:
            return "driver_incompatible";
        case GpuReason::GpuUnsupported:
            return "gpu_unsupported";
        case GpuReason::OutOfMemory:
            return "out_of_memory";
        case GpuReason::WorkerMissing:
            return "worker_missing";
        case GpuReason::ProtocolMismatch:
            return "protocol_mismatch";
        case GpuReason::DisabledForSession:
            return "gpu_disabled_for_session";
        case GpuReason::WorkerFailed:
            return "worker_failed";
    }
    return "unknown";
}

std::string DescribeGpuReason (GpuReason reason)
{
    switch (reason) {
        case GpuReason::None:
            return "GPU ray queries are available";
        case GpuReason::WorkerCrashed:
            return "the GPU worker stopped unexpectedly; its study was discarded and runs on the CPU";
        case GpuReason::DriverIncompatible:
            return "the graphics driver is older than the GPU ray-tracing runtime requires; running on the CPU";
        case GpuReason::GpuUnsupported:
            return "no GPU here supports hardware ray tracing; running on the CPU";
        case GpuReason::OutOfMemory:
            return "the GPU ran out of memory for this model; running on the CPU";
        case GpuReason::WorkerMissing:
            return "the GPU worker is not installed beside the add-on; running on the CPU";
        case GpuReason::ProtocolMismatch:
            return "the GPU worker belongs to another add-on version; reinstall both; running on the CPU";
        case GpuReason::DisabledForSession:
            return "the GPU worker failed repeatedly and is disabled until Archicad restarts; running on the CPU";
        case GpuReason::WorkerFailed:
            return "the GPU worker could not complete the request; running on the CPU";
    }
    return "GPU ray queries are unavailable for an unknown reason; running on the CPU";
}

bool IsKnownGpuReason (uint32_t value)
{
    return value <= static_cast<uint32_t> (GpuReason::WorkerFailed);
}

} // namespace evp::rayworker
