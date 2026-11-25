// Tests for SunStudy/SunStudyLimits -- the machine's analysis ceiling.
//
// ⚠️ THE FAILURE THESE PREVENT IS A CRASH, NOT A WRONG NUMBER. A ceiling that
// ignored GPU memory would let a fine grid on a large model allocate a gigabyte
// of atlas on a laptop GPU and take the driver down; one that ignored system
// memory would page the machine to a halt halfway through a study. Each test
// makes one resource the scarce one and requires it to bind.

#include "SunStudy/SunStudyLimits.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

TEST (SunStudyLimits, WithNothingKnownTheSamplersCapBinds)
{
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (MachineResources (), 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::SamplerCap);
    EXPECT_EQ (limits.maxSamples, kSamplerMaxSamples);
    EXPECT_EQ (limits.maxRays, uint64_t (kSamplerMaxSamples) * 49u);
    EXPECT_EQ (limits.maxAtlasDimension, kAtlasPackerMaxDimension);
}

TEST (SunStudyLimits, ALittleSystemMemoryBinds)
{
    MachineResources machine;
    machine.availableRamBytes = 512ull << 20; // 512 MB free
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::SystemMemory);
    EXPECT_LT (limits.maxSamples, kSamplerMaxSamples);
}

TEST (SunStudyLimits, ASmallTextureCeilingBindsAndCapsTheAtlas)
{
    MachineResources machine;
    machine.gpuKnown = true;
    machine.gpuMemoryBytes = 24ull << 30;
    machine.maxTextureDimension = 2048;
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::TextureSize);
    EXPECT_EQ (limits.maxAtlasDimension, 2048u);
}

TEST (SunStudyLimits, TheTriangleDomainAffordsFewerSamplesThanThePatchDomain)
{
    // One tile per triangle leaves most of every tile empty, so the same GPU
    // holds far fewer triangle-domain samples.
    MachineResources machine;
    machine.gpuKnown = true;
    machine.gpuMemoryBytes = 256ull << 20;
    machine.maxTextureDimension = 16384;
    EXPECT_LT (kernels::ComputeAnalysisLimits (machine, 49, kTriangleAtlasOccupancy).maxSamples,
               kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy).maxSamples);
}

TEST (SunStudyLimits, AMeasuredBudgetTooSmallForOneSampleBindsAtZero)
{
    // #117: 0 meant both "not measured" and "measured, too small", so a known
    // tiny budget left the 4M sampler cap in force. Through the real Rust rule.
    MachineResources machine;
    machine.availableRamBytes = 64;
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::SystemMemory);
    EXPECT_EQ (limits.maxSamples, 0u);
}

TEST (SunStudyLimits, AKnownGpuWithUnreportedFactsDoesNotBind)
{
    MachineResources machine;
    machine.gpuKnown = true; // memory and texture limit left unreported (0)
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::SamplerCap);
    EXPECT_EQ (limits.maxSamples, kSamplerMaxSamples);
}

TEST (SunStudyLimits, TheGroundGridHonoursTheMachineCeiling)
{
    // StartSunStudy now passes limits.maxSamples to the ground grid instead of
    // the sampler's fixed 4M default (#117). 34 x 44 m box, 2 m pad, 1 m grid.
    const Vec3 lo { -12, -22, 0 }, hi { 22, 22, 9 };
    const GroundGrid open = kernels::MakeGroundSampleGrid (lo, hi, 1.0, 2.0, 0.1);
    ASSERT_TRUE (open.valid);
    const size_t needed = open.columns * open.rows;
    EXPECT_TRUE (kernels::MakeGroundSampleGrid (lo, hi, 1.0, 2.0, 0.1, needed).valid);
    EXPECT_FALSE (kernels::MakeGroundSampleGrid (lo, hi, 1.0, 2.0, 0.1, needed - 1).valid);
    EXPECT_FALSE (kernels::MakeGroundSampleGrid (lo, hi, 1.0, 2.0, 0.1, 0).valid);
}

TEST (SunStudyLimits, DisplayRefusesAnAtlasLargerThanTheDevice)
{
    // #117: a study packed before the GPU was known (8192 fallback) shown on a
    // 4096 device is refused with the reason, not a nameless allocation failure.
    EXPECT_TRUE (AtlasDisplayRefusal (4096, 4096, 4096).empty ());
    EXPECT_TRUE (AtlasDisplayRefusal (8192, 8192, 0).empty ()); // unknown device: no claim
    const std::string why = AtlasDisplayRefusal (8192, 4096, 4096);
    EXPECT_NE (why.find ("8192x4096"), std::string::npos) << why;
    EXPECT_NE (why.find ("4096x4096 texture limit"), std::string::npos) << why;
    EXPECT_FALSE (AtlasDisplayRefusal (16, 8193, 8192).empty ());
}

