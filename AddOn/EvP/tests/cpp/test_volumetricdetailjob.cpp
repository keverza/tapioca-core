#include "Edit/EditVolumetricDetailJob.hpp"

#include "Edit/EditLandscape.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;

namespace {

struct StoreReset {
    StoreReset ()
    {
        lg::EditorMeshStore::Get ().Release ();
    }
    ~StoreReset ()
    {
        lg::EditorMeshStore::Get ().Release ();
    }
};

edit::Document RockDocument (lg::Id& id, bool cave = false)
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1, 2, 3 }, error)) << error;
    EXPECT_TRUE (lg::AddVolumetricDetail (doc, cave ? lg::MakeVolumetricCavePreset () : lg::MakeVolumetricRockPreset (),
                                          id, error))
        << error;
    return doc;
}

std::shared_ptr<lg::PublishedTerrain> FlatTerrain (const edit::Document& doc, uint64_t revision = 7, double height = 2)
{
    auto terrain = std::make_shared<lg::PublishedTerrain> ();
    terrain->points = { 0, 0, height, 10, 0, height, 10, 10, height, 0, 10, height };
    terrain->triangles = { 0, 1, 2, 0, 2, 3 };
    terrain->revision = revision;
    terrain->session = doc.landscape->sessionUuid;
    terrain->sourceTerrainGuid = doc.landscape->sourceTerrainGuid;
    return terrain;
}

lg::VolumetricDetailOutcome RunSuccessfully (const lg::VolumetricDetailJob& job)
{
    auto outcome = lg::RunVolumetricDetail (job);
    EXPECT_TRUE (outcome.built) << outcome.error;
    EXPECT_NE (outcome.mesh, nullptr);
    return outcome;
}

std::shared_ptr<const lg::PublishedMesh> Stored (lg::Id id)
{
    return lg::EditorMeshStore::Get ().Get ("volumetric/" + std::to_string (id));
}

} // namespace

TEST (VolumetricDetailJob, PreviewAndFinalShareDefinitionAndOnlyChangeVoxelSize)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id);
    lg::VolumetricDetailPublisher publisher;
    lg::VolumetricDetailRequest request;
    request.detail = id;
    request.level = lg::VolumetricBuildLevel::Preview;
    request.options.maxTriangles = 100000;
    lg::VolumetricDetailJob previewJob;
    std::string error;
    ASSERT_TRUE (publisher.Prepare (doc, request, previewJob, error)) << error;
    auto preview = RunSuccessfully (previewJob);
    ASSERT_TRUE (publisher.Publish (doc, nullptr, preview, error)) << error;

    request.level = lg::VolumetricBuildLevel::Final;
    lg::VolumetricDetailJob finalJob;
    ASSERT_TRUE (publisher.Prepare (doc, request, finalJob, error)) << error;
    auto final = RunSuccessfully (finalJob);

    EXPECT_EQ (previewJob.detail, finalJob.detail);
    EXPECT_EQ (preview.definitionHash, final.definitionHash);
    EXPECT_EQ (previewJob.translation, finalJob.translation);
    EXPECT_EQ (previewJob.options.strategy, finalJob.options.strategy);
    EXPECT_EQ (previewJob.options.maxTriangles, finalJob.options.maxTriangles);
    EXPECT_EQ (previewJob.options.soloOperation, finalJob.options.soloOperation);
    EXPECT_DOUBLE_EQ (preview.voxelSize, 2 * final.voxelSize);
    EXPECT_LT (preview.diagnostics.triangles, final.diagnostics.triangles);
    const std::set<int32_t> previewMaterials (preview.mesh->triMaterial.begin (), preview.mesh->triMaterial.end ());
    const std::set<int32_t> finalMaterials (final.mesh->triMaterial.begin (), final.mesh->triMaterial.end ());
    EXPECT_EQ (previewMaterials, finalMaterials);

    ASSERT_TRUE (publisher.Publish (doc, nullptr, final, error)) << error;
    ASSERT_NE (Stored (id), nullptr);
    EXPECT_EQ (Stored (id)->positions, final.mesh->positions);
    ASSERT_NE (publisher.LastPublished (), nullptr);
    EXPECT_EQ (publisher.LastPublished ()->level, lg::VolumetricBuildLevel::Final);
}

