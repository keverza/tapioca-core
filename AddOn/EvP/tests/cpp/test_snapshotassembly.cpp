#include "Geometry/SnapshotAssembly.hpp"
#include "Geometry/MeshStore.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

using geomsrv::SnapshotAssembly;

namespace {
std::shared_ptr<const geomsrv::Snapshot> Base ()
{
    SnapshotAssembly assembly (501, 10, { { 1, 0.0 }, { 7, 0.7 } });
    assembly.Add (evptest::MakeBox ("keep", 0, 0, 0));
    assembly.Add (evptest::MakeBox ("edit", 5, 0, 0));
    assembly.Add (evptest::MakeBox ("gone", 10, 0, 0));
    return assembly.Finish (true);
}
} // namespace

TEST (SnapshotAssembly, CompletedFullCaptureOwnsDoubleGeometryAndExactMaterialPool)
{
    const auto snapshot = Base ();
    ASSERT_NE (snapshot, nullptr);
    EXPECT_TRUE (snapshot->completeModel);
    EXPECT_EQ (snapshot->captureStamp, 10u);
    EXPECT_DOUBLE_EQ (snapshot->materialTransparency.at (7), 0.7);
    EXPECT_EQ (snapshot->TotalTriangles (), 36u);
}

TEST (SnapshotAssembly, IncrementalEditCreationAndDeletionPreserveTheImmutableSource)
{
    const auto base = Base ();
    SnapshotAssembly assembly (502, 11, base->materialTransparency, base, { "edit", "gone", "new" });
    assembly.Add (evptest::MakeBox ("edit", 5.5, 0, 0));
    assembly.Add (evptest::MakeBox ("new", 15, 0, 0));
    const auto next = assembly.Finish (true);
    ASSERT_NE (next, nullptr);
    EXPECT_EQ (next->meshes.size (), 3u);
    EXPECT_EQ (next->FindMesh ("gone"), nullptr);
    EXPECT_NE (next->FindMesh ("new"), nullptr);
    EXPECT_EQ (next->FindMesh ("keep")->vertices, base->FindMesh ("keep")->vertices);
    EXPECT_NE (next->FindMesh ("edit")->vertices, base->FindMesh ("edit")->vertices);
    EXPECT_NE (base->FindMesh ("gone"), nullptr);
    EXPECT_EQ (base->captureStamp, 10u);
}

TEST (SnapshotAssembly, MissingRevisionsIncompleteSelectionAndRenumberedPoolsRequireFullCapture)
{
    const auto base = Base ();
    EXPECT_TRUE (SnapshotAssembly::CanUpdate (base.get (), 11, base->materialTransparency));
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 12, base->materialTransparency));
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 10, base->materialTransparency));
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (nullptr, 11, base->materialTransparency));
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 11, { { 1, 0.7 }, { 7, 0.0 } }));
    auto bad = *base;
    bad.completeModel = false;
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (&bad, 11, bad.materialTransparency));
    bad.completeModel = true;
    bad.scope = "selection";
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (&bad, 11, bad.materialTransparency));
    bad.scope = "all";
    bad.captureStamp = 0;
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (&bad, 1, bad.materialTransparency));
}

TEST (SnapshotAssembly, CancelledDuplicateAndUnrequestedMeshesNeverPublishPartialSnapshots)
{
    SnapshotAssembly cancelled (501, 10, {});
    cancelled.Add (evptest::MakeBox ("a", 0, 0, 0));
    EXPECT_EQ (cancelled.Finish (false), nullptr);
    SnapshotAssembly duplicate (501, 10, {});
    duplicate.Add (evptest::MakeBox ("a", 0, 0, 0));
    duplicate.Add (evptest::MakeBox ("a", 5, 0, 0));
    EXPECT_EQ (duplicate.Finish (true), nullptr);
    const auto base = Base ();
    SnapshotAssembly update (502, 11, base->materialTransparency, base, { "edit" });
    update.Add (evptest::MakeBox ("unrequested", 5, 0, 0));
    EXPECT_EQ (update.Finish (true), nullptr);
}

TEST (SnapshotAssembly, SharedPublicationDoesNotReplaceASelectionAndIdsDoNotResetOnRelease)
{
    auto& store = geomsrv::MeshStore::Get ();
    store.Release ();
    auto selected = std::make_shared<geomsrv::Snapshot> ();
    selected->scope = "selection";
    store.Publish (selected);
    const auto id = store.NextId ();
    const auto shared = Base ();
    store.PublishShared (shared);
    EXPECT_EQ (store.Current (), selected);
    EXPECT_EQ (store.Shared (), shared);
    EXPECT_GT (store.Bytes (), shared->Bytes ());
    store.Release ();
    EXPECT_EQ (store.Shared (), nullptr);
    EXPECT_EQ (store.Current (), nullptr);
    EXPECT_GT (store.NextId (), id);
}

