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

std::string AtlasDisplayRefusal (uint32_t width, uint32_t height, uint32_t deviceMaxDimension)
{
    if (deviceMaxDimension == 0 || (width <= deviceMaxDimension && height <= deviceMaxDimension))
        return std::string ();
    return "the " + std::to_string (width) + "x" + std::to_string (height) + " atlas exceeds this GPU's " +
           std::to_string (deviceMaxDimension) + "x" + std::to_string (deviceMaxDimension) +
           " texture limit - run the study again so it is packed for this GPU, or ask for a coarser grid";
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
