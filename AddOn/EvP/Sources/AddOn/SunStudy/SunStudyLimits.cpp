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

size_t Words32 (size_t steps)
{
    return std::max<size_t> (1, (steps + 31) / 32);
}

} // namespace

double SystemBytesPerSample (size_t steps, double occupancy)
{
    // The study's own positions and normals (48), its sampling grid's copy of
    // them plus area, cell and span (68), the hours (8), the accumulator's lit
    // bits (64-bit words per sample), and the display's CPU copy of the atlas
    // images at the atlas's occupancy.
    const double bits = 8.0 * double ((steps + 63) / 64);
    const double texels = 1.0 / std::max (occupancy, 0.01);
    return 48.0 + 68.0 + 8.0 + bits + texels * GpuBytesPerTexel (steps);
}

double GpuBytesPerTexel (size_t steps)
{
    // R32_FLOAT hours + one R32_UINT slice per 32 steps.
    return 4.0 + 4.0 * double (Words32 (steps));
}

AnalysisLimits ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy)
{
    AnalysisLimits out;
    const double occ = std::clamp (occupancy, 0.01, 1.0);
    out.samplerCap = kSamplerMaxSamples;

    if (machine.availableRamBytes > 0) {
        const double budget = double (machine.availableRamBytes) * kRamBudgetFraction;
        out.ramCap = size_t (budget / SystemBytesPerSample (steps, occ));
    }

    out.maxAtlasDimension = kAtlasPackerMaxDimension;
    if (machine.gpuKnown) {
        if (machine.maxTextureDimension > 0)
            out.maxAtlasDimension = std::min (out.maxAtlasDimension, machine.maxTextureDimension);
        const double texels = double (out.maxAtlasDimension) * double (out.maxAtlasDimension);
        out.textureCap = size_t (texels * occ);
        if (machine.gpuMemoryBytes > 0) {
            const double budget = double (machine.gpuMemoryBytes) * kGpuBudgetFraction;
            out.gpuCap = size_t (budget / GpuBytesPerTexel (steps) * occ);
        }
    }
    else {
        const double texels = double (out.maxAtlasDimension) * double (out.maxAtlasDimension);
        out.textureCap = size_t (texels * occ);
    }

    out.maxSamples = out.samplerCap;
    out.binding = LimitBinding::SamplerCap;
    auto bind = [&out] (size_t cap, LimitBinding why) {
        if (cap > 0 && cap < out.maxSamples) {
            out.maxSamples = cap;
            out.binding = why;
        }
    };
    bind (out.ramCap, LimitBinding::SystemMemory);
    bind (out.gpuCap, LimitBinding::GpuMemory);
    bind (out.textureCap, LimitBinding::TextureSize);
    out.maxRays = uint64_t (out.maxSamples) * uint64_t (steps);
    return out;
}

double FinestGridSpacing (double analysedArea, size_t maxSamples)
{
    if (!(analysedArea > 0.0) || maxSamples == 0)
        return 0.0;
    return std::sqrt (analysedArea / double (maxSamples));
}

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
