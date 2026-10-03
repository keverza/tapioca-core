#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudySelectionBinding.hpp"
#include <gtest/gtest.h>

using namespace evp::sunstudy;

TEST (SunStudyReceivers, CoplanarConnectedGlassAndOpaqueFacesRemainSeparate)
{
    const std::vector<double> vertices { 0, 0, 0, 2, 0, 0, 4, 0, 0, 0, 2, 0, 2, 2, 0, 4, 2, 0 };
    const std::vector<uint32_t> triangles { 0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4 };
    const std::vector<uint8_t> mask { 1, 1, 0, 0 };
    PatchSamplerOptions options;
    options.spacing = 0.5;
    const auto all =
        BuildPatchSampleGrid (vertices.data (), 6, triangles.data (), 4, nullptr, { "pane-frame" }, options);
    ASSERT_EQ (all.spans.size (), 1u);
    options.sampleFace = &mask;
    const auto glass =
        BuildPatchSampleGrid (vertices.data (), 6, triangles.data (), 4, nullptr, { "pane-frame" }, options);
    ASSERT_TRUE (glass.valid);
    ASSERT_EQ (glass.spans.size (), 1u);
    EXPECT_DOUBLE_EQ (glass.spans[0].area, 4.0);
    EXPECT_EQ (glass.Count (), 16u);
    EXPECT_EQ (glass.excludedPatches, 1u);
    EXPECT_EQ (glass.patchOfTriangle,
               (std::vector<uint32_t> { 0, 0, PatchSampleGrid::kNoPatch, PatchSampleGrid::kNoPatch }));
    for (size_t sample = 0; sample < glass.Count (); ++sample)
        EXPECT_LT (glass.positions[sample * 3], 2.0);
}

TEST (SunStudyReceivers, LiveAnalysisClearAndDetachRetainAnExplicitEmptyScope)
{
    SunStudySelectionBinding binding;
    binding.analysisSet = "Analysis";
    binding.generation = 3;
    std::vector<std::string> analysis { "glass" }, context, ignored;
    EXPECT_EQ (binding.Refresh (3, {}, {}, context, ignored, { "{GLASS}" }, &analysis),
               SelectionBindingRefresh::Unchanged);
    EXPECT_EQ (binding.Refresh (3, {}, {}, context, ignored, {}, &analysis), SelectionBindingRefresh::Changed);
    EXPECT_TRUE (analysis.empty ());
    EXPECT_EQ (binding.Refresh (4, {}, {}, context, ignored, { "other" }, &analysis),
               SelectionBindingRefresh::Detached);
    EXPECT_TRUE (analysis.empty ()); // caller's analysisRestricted remains set, preventing a wider rerun
    EXPECT_EQ (binding.generation, 0u);
    EXPECT_TRUE (binding.analysisSet.empty ());
}
