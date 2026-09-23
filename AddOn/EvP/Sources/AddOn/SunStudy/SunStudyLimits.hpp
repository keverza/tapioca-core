#ifndef EVP_SUNSTUDY_SUNSTUDYLIMITS_HPP
#define EVP_SUNSTUDY_SUNSTUDYLIMITS_HPP

// SunStudy/SunStudyLimits -- how big a study THIS machine can run and show
// without running out of memory, as numbers a person can read and the start
// command enforces.
//
// Four ceilings, and the smallest one binds:
//
//   * the SAMPLER's own cap (a fixed ceiling on a study's sample count);
//   * SYSTEM MEMORY: every sample is held several times over -- the study's
//     own positions and normals, its sampling grid, its per-step lit bits,
//     and the display's copy of the atlas images;
//   * GPU MEMORY: the hours atlas and the per-step bit slices, per texel;
//   * the largest TEXTURE the GPU (and the atlas packer) will make.
//
// ⚠️ REFUSED, NEVER TRUNCATED, AND SAID IN ADVANCE. A study that would not fit
// is refused at the start with the limit that bound it and the finest grid the
// model allows -- not discovered as a driver reset or a paging machine halfway
// through, and not quietly measured on part of the model.
//
// ⚠️ ESTIMATES, AND CONSERVATIVE ONES. The byte counts below follow what the
// code actually holds per sample; the budgets take a FRACTION of what is free,
// because the viewer, Archicad and the operating system share the same memory.

#include <cstddef>
#include <cstdint>
#include <string>

namespace evp::sunstudy {

// What the machine offers. GPU facts are published by the viewer when its
// device exists; before that they are unknown and do not bind.
struct MachineResources {
    bool gpuKnown = false;
    std::string adapter;
    uint64_t gpuMemoryBytes = 0;      // dedicated video memory
    uint32_t maxTextureDimension = 0; // the device's 2D texture limit
    uint64_t availableRamBytes = 0;   // free physical memory right now
};

enum class LimitBinding : uint8_t {
    SamplerCap = 0,
    SystemMemory = 1,
    GpuMemory = 2,
    TextureSize = 3,
};

struct AnalysisLimits {
    size_t maxSamples = 0;
    // The atlas packer's ceiling for this machine: min(device limit, 8192).
    uint32_t maxAtlasDimension = 0;
    LimitBinding binding = LimitBinding::SamplerCap;
    // Rays one study casts at the ceiling: samples x steps.
    uint64_t maxRays = 0;
    // The per-limit sample ceilings, for the HUD's breakdown. 0 = unknown.
    size_t samplerCap = 0;
    size_t ramCap = 0;
    size_t gpuCap = 0;
    size_t textureCap = 0;
};

// The sampler's fixed ceiling and the atlas packer's texture ceiling.
constexpr size_t kSamplerMaxSamples = 4000000;
constexpr uint32_t kAtlasPackerMaxDimension = 8192;

// Measured atlas occupancy (used texels / all texels): one tile per surface
// packs tightly, one per triangle leaves half of every tile empty.
constexpr double kPatchAtlasOccupancy = 0.70;
constexpr double kTriangleAtlasOccupancy = 0.15;

// The fraction of free memory a study may take.
constexpr double kRamBudgetFraction = 0.50;
constexpr double kGpuBudgetFraction = 0.25;

// Bytes one sample costs in system memory at `steps` timesteps, display copy
// included, and bytes one atlas texel costs on the GPU.
double SystemBytesPerSample (size_t steps, double occupancy);
double GpuBytesPerTexel (size_t steps);

AnalysisLimits ComputeAnalysisLimits (const MachineResources& machine, size_t steps, double occupancy);

// The finest grid spacing (m) at which `analysedArea` (m2) stays within
// `maxSamples`: one sample per spacing-squared of surface.
double FinestGridSpacing (double analysedArea, size_t maxSamples);

const char* LimitBindingName (LimitBinding binding);

// The viewer's GPU facts, published once its device exists; thread-safe.
void PublishGpuResources (const std::string& adapter, uint64_t gpuMemoryBytes, uint32_t maxTextureDimension);
// The published GPU facts with `availableRamBytes` filled in by the caller,
// who knows how to ask the operating system.
MachineResources CurrentMachineResources (uint64_t availableRamBytes);

// Free physical memory right now, from the operating system; 0 when unknown
// (and off Windows, where the offline tests build).
uint64_t AvailableSystemMemory ();

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYLIMITS_HPP
