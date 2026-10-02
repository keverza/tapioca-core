#include "SunStudy/SunStudyOccluders.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>
#include <algorithm>

using namespace evp::sunstudy;

namespace {
geomsrv::Snapshot Scene ()
{
    geomsrv::Snapshot snapshot;
    snapshot.id = 21;
    snapshot.meshes = { evptest::MakeBox ("analysis", 4, 0, 5), evptest::MakeBox ("context", 0, 0, 5),
                        evptest::MakeBox ("ignored", 8, 0, 5) };
    return snapshot;
}
ElementRoles Roles (const geomsrv::Snapshot& snapshot)
{
    return ResolveElementRoles (snapshot, {}, { "context" }, { "ignored" });
}
} // namespace

TEST (SunStudyOccluders, UnchangedContextSurvivesAnalysisEditsAndReorderedCanonicalGuids)
{
    auto snapshot = Scene ();
    const auto first = BuildSunStudyOccluders (snapshot, Roles (snapshot));
    ASSERT_NE (first, nullptr);
    EXPECT_FALSE (first->contextReused);
    EXPECT_EQ (first->context->TriangleCount (), 12u);
    snapshot.id = 22;
    snapshot.meshes[0].vertices[0] += 2.0;
    snapshot.meshes[1].guid = " {Context} ";
    std::reverse (snapshot.meshes.begin (), snapshot.meshes.end ());
    const auto next = BuildSunStudyOccluders (snapshot, Roles (snapshot), first.get ());
    ASSERT_NE (next, nullptr);
    EXPECT_TRUE (next->contextReused);
    EXPECT_EQ (next->context, first->context);
    EXPECT_EQ (next->contextSnapshot, first->contextSnapshot);
    EXPECT_NE (next->analysis, first->analysis);
    EXPECT_EQ (next->analysis->SnapshotId (), 22u);
}

TEST (SunStudyOccluders, MaterialAndIgnoredGeometryChangesDoNotRebuildContext)
{
    auto snapshot = Scene ();
    const auto first = BuildSunStudyOccluders (snapshot, Roles (snapshot));
    snapshot.meshes[1].triMaterial.assign (12, 91);
    snapshot.meshes[1].normals.assign (24, 0.25);
    snapshot.meshes[2].vertices[0] += 100.0;
    const auto next = BuildSunStudyOccluders (snapshot, Roles (snapshot), first.get ());
    ASSERT_NE (next, nullptr);
    EXPECT_EQ (next->context, first->context);
    EXPECT_TRUE (next->contextReused);
}

TEST (SunStudyOccluders, ChangedVerticesOrIndicesInvalidateContext)
{
    const auto snapshot = Scene ();
    const auto first = BuildSunStudyOccluders (snapshot, Roles (snapshot));
    for (bool indices : { false, true }) {
        auto changed = snapshot;
        if (indices)
            std::swap (changed.meshes[1].triangles[0], changed.meshes[1].triangles[1]);
        else
            changed.meshes[1].vertices[0] += 0.0001;
        const auto next = BuildSunStudyOccluders (changed, Roles (changed), first.get ());
        ASSERT_NE (next, nullptr);
        EXPECT_FALSE (next->contextReused);
        EXPECT_NE (next->context, first->context);
    }
}

TEST (SunStudyOccluders, ContextAddDeleteAndRoleChangesInvalidateMembership)
{
    const auto original = Scene ();
    const auto first = BuildSunStudyOccluders (original, Roles (original));
    auto added = original;
    added.meshes.push_back (evptest::MakeBox ("new-context", 12, 0, 5));
    auto roles = ResolveElementRoles (added, {}, { "context", "new-context" }, { "ignored" });
    const auto next = BuildSunStudyOccluders (added, roles, first.get ());
    ASSERT_NE (next, nullptr);
    EXPECT_FALSE (next->contextReused);
    EXPECT_EQ (next->context->MeshCount (), 2u);
    roles = ResolveElementRoles (original, {}, { "analysis", "context" }, { "ignored" });
    const auto changedRole = BuildSunStudyOccluders (original, roles, first.get ());
    ASSERT_NE (changedRole, nullptr);
    EXPECT_FALSE (changedRole->contextReused);
    EXPECT_EQ (changedRole->context->MeshCount (), 2u);
    const auto removed = BuildSunStudyOccluders (original, Roles (original), changedRole.get ());
    ASSERT_NE (removed, nullptr);
    EXPECT_FALSE (removed->contextReused);
    EXPECT_EQ (removed->context->MeshCount (), 1u);
}