TEST (SunStudyLimits, TraversalThreadsAreBoundedAtTheEntry)
{
    // #117: maxParallel reached the tracer verbatim; 0 decides, 1 is inline.
    static_assert (ClampTraversalThreads (0) == 0);
    EXPECT_EQ (ClampTraversalThreads (-5), 0u);
    EXPECT_EQ (ClampTraversalThreads (1), 1u);
    EXPECT_EQ (ClampTraversalThreads (64), 64u);
    EXPECT_EQ (ClampTraversalThreads (65), kMaxTraversalThreads);
    EXPECT_EQ (ClampTraversalThreads (INT64_MAX), kMaxTraversalThreads);
}

// ---- the limits readout (#117) -------------------------------------------------

namespace {

std::string Joined (const std::vector<LimitReportLine>& lines)
{
    std::string text;
    for (const LimitReportLine& line : lines)
        text += line.text + "\n";
    return text;
}

bool Has (const std::string& text, const char* part)
{
    return text.find (part) != std::string::npos;
}

MachineResources Gpu (uint64_t memory, uint32_t texture)
{
    MachineResources machine;
    machine.gpuKnown = true;
    machine.adapter = "Test GPU";
    machine.gpuMemoryBytes = memory;
    machine.maxTextureDimension = texture;
    machine.availableRamBytes = 32ull << 30;
    return machine;
}

std::string Report (const MachineResources& machine, const LimitReportInputs& inputs)
{
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, inputs.steps, kPatchAtlasOccupancy);
    return Joined (DescribeAnalysisLimits (limits, machine, inputs));
}

} // namespace

TEST (SunStudyLimitReport, TheGpuCeilingsAreNamedAsDisplayNotCompute)
{
    EXPECT_STREQ (LimitBindingName (LimitBinding::GpuMemory), "display atlas GPU memory");
    EXPECT_STREQ (LimitBindingName (LimitBinding::TextureSize), "display texture size");
    LimitReportInputs inputs;
    inputs.steps = 49;
    const std::string text = Report (Gpu (256ull << 20, 16384), inputs);
    EXPECT_TRUE (Has (text, "set by display atlas GPU memory")) << text;
    EXPECT_TRUE (Has (text, "display atlas VRAM")) << text;
    EXPECT_TRUE (Has (text, "display texture")) << text;
    EXPECT_FALSE (Has (text, "| GPU ")) << "the old row read as a compute limit:\n" << text;
}

TEST (SunStudyLimitReport, CpuAndGpuRunsMeetTheSameDisplayCeilings)
{
    // The limits take no backend: both upload the same atlas to be shown. Only
    // the line saying what the backend consumes differs.
    const MachineResources machine = Gpu (512ull << 20, 8192);
    LimitReportInputs cpu;
    cpu.steps = 49;
    LimitReportInputs gpu = cpu;
    gpu.gpuSelected = true;
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    const auto a = DescribeAnalysisLimits (limits, machine, cpu);
    const auto b = DescribeAnalysisLimits (limits, machine, gpu);
    ASSERT_EQ (a.size (), b.size ());
    size_t differing = 0;
    for (size_t i = 0; i < a.size (); ++i)
        differing += a[i].text != b[i].text ? 1 : 0;
    EXPECT_EQ (differing, 2u) << "the heading's backend and the backend note, nothing else";
    const std::string cpuText = Joined (a);
    EXPECT_TRUE (Has (cpuText, "Next run: CPU")) << cpuText;
    EXPECT_TRUE (Has (cpuText, "CPU tracing uses system memory")) << cpuText;
    EXPECT_TRUE (Has (cpuText, "not compute")) << cpuText;
    const std::string gpuText = Joined (b);
    EXPECT_TRUE (Has (gpuText, "Next run: GPU")) << gpuText;
    EXPECT_TRUE (Has (gpuText, "checks its own scene budget")) << gpuText;
}

