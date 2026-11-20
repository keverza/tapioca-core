#include "SunStudy/SunStudyLimits.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace evp::sunstudy {

namespace {

std::mutex gMutex;
MachineResources gGpu;

} // namespace

const char* LimitBindingName (LimitBinding binding)
{
    switch (binding) {
        case LimitBinding::SystemMemory:
            return "system memory";
        case LimitBinding::GpuMemory:
            return "GPU memory";
        case LimitBinding::TextureSize:
            return "GPU texture size";
        default:
            return "the sampler's cap";
    }
}

void PublishGpuResources (const std::string& adapter, uint64_t gpuMemoryBytes, uint32_t maxTextureDimension)
{
    std::lock_guard<std::mutex> lock (gMutex);
    gGpu.gpuKnown = true;
    gGpu.adapter = adapter;
    gGpu.gpuMemoryBytes = gpuMemoryBytes;
    gGpu.maxTextureDimension = maxTextureDimension;
}

uint64_t AvailableSystemMemory ()
{
#ifdef _WIN32
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof (memory);
    return ::GlobalMemoryStatusEx (&memory) ? uint64_t (memory.ullAvailPhys) : 0u;
#else
    return 0u;
#endif
}

MachineResources CurrentMachineResources (uint64_t availableRamBytes)
{
    std::lock_guard<std::mutex> lock (gMutex);
    MachineResources out = gGpu;
    out.availableRamBytes = availableRamBytes;
    return out;
}

} // namespace evp::sunstudy