TEST (VolumetricDetailJob, LastGoodSurvivesBudgetCancellationAndInvalidOutput)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id);
    lg::VolumetricDetailPublisher publisher;
    lg::VolumetricDetailRequest request;
    request.detail = id;
    lg::VolumetricDetailJob job;
    std::string error;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto good = RunSuccessfully (job);
    ASSERT_TRUE (publisher.Publish (doc, nullptr, good, error)) << error;
    const auto lastGood = Stored (id);
    ASSERT_NE (lastGood, nullptr);

    request.options.maxTriangles = 1;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto refused = lg::RunVolumetricDetail (job);
    ASSERT_FALSE (refused.built);
    ASSERT_TRUE (publisher.Publish (doc, nullptr, refused, error)) << error;
    EXPECT_EQ (Stored (id), lastGood);
    ASSERT_NE (publisher.LatestOutcome (), nullptr);
    EXPECT_FALSE (publisher.LatestOutcome ()->error.empty ());

    request.options.maxTriangles = 100000;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto completed = RunSuccessfully (job);
    publisher.Cancel ();
    EXPECT_FALSE (publisher.Publish (doc, nullptr, completed, error));
    EXPECT_EQ (Stored (id), lastGood);

    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto invalid = RunSuccessfully (job);
    auto changed = *invalid.mesh;
    changed.valid = false;
    invalid.mesh = std::make_shared<const lg::PublishedMesh> (std::move (changed));
    EXPECT_FALSE (publisher.Publish (doc, nullptr, invalid, error));
    EXPECT_EQ (Stored (id), lastGood);

    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto invalidBounds = RunSuccessfully (job);
    auto changedBounds = *invalidBounds.mesh;
    changedBounds.bounds = {};
    invalidBounds.mesh = std::make_shared<const lg::PublishedMesh> (std::move (changedBounds));
    EXPECT_FALSE (publisher.Publish (doc, nullptr, invalidBounds, error));
    EXPECT_EQ (Stored (id), lastGood);

    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    job.cancel->store (true);
    auto cancelled = lg::RunVolumetricDetail (job);
    EXPECT_TRUE (cancelled.cancelled);
    EXPECT_FALSE (cancelled.built);
    EXPECT_EQ (cancelled.mesh, nullptr);
    EXPECT_FALSE (publisher.Publish (doc, nullptr, cancelled, error));
    EXPECT_EQ (Stored (id), lastGood);
}

TEST (VolumetricDetailJob, SupersededAndStaleCompletionNeverRetargetPublication)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id);
    lg::VolumetricDetailPublisher publisher;
    lg::VolumetricDetailRequest request;
    request.detail = id;
    lg::VolumetricDetailJob firstJob;
    std::string error;
    ASSERT_TRUE (publisher.Prepare (doc, request, firstJob, error)) << error;
    auto first = RunSuccessfully (firstJob);
    ASSERT_TRUE (publisher.Publish (doc, nullptr, first, error)) << error;
    const auto lastGood = Stored (id);

    lg::VolumetricDetailJob olderJob;
    ASSERT_TRUE (publisher.Prepare (doc, request, olderJob, error)) << error;
    auto older = RunSuccessfully (olderJob);
    lg::VolumetricDetailJob newerJob;
    ASSERT_TRUE (publisher.Prepare (doc, request, newerJob, error)) << error;
    EXPECT_FALSE (publisher.Publish (doc, nullptr, older, error));
    EXPECT_EQ (Stored (id), lastGood);

    auto edited = doc.landscape->volumetricDetails.front ();
    edited.voxelSize = 0.1;
    ASSERT_TRUE (lg::SetVolumetricDetail (doc, edited, error)) << error;
    auto newer = RunSuccessfully (newerJob);
    EXPECT_FALSE (publisher.Publish (doc, nullptr, newer, error));
    EXPECT_EQ (Stored (id), lastGood);

    lg::VolumetricDetailJob currentJob;
    ASSERT_TRUE (publisher.Prepare (doc, request, currentJob, error)) << error;
    lg::VolumetricDetailRequest missing = request;
    missing.detail = id + 1000;
    lg::VolumetricDetailJob ignored;
    EXPECT_FALSE (publisher.Prepare (doc, missing, ignored, error));
    auto current = RunSuccessfully (currentJob);
    EXPECT_TRUE (publisher.Publish (doc, nullptr, current, error)) << error;
    EXPECT_EQ (Stored (id)->positions, current.mesh->positions);
}

