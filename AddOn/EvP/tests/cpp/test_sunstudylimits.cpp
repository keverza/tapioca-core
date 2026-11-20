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
