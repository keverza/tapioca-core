// Tests for SunStudy/SunStudyRoles -- which elements a study measures, which
// only cast shadow, and which it leaves out.
//
// ⚠️ EVERY FAILURE HERE IS A PLAUSIBLE STUDY. A context building resolved as
// IGNORED stops casting shadow and the courtyard reads as sunlit; an analysis
// element resolved as context simply has no colour, which looks like "not
// selected" rather than like a fault. The table in the header is the contract,
// so each row of it is a test.

#include <vector>

#include "SunStudy/SunStudyRoles.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

const std::vector<std::string> kElements { "{AAAA-1}", "{BBBB-2}", "{CCCC-3}" };

} // namespace

TEST (SunStudyRoles, NamingNothingAnalysesEverything)
{
    const ElementRoles roles = ResolveElementRoles (kElements, {}, {});
    EXPECT_EQ (roles.analysis, 3u);
    EXPECT_FALSE (roles.RestrictsOccluders ());
    EXPECT_EQ (roles.SampleMask (), (std::vector<uint8_t> { 1, 1, 1 }));
}

TEST (SunStudyRoles, NamingOnlyContextAnalysesEverythingElse)
{
    const ElementRoles roles = ResolveElementRoles (kElements, {}, { "{BBBB-2}" });
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Context);
    EXPECT_EQ (roles.roles[2], ElementRole::Analysis);
    EXPECT_FALSE (roles.RestrictsOccluders ());
}

TEST (SunStudyRoles, NamingOnlyAnalysisKeepsEveryOtherElementCastingShadow)
{
    // The case a person reaches for first: "study this facade". The rest of
    // the model must go on shading it.
    const ElementRoles roles = ResolveElementRoles (kElements, { "{AAAA-1}" }, {});
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Context);
    EXPECT_EQ (roles.roles[2], ElementRole::Context);
    EXPECT_EQ (roles.ignored, 0u);
    EXPECT_FALSE (roles.RestrictsOccluders ());
    EXPECT_EQ (roles.SampleMask (), (std::vector<uint8_t> { 1, 0, 0 }));
}

TEST (SunStudyRoles, NamingBothIgnoresTheRestAndRestrictsTheOccluders)
{
    const ElementRoles roles = ResolveElementRoles (kElements, { "{AAAA-1}" }, { "{BBBB-2}" });
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Context);
    EXPECT_EQ (roles.roles[2], ElementRole::Ignored);
    EXPECT_TRUE (roles.RestrictsOccluders ());
}

TEST (SunStudyRoles, AnElementInBothListsIsAnalysed)
{
    const ElementRoles roles = ResolveElementRoles (kElements, { "{AAAA-1}" }, { "{AAAA-1}", "{BBBB-2}" });
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Context);
}

TEST (SunStudyRoles, GuidsMatchAcrossBracesAndCase)
{
    const ElementRoles roles = ResolveElementRoles (kElements, { "aaaa-1" }, { "{bbbb-2}" });
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Context);
    EXPECT_EQ (roles.unmatchedAnalysis, 0u);
    EXPECT_EQ (roles.unmatchedContext, 0u);
}

TEST (SunStudyRoles, PickedElementsTheSnapshotDoesNotHoldAreCountedNotDropped)
{
    const ElementRoles roles = ResolveElementRoles (kElements, { "{AAAA-1}", "{GONE-9}" }, { "{GONE-8}" });
    EXPECT_EQ (roles.unmatchedAnalysis, 1u);
    EXPECT_EQ (roles.unmatchedContext, 1u);
    EXPECT_FALSE (roles.analysisNamedButAbsent);
}

TEST (SunStudyRoles, TheOccludersDropOnlyTheIgnoredAndKeepTheSnapshotsIdentity)
{
    geomsrv::Snapshot snapshot;
    snapshot.id = 41;
    for (const std::string& guid : kElements) {
        geomsrv::Mesh mesh;
        mesh.guid = guid;
        snapshot.meshes.push_back (mesh);
    }

    // Nothing ignored: no subset, the caller keeps the cached BVH.
    EXPECT_EQ (OccluderSnapshot (snapshot, ResolveElementRoles (snapshot, { "{AAAA-1}" }, {})), nullptr);

    // Both lists named: the third element is ignored and leaves the occluders.
    const ElementRoles roles = ResolveElementRoles (snapshot, { "{AAAA-1}" }, { "{BBBB-2}" });
    const auto subset = OccluderSnapshot (snapshot, roles);
    ASSERT_NE (subset, nullptr);
    EXPECT_EQ (subset->id, 41u) << "the subset claims to be a different model state";
    ASSERT_EQ (subset->meshes.size (), 2u);
    EXPECT_EQ (subset->meshes[0].guid, "{AAAA-1}");
    EXPECT_EQ (subset->meshes[1].guid, "{BBBB-2}");
}

TEST (SunStudyRoles, AnIgnoredElementNeitherCastsShadowNorIsMeasured)
{
    // "What would this look like without the tree?" -- nothing deleted.
    const ElementRoles roles = ResolveElementRoles (kElements, {}, {}, { "{CCCC-3}" });
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[2], ElementRole::Ignored);
    EXPECT_TRUE (roles.RestrictsOccluders ());
}

TEST (SunStudyRoles, TheIgnoredListWinsOverAnalysisAndContext)
{
    const ElementRoles roles =
        ResolveElementRoles (kElements, { "{AAAA-1}", "{BBBB-2}" }, { "{CCCC-3}" }, { "{BBBB-2}", "{CCCC-3}" });
    EXPECT_EQ (roles.roles[0], ElementRole::Analysis);
    EXPECT_EQ (roles.roles[1], ElementRole::Ignored);
    EXPECT_EQ (roles.roles[2], ElementRole::Ignored);
}

TEST (SunStudyRoles, AnAnalysisListThatIsAllIgnoredIsRefusedNotWidened)
{
    // ⚠️ THE TRAP: thinning the analysis list by the ignore list leaves it
    // empty, and "empty" means "analyse everything". A NAMED list stays named.
    const ElementRoles roles = ResolveElementRoles (kElements, { "{AAAA-1}" }, {}, { "{AAAA-1}" });
    EXPECT_TRUE (roles.analysisNamedButAbsent);
    EXPECT_EQ (roles.analysis, 0u);
    EXPECT_EQ (roles.roles[1], ElementRole::Context);
}

TEST (SunStudyRoles, AnIgnoredElementTheModelDoesNotHoldIsCounted)
{
    const ElementRoles roles = ResolveElementRoles (kElements, {}, {}, { "{GONE-7}" });
    EXPECT_EQ (roles.unmatchedIgnored, 1u);
    EXPECT_FALSE (roles.RestrictsOccluders ()) << "a missing element was ignored out of a model it is not in";
}

TEST (SunStudyRoles, AnAnalysisListWithNothingInTheModelIsNotAStudyOfEverything)
{
    // ⚠️ THE FAILURE THIS PREVENTS: an empty intersection must not fall back
    // to "analyse everything", which is what an empty LIST means.
    const ElementRoles roles = ResolveElementRoles (kElements, { "{GONE-9}" }, {});
    EXPECT_TRUE (roles.analysisNamedButAbsent);
    EXPECT_EQ (roles.analysis, 0u);
}