TEST (VolumetricDetailJob, TerrainAnchorUsesExactSnapshotAndMarksChangesStale)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id);
    auto terrain = FlatTerrain (doc);
    const auto terrainBefore = *terrain;
    auto detail = doc.landscape->volumetricDetails.front ();
    detail.anchor = lg::VolumetricTerrainAnchor { terrain->revision, lg::HashVolumetricTerrainGeometry (*terrain),
                                                   5.0, 5.0, 0.5 };
    std::string error;
    ASSERT_TRUE (lg::SetVolumetricDetail (doc, detail, error)) << error;

    lg::VolumetricDetailPublisher publisher;
    lg::VolumetricDetailRequest request;
    request.detail = id;
    request.terrain = terrain;
    lg::VolumetricDetailJob job;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    EXPECT_DOUBLE_EQ (job.translation.x, 5.0);
    EXPECT_DOUBLE_EQ (job.translation.y, 5.0);
    EXPECT_DOUBLE_EQ (job.translation.z, 2.5);
    auto anchored = RunSuccessfully (job);
    ASSERT_TRUE (publisher.Publish (doc, terrain, anchored, error)) << error;
    EXPECT_EQ (*terrain, terrainBefore);

    lg::SdfBuildResult unanchored;
    ASSERT_TRUE (lg::BuildSdf (detail, detail.voxelSize, {}, unanchored, error)) << error;
    ASSERT_EQ (anchored.mesh->positions.size (), unanchored.mesh.positions.size ());
    for (size_t index = 0; index < anchored.mesh->positions.size (); index += 3) {
        EXPECT_DOUBLE_EQ (anchored.mesh->positions[index], unanchored.mesh.positions[index] + 5.0);
        EXPECT_DOUBLE_EQ (anchored.mesh->positions[index + 1], unanchored.mesh.positions[index + 1] + 5.0);
        EXPECT_DOUBLE_EQ (anchored.mesh->positions[index + 2], unanchored.mesh.positions[index + 2] + 2.5);
    }
    EXPECT_EQ (anchored.mesh->triangles, unanchored.mesh.triangles);
    EXPECT_EQ (anchored.mesh->normals, unanchored.mesh.normals);

    auto changedTerrain = std::make_shared<lg::PublishedTerrain> (*terrain);
    changedTerrain->points[2] += 1.0;
    EXPECT_TRUE (lg::VolumetricAnchorStale (detail, changedTerrain.get ()));
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto completed = RunSuccessfully (job);
    EXPECT_FALSE (publisher.Publish (doc, changedTerrain, completed, error));
    EXPECT_EQ (Stored (id)->positions, anchored.mesh->positions);
    request.terrain = changedTerrain;
    EXPECT_FALSE (publisher.Prepare (doc, request, job, error));

    request.terrain = terrain;
    detail = doc.landscape->volumetricDetails.front ();
    detail.anchor->x = 50.0;
    ASSERT_TRUE (lg::SetVolumetricDetail (doc, detail, error)) << error;
    EXPECT_FALSE (publisher.Prepare (doc, request, job, error));
    EXPECT_NE (error.find ("VolumetricAnchorOutside"), std::string::npos);
}

