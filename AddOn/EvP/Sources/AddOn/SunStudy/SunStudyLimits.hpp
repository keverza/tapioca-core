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
//   * DISPLAY ATLAS GPU MEMORY: the hours atlas and the per-step bit slices,
//     per texel, that the viewer uploads to SHOW a result;
//   * the largest DISPLAY TEXTURE the GPU (and the atlas packer) will make.
//
// ⚠️ THE TWO GPU CEILINGS ARE DISPLAY, NOT COMPUTE (#117). Every study, CPU or
// GPU traced, is shown through the same atlas, so both backends meet them
// alike. The display adapter is not claimed to be the compute adapter: GPU
// traversal checks its own scene budget when the run starts, and nothing here
// models it. With the GPU not yet known, neither display ceiling binds except
// the packer's 8192 texture fallback.
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
#include <vector>

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

// ⚠️ A C ABI with the Rust rule: `GpuMemory` and `TextureSize` keep their names
// and values, but they are the DISPLAY atlas's memory and texture size.
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
    // An ESTIMATE, enforced nowhere: the rays a study at the ceiling would
    // cast, samples x steps. Shown as such, never as a limit (#117).
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

// The most traversal threads a command may ask for (#117), the visibility
// study's precedent. An explicit count is obeyed exactly below the commands
// (a measurement knob), so the ceiling sits at the entry: schema and native.
constexpr size_t kMaxTraversalThreads = 64;
// A requested `maxParallel`: 0 or less decides, above the ceiling is the ceiling.
constexpr size_t ClampTraversalThreads (int64_t requested)
{
    return requested <= 0                                ? size_t (0)
           : requested >= int64_t (kMaxTraversalThreads) ? kMaxTraversalThreads
                                                         : size_t (requested);
}

// The fraction of free memory a study may take.
constexpr double kRamBudgetFraction = 0.50;
constexpr double kGpuBudgetFraction = 0.25;

const char* LimitBindingName (LimitBinding binding);

// The viewer's GPU facts, published once its device exists; thread-safe.
void PublishGpuResources (const std::string& adapter, uint64_t gpuMemoryBytes, uint32_t maxTextureDimension);
// The published GPU facts with `availableRamBytes` filled in by the caller,
// who knows how to ask the operating system.
MachineResources CurrentMachineResources (uint64_t availableRamBytes);

// Why a `width` x `height` study atlas cannot be displayed on a device whose
// 2D texture limit is `deviceMaxDimension` (0 = unknown, never refuses); empty
// when it fits. The start command packs within the GPU published at the time;
// this catches a study packed before the device was known, or for another one,
// before the allocation fails with no reason given (#117).
std::string AtlasDisplayRefusal (uint32_t width, uint32_t height, uint32_t deviceMaxDimension);

// The grid a refused study can ask for instead (#117): the spacing at which
// `areaFloor` m2 fills `kSuggestedGridFill` of `maxSamples`, rounded UP to the
// centimetre and coarser than `requested`; 0 when there is nothing to say.
// Measured live: count = 555k/s^2 + 29.5k/s, so the area term is ~95 % of the
// count at the ceiling, and the 10 % slack covers the boundary cells.
constexpr double kSuggestedGridFill = 0.90;
double SuggestedGridSpacing (double areaFloor, size_t maxSamples, double requested);

// Free physical memory right now, from the operating system; 0 when unknown
// (and off Windows, where the offline tests build).
uint64_t AvailableSystemMemory ();

// ---- the limits readout (#117) -------------------------------------------------
//
// ⚠️ TWO STUDIES, NEVER ONE. The NEXT RUN is what the panel's form would start:
// its backend, the patch domain the panel always asks for, and at most the
// form's step count. The SHOWN STUDY is whatever is on screen, which may be a
// triangle study a command started, on another backend. A readout that sized
// the next run from the shown study passed off one as the other.
struct LimitReportInputs {
    // The next run.
    bool gpuSelected = false;
    size_t steps = 0; // an upper bound: the form's slice before night is dropped
    double finestGridSpacing = 0.0;
    double analysedArea = 0.0; // the shown study's, the only area measured
    // The study on screen; an empty id means none.
    std::string shownStudyId;
    uint64_t shownSamples = 0;
    size_t shownSteps = 0;
    bool shownPatchDomain = false;
    bool shownPreview = false;
    // As the panel's runner recorded it, only when it ran THIS study; all
    // empty when something else started it.
    std::string shownRequestedBackend;
    std::string shownResolvedBackend;
    std::string shownFallbackReason;
};

enum class LimitReportTone : uint8_t { Heading, Plain, Quiet, Warning };

struct LimitReportLine {
    LimitReportTone tone = LimitReportTone::Plain;
    std::string text;
};

// The readout, line by line: what binds the next run, the per-resource
// ceilings with the GPU ones named as DISPLAY, what the selected backend does
// and does not consume, and the shown study apart. `limits` is for the next
// run, computed by the caller. Pure.
std::vector<LimitReportLine> DescribeAnalysisLimits (const AnalysisLimits& limits, const MachineResources& machine,
                                                     const LimitReportInputs& inputs);

// The readout as the HUD last drew it, for SunStudyOverlayState: the HUD text
// is checked by a diagnostic, not only by a screenshot. Thread-safe.
void PublishLimitReport (std::vector<LimitReportLine> lines);
std::vector<LimitReportLine> PublishedLimitReport ();

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYLIMITS_HPP