TEST (SunStudyLimitReport, AnUnknownGpuSaysSoAndOnlyThePackerTextureLimitApplies)
{
    MachineResources machine;
    machine.availableRamBytes = 32ull << 30;
    LimitReportInputs inputs;
    inputs.steps = 49;
    const std::string text = Report (machine, inputs);
    EXPECT_TRUE (Has (text, "display GPU not known yet")) << text;
    EXPECT_TRUE (Has (text, "display atlas VRAM ?")) << "unknown, not a 0.0 M ceiling:\n" << text;
    MachineResources nothing;
    const std::string bare = Report (nothing, inputs);
    EXPECT_TRUE (Has (bare, "free memory ?")) << bare;
    EXPECT_TRUE (Has (bare, "free memory not known")) << bare;
}

TEST (SunStudyLimitReport, EachBindingIsNamed)
{
    LimitReportInputs inputs;
    inputs.steps = 49;
    EXPECT_TRUE (Has (Report (MachineResources (), inputs), "set by the sampler's cap"));
    MachineResources ram;
    ram.availableRamBytes = 512ull << 20;
    EXPECT_TRUE (Has (Report (ram, inputs), "set by system memory"));
    EXPECT_TRUE (Has (Report (Gpu (64ull << 20, 16384), inputs), "set by display atlas GPU memory"));
    EXPECT_TRUE (Has (Report (Gpu (24ull << 30, 2048), inputs), "set by display texture size"));
}

TEST (SunStudyLimitReport, RaysAreAnEstimateNotALimit)
{
    LimitReportInputs inputs;
    inputs.steps = 49;
    const std::string text = Report (MachineResources (), inputs);
    EXPECT_TRUE (Has (text, "~196 M rays at that ceiling (an estimate, not a limit)")) << text;
    EXPECT_FALSE (Has (text, "max 196 M rays")) << text;
}

TEST (SunStudyLimitReport, TheNextRunAndTheShownStudyAreNeverOne)
{
    // A triangle study a command started on GPU, 2 steps, on screen; the form's
    // next run is a CPU patch run of up to 49 steps. Each line says which.
    LimitReportInputs inputs;
    inputs.steps = 49;
    inputs.shownStudyId = "sun-7";
    inputs.shownSamples = 1000000;
    inputs.shownSteps = 2;
    inputs.shownPatchDomain = false;
    const std::string text = Report (MachineResources (), inputs);
    EXPECT_TRUE (Has (text, "Next run: CPU, patch domain, up to 49 steps")) << text;
    EXPECT_TRUE (Has (text, "Shown study: sun-7, triangle domain, 2 steps")) << text;
    EXPECT_TRUE (Has (text, "backend not recorded here")) << text;
    EXPECT_TRUE (Has (text, "1.00 M samples, 25% of the next run's limit")) << text;
    LimitReportInputs none;
    none.steps = 49;
    const std::string empty = Report (MachineResources (), none);
    EXPECT_TRUE (Has (empty, "Shown study: none")) << empty;
    EXPECT_FALSE (Has (empty, "of the next run's limit")) << empty;
}

TEST (SunStudyLimitReport, TheShownStudysBackendAndFallbackAreItsOwn)
{
    LimitReportInputs inputs;
    inputs.steps = 49;
    inputs.shownStudyId = "sun-2";
    inputs.shownSamples = 3500000;
    inputs.shownSteps = 49;
    inputs.shownPatchDomain = true;
    inputs.shownRequestedBackend = "gpu";
    inputs.shownResolvedBackend = "cpu";
    inputs.shownFallbackReason = "no capable GPU";
    const MachineResources machine;
    const AnalysisLimits limits = kernels::ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    const auto lines = DescribeAnalysisLimits (limits, machine, inputs);
    const std::string text = Joined (lines);
    EXPECT_TRUE (Has (text, "asked for gpu, ran on cpu: no capable GPU")) << text;
    // Next run is CPU (the form's choice), whatever the shown study asked for.
    EXPECT_TRUE (Has (text, "Next run: CPU")) << text;
    ASSERT_FALSE (lines.empty ());
    EXPECT_EQ (lines.back ().tone, LimitReportTone::Warning) << "88% of the limit is flagged";
}

TEST (SunStudyLimitReport, ThePublishedReportIsWhatTheHudDrew)
{
    PublishLimitReport ({ { LimitReportTone::Heading, "Next run: CPU" } });
    const auto lines = PublishedLimitReport ();
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_EQ (lines[0].text, "Next run: CPU");
    PublishLimitReport ({});
    EXPECT_TRUE (PublishedLimitReport ().empty ());
}