TEST (SnapshotAssembly, OriginalSurfaceSidecarAndCatalogSurviveFullAndIncrementalCaptures)
{
    const std::map<int32_t, geomsrv::CapturedMaterialIdentity> materials { { 7, { "Pool A", 501 } } };
    const std::map<int32_t, geomsrv::CapturedSurface> surfaces { { 101, { "Finish A", "surface-guid" } } };
    SnapshotAssembly full (600, 20, { { 7, 0 } }, nullptr, {}, materials, surfaces);
    auto mesh = evptest::MakeBox ("keep", 0, 0, 0);
    mesh.triSurfaceAttribute.assign (12, 101);
    full.Add (mesh);
    const auto base = full.Finish (true);
    ASSERT_NE (base, nullptr);
    EXPECT_EQ (base->materialIdentity, materials);
    EXPECT_EQ (base->surfaceAttributes, surfaces);
    EXPECT_TRUE (SnapshotAssembly::CanUpdate (base.get (), 21, base->materialTransparency, materials, surfaces));
    SnapshotAssembly next (601, 21, base->materialTransparency, base, { "new" }, materials, surfaces);
    next.Add (evptest::MakeBox ("new", 5, 0, 0));
    const auto updated = next.Finish (true);
    ASSERT_NE (updated, nullptr);
    EXPECT_EQ (updated->FindMesh ("keep")->triSurfaceAttribute, mesh.triSurfaceAttribute);
    EXPECT_EQ (updated->surfaceAttributes, surfaces);
    auto renamed = materials;
    renamed[7].name = "Different pool";
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 21, base->materialTransparency, renamed, surfaces));
    auto replaced = surfaces;
    replaced[101].guid = "replacement-attribute-guid";
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 21, base->materialTransparency, materials, replaced));
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 21, base->materialTransparency));
}

// A whole-model caller waits on this flag instead of walking the model on the
// main thread (#97), so it must clear on every return path of a full pass.
TEST (MeshStoreFullPass, AScopeMarksAFullWalkAndClearsOnExit)
{
    auto& store = geomsrv::MeshStore::Get ();
    ASSERT_FALSE (store.FullPassInFlight ());
    {
        const geomsrv::MeshStore::FullPassScope pass (true);
        EXPECT_TRUE (store.FullPassInFlight ());
    }
    EXPECT_FALSE (store.FullPassInFlight ());
    {
        const geomsrv::MeshStore::FullPassScope update (false);
        EXPECT_FALSE (store.FullPassInFlight ());
        store.SetFullPassInFlight (true); // a by-GUID pass that fell back to a full walk
        EXPECT_TRUE (store.FullPassInFlight ());
    }
    EXPECT_FALSE (store.FullPassInFlight ());
}

// One element's new surface grew the pool 69 -> 70 and forced a 48 s full pass
// (2026-10-31, #97). A pool that only grew keeps every kept element's indices.
TEST (SnapshotAssembly, APoolThatOnlyGrewStillUpdatesARenumberedOneDoesNot)
{
    const auto base = Base ();
    auto grown = base->materialTransparency;
    grown[9] = 0.3;
    EXPECT_TRUE (SnapshotAssembly::CanUpdate (base.get (), 11, grown));
    auto shifted = base->materialTransparency;
    shifted.erase (7);
    shifted[8] = 0.7;
    EXPECT_FALSE (SnapshotAssembly::CanUpdate (base.get (), 11, shifted)) << "an entry the base held moved";

    const std::map<int32_t, geomsrv::CapturedMaterialIdentity> materials { { 7, { "Pool A", 501 } } };
    const std::map<int32_t, geomsrv::CapturedSurface> surfaces { { 101, { "Finish A", "surface-guid" } } };
    SnapshotAssembly full (700, 30, { { 7, 0 } }, nullptr, {}, materials, surfaces);
    full.Add (evptest::MakeBox ("keep", 0, 0, 0));
    const auto withPool = full.Finish (true);
    ASSERT_NE (withPool, nullptr);
    auto moreMaterials = materials;
    moreMaterials[8] = { "Pool B", 502 };
    auto moreSurfaces = surfaces;
    moreSurfaces[102] = { "Finish B", "surface-guid-b" };
    EXPECT_TRUE (
        SnapshotAssembly::CanUpdate (withPool.get (), 31, withPool->materialTransparency, moreMaterials, moreSurfaces));
}
