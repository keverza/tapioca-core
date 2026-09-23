// Tests for SunStudy/SunStudyLimits -- the machine's analysis ceiling.
//
// ⚠️ THE FAILURE THESE PREVENT IS A CRASH, NOT A WRONG NUMBER. A ceiling that
// ignored GPU memory would let a fine grid on a large model allocate a gigabyte
// of atlas on a laptop GPU and take the driver down; one that ignored system
// memory would page the machine to a halt halfway through a study. Each test
// makes one resource the scarce one and requires it to bind.

#include "SunStudy/SunStudyLimits.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

TEST (SunStudyLimits, WithNothingKnownTheSamplersCapBinds)
{
    const AnalysisLimits limits = ComputeAnalysisLimits (MachineResources (), 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::SamplerCap);
    EXPECT_EQ (limits.maxSamples, kSamplerMaxSamples);
    EXPECT_EQ (limits.maxRays, uint64_t (kSamplerMaxSamples) * 49u);
    EXPECT_EQ (limits.maxAtlasDimension, kAtlasPackerMaxDimension);
}

TEST (SunStudyLimits, ALittleGpuMemoryBinds)
{
    MachineResources machine;
    machine.gpuKnown = true;
    machine.gpuMemoryBytes = 128ull << 20; // 128 MB
    machine.maxTextureDimension = 16384;
    const AnalysisLimits limits = ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::GpuMemory);
    // A quarter of 128 MB over 12 bytes a texel (hours + two step slices),
    // at 70 % occupancy.
    const double expected = double (32ull << 20) / GpuBytesPerTexel (49) * kPatchAtlasOccupancy;
    EXPECT_NEAR (double (limits.maxSamples), expected, 1.0);
}

TEST (SunStudyLimits, ALittleSystemMemoryBinds)
{
    MachineResources machine;
    machine.availableRamBytes = 512ull << 20; // 512 MB free
    const AnalysisLimits limits = ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
    EXPECT_EQ (limits.binding, LimitBinding::SystemMemory);
    EXPECT_LT (limits.maxSamples, kSamplerMaxSamples);
}

TEST (SunStudyLimits, ASmallTextureCeilingBindsAndCapsTheAtlas)
{
    MachineResources machine;
    machine.gpuKnown = true;
    machine.gpuMemoryBytes = 24ull << 30;
    machine.maxTextureDimension = 2048;
    const AnalysisLimits limits = ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy);
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
    EXPECT_LT (ComputeAnalysisLimits (machine, 49, kTriangleAtlasOccupancy).maxSamples,
               ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy).maxSamples);
}

TEST (SunStudyLimits, TheFinestGridIsOneSamplePerSpacingSquared)
{
    EXPECT_NEAR (FinestGridSpacing (10000.0, 1000000), 0.1, 1e-12);
    EXPECT_EQ (FinestGridSpacing (0.0, 1000), 0.0);
    EXPECT_EQ (FinestGridSpacing (100.0, 0), 0.0);
}
