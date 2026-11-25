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