TEST (VolumetricDetailJob, LargeFiniteAnchorOffsetRefusesCollapsedGeometry)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id);
    lg::VolumetricDetailPublisher publisher;
    lg::VolumetricDetailRequest request;
    request.detail = id;
    lg::VolumetricDetailJob job;
    std::string error;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto good = RunSuccessfully (job);
    ASSERT_TRUE (publisher.Publish (doc, nullptr, good, error)) << error;
    const auto lastGood = Stored (id);

    auto terrain = FlatTerrain (doc);
    auto detail = doc.landscape->volumetricDetails.front ();
    detail.anchor = lg::VolumetricTerrainAnchor { terrain->revision, lg::HashVolumetricTerrainGeometry (*terrain),
                                                   5.0, 5.0, 1e20 };
    ASSERT_TRUE (lg::SetVolumetricDetail (doc, detail, error)) << error;
    request.terrain = terrain;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto collapsed = lg::RunVolumetricDetail (job);
    EXPECT_FALSE (collapsed.built);
    EXPECT_EQ (collapsed.mesh, nullptr);
    EXPECT_NE (collapsed.error.find ("collapses"), std::string::npos);
    ASSERT_TRUE (publisher.Publish (doc, terrain, collapsed, error)) << error;
    EXPECT_EQ (Stored (id), lastGood);
}

TEST (VolumetricDetailJob, IndependentPublishersCannotOverwriteANewerGlobalRequest)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id);
    lg::VolumetricDetailPublisher previewPublisher;
    lg::VolumetricDetailPublisher finalPublisher;
    lg::VolumetricDetailRequest previewRequest;
    previewRequest.detail = id;
    previewRequest.level = lg::VolumetricBuildLevel::Preview;
    lg::VolumetricDetailJob previewJob;
    std::string error;
    ASSERT_TRUE (previewPublisher.Prepare (doc, previewRequest, previewJob, error)) << error;

    lg::VolumetricDetailRequest finalRequest;
    finalRequest.detail = id;
    lg::VolumetricDetailJob finalJob;
    ASSERT_TRUE (finalPublisher.Prepare (doc, finalRequest, finalJob, error)) << error;
    auto final = RunSuccessfully (finalJob);
    ASSERT_TRUE (finalPublisher.Publish (doc, nullptr, final, error)) << error;
    ASSERT_EQ (Stored (id)->positions, final.mesh->positions);

    auto slowPreview = RunSuccessfully (previewJob);
    EXPECT_FALSE (previewPublisher.Publish (doc, nullptr, slowPreview, error));
    EXPECT_EQ (Stored (id)->positions, final.mesh->positions);
}

TEST (VolumetricDetailJob, AsyncFuturePublishesCompleteOwnedResult)
{
    StoreReset reset;
    lg::Id id = 0;
    auto doc = RockDocument (id, true);
    lg::VolumetricDetailPublisher publisher;
    lg::VolumetricDetailRequest request;
    request.detail = id;
    lg::VolumetricDetailJob job;
    std::string error;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto future = lg::LaunchVolumetricDetail (job);
    auto outcome = future.get ();
    ASSERT_TRUE (outcome.built) << outcome.error;
    ASSERT_TRUE (publisher.Publish (doc, nullptr, outcome, error)) << error;
    ASSERT_NE (Stored (id), nullptr);
    EXPECT_EQ (Stored (id)->positions, outcome.mesh->positions);
    EXPECT_TRUE (std::all_of (Stored (id)->triMaterial.begin (), Stored (id)->triMaterial.end (),
                              [] (int32_t material) { return material > 0; }));
}
