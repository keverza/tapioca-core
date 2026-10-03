#include "MeshFixtures.hpp"
#include "SunStudy/SunStudyPreset.hpp"
#include <gtest/gtest.h>
#include <limits>

using namespace evp::sunstudy;
using namespace evptest;

TEST (SunStudyPreset, LightModelRecommendsEarlyAndLargeTopologyRecommendsLate)
{
    EXPECT_NE (SunStudyPresetAdvice (818230, "early").find ("Late model"), std::string::npos);
    EXPECT_NE (SunStudyPresetAdvice (43959, "late").find ("Early model"), std::string::npos);
    EXPECT_TRUE (SunStudyPresetAdvice (818230, "late").empty ());
    EXPECT_TRUE (SunStudyPresetAdvice (43959, "early").empty ());
}

TEST (SunStudyPreset, UnknownMediumAndBoundaryCountsDoNotGiveMisleadingGuidance)
{
    EXPECT_TRUE (SunStudyPresetAdvice (0, "late").empty ());
    EXPECT_TRUE (SunStudyPresetAdvice (100000, "early").empty ());
    EXPECT_TRUE (SunStudyPresetAdvice (100000, "late").empty ());
    EXPECT_TRUE (SunStudyPresetAdvice (249999, "early").empty ());
    EXPECT_FALSE (SunStudyPresetAdvice (250000, "early").empty ());
    EXPECT_FALSE (SunStudyPresetAdvice (50000, "late").empty ());
    EXPECT_TRUE (SunStudyPresetAdvice (50001, "late").empty ());
}

TEST (SunStudyPreset, GlassFacesOnlyAndEveryOtherNonIgnoredFaceRemainsContext)
{
    auto pane = MakeBox ("pane-and-frame", 0, 0, 0);
    pane.triMaterial.assign (12, 1);
    pane.triMaterial[2] = pane.triMaterial[3] = 7;
    const auto scene = MakeSnapshot ({ pane, MakeBox ("wall", 5, 0, 0), MakeBox ("excluded", 10, 0, 0) });
    const auto roles = ResolveElementRoles (scene, {}, {}, { "excluded" });
    const auto result = BuildSunStudyReceivers (scene, roles, true, { { 0, 0.0 }, { 1, 0.0 }, { 7, 0.49 } }, 0.4);
    EXPECT_EQ (result.analysisFaces, 2u);
    EXPECT_EQ (result.contextFaces, 22u);
    EXPECT_EQ (result.roles.analysis, 1u);
    EXPECT_EQ (result.roles.context, 1u);
    EXPECT_EQ (result.roles.ignored, 1u);
    ASSERT_EQ (result.faces[0].size (), 12u);
    EXPECT_EQ (result.faces[0][0], 0u);
    EXPECT_EQ (result.faces[0][2], 1u);
    const auto occluders = OccluderSnapshot (scene, result.roles);
    ASSERT_NE (occluders, nullptr);
    EXPECT_EQ (occluders->TotalTriangles (), 24u); // frame + glass + wall, not just receivers
}

TEST (SunStudyPreset, PickedReceiversAndMissingMaterialNeverWidenToWholeModel)
{
    const auto scene = MakeSnapshot ({ MakeBox ("picked", 0, 0, 0), MakeBox ("other", 5, 0, 0) });
    const auto roles = ResolveElementRoles (scene, { "picked" }, {}, {});
    const auto early = BuildSunStudyReceivers (scene, roles, false, {}, 0.4);
    EXPECT_EQ (early.analysisFaces, 12u);
    EXPECT_EQ (early.contextFaces, 12u);
    const auto late = BuildSunStudyReceivers (scene, roles, true, {}, 0.4);
    EXPECT_EQ (late.analysisFaces, 0u);
    EXPECT_EQ (late.contextFaces, 24u);
    EXPECT_EQ (late.unknownMaterialFaces, 12u);
    EXPECT_EQ (late.roles.context, 2u);
}

TEST (SunStudyPreset, ThresholdIsInclusiveAndNonFiniteTransparencyIsUnknown)
{
    const auto scene = MakeSnapshot ({ MakeBox ("glass", 0, 0, 0) });
    const auto roles = ResolveElementRoles (scene, {}, {}, {});
    EXPECT_EQ (BuildSunStudyReceivers (scene, roles, true, { { 0, 0.4 } }, 0.4).analysisFaces, 12u);
    EXPECT_EQ (BuildSunStudyReceivers (scene, roles, true, { { 0, 0.399 } }, 0.4).analysisFaces, 0u);
    const auto invalid =
        BuildSunStudyReceivers (scene, roles, true, { { 0, std::numeric_limits<double>::quiet_NaN () } }, 0.4);
    EXPECT_EQ (invalid.analysisFaces, 0u);
    EXPECT_EQ (invalid.unknownMaterialFaces, 12u);
}

TEST (SunStudyPreset, ReceiverClassificationUsesTheRetainedSnapshotMaterialPool)
{
    auto captured = MakeSnapshot ({ MakeBox ("glass", 0, 0, 0) });
    captured.materialTransparency = { { 0, 0.7 } };
    auto newer = captured;
    newer.id = captured.id + 1;
    newer.materialTransparency[0] = 0.0; // same numeric index, different modeler's pool
    const auto roles = ResolveElementRoles (captured, {}, {}, {});
    EXPECT_EQ (BuildSunStudyReceivers (captured, roles, true, captured.materialTransparency, 0.4).analysisFaces, 12u);
    EXPECT_EQ (BuildSunStudyReceivers (newer, roles, true, newer.materialTransparency, 0.4).analysisFaces, 0u);
    EXPECT_EQ (captured.materialTransparency.at (0), 0.7);
}