TEST (SunStudyOccluders, ContextAndIgnoredOnlyAutomaticallyAnalyseNewElements)
{
    auto snapshot = Scene ();
    const auto first = BuildSunStudyOccluders (snapshot, Roles (snapshot));
    snapshot.id = 22;
    snapshot.meshes.push_back (evptest::MakeBox ("new-analysis", 12, 0, 5));
    const auto roles = Roles (snapshot);
    EXPECT_EQ (roles.analysis, 2u);
    EXPECT_EQ (roles.context, 1u);
    EXPECT_EQ (roles.ignored, 1u);
    EXPECT_EQ (roles.SampleMask (), (std::vector<uint8_t> { 1, 0, 0, 1 }));
    const auto next = BuildSunStudyOccluders (snapshot, roles, first.get ());
    ASSERT_NE (next, nullptr);
    EXPECT_TRUE (next->contextReused);
    EXPECT_EQ (next->analysis->MeshCount (), 2u);
    EXPECT_EQ (next->analysis->TriangleCount (), 24u);
    SunStudyPartitionTraversal traversal (next->analysis, next->context);
    const double newElement[3] = { 12.25, 0.25, 0 }, ignored[3] = { 8.25, 0.25, 0 }, up[3] = { 0, 0, 1 };
    EXPECT_TRUE (traversal.Occluded (newElement, up, 0.001, 0.0));
    EXPECT_FALSE (traversal.Occluded (ignored, up, 0.001, 0.0));
    EXPECT_EQ (traversal.SceneVersion (), 22u);
}

TEST (SunStudyOccluders, PartitionedCpuQueriesMatchCombinedRoleFilteredScene)
{
    const auto snapshot = Scene ();
    const auto roles = Roles (snapshot);
    const auto parts = BuildSunStudyOccluders (snapshot, roles);
    SunStudyPartitionTraversal cpu (parts->analysis, parts->context);
    CpuTraversal combined (std::make_shared<geomsrv::QueryEngine> (OccluderSnapshot (snapshot, roles)));
    const double positions[] = { 0.25, 0.25, 0, 4.25, 0.25, 0, 8.25, 0.25, 0, 100, 100, 0 };
    const double up[] = { 0, 0, 1 };
    uint8_t a[4] {}, b[4] {};
    cpu.OccludeDirectional (positions, 4, up, 0.001, 0.0, a, 1);
    combined.OccludeDirectional (positions, 4, up, 0.001, 0.0, b, 1);
    EXPECT_EQ (std::vector<uint8_t> (a, a + 4), (std::vector<uint8_t> { 1, 1, 0, 0 }));
    EXPECT_EQ (std::vector<uint8_t> (a, a + 4), std::vector<uint8_t> (b, b + 4));
    OcclusionRay rays[4];
    for (size_t i = 0; i < 4; ++i)
        std::copy_n (&positions[i * 3], 3, rays[i].origin);
    cpu.OccludeRays (rays, 4, a, 1);
    combined.OccludeRays (rays, 4, b, 1);
    EXPECT_EQ (std::vector<uint8_t> (a, a + 4), std::vector<uint8_t> (b, b + 4));
}

TEST (SunStudyOccluders, AmbiguousGuidMembershipAndCancellationNeverSeedStaleContext)
{
    auto snapshot = Scene ();
    const auto first = BuildSunStudyOccluders (snapshot, Roles (snapshot));
    snapshot.meshes.push_back (snapshot.meshes[1]);
    const auto next = BuildSunStudyOccluders (snapshot, Roles (snapshot), first.get ());
    ASSERT_NE (next, nullptr);
    EXPECT_FALSE (next->contextReused);
    EXPECT_EQ (next->context->MeshCount (), 2u);
    EXPECT_EQ (BuildSunStudyOccluders (snapshot, Roles (snapshot), first.get (), [] { return true; }), nullptr);
    EXPECT_EQ (first->context->MeshCount (), 1u);
    EXPECT_EQ (BuildSunStudyOccluders (snapshot, {}, first.get ()), nullptr);
}
