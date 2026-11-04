#include "ArchViz/VolumetricOverlay.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditSession.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace ol = geomsrv::archviz::overlaylayers;

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

lg::VolumetricDetailOutcome BuildAndPublish (lg::VolumetricDetailPublisher& publisher,
                                             const edit::Document& doc, lg::Id id,
                                             lg::VolumetricBuildLevel level)
{
    lg::VolumetricDetailRequest request;
    request.detail = id;
    request.level = level;
    lg::VolumetricDetailJob job;
    std::string error;
    EXPECT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto outcome = lg::RunVolumetricDetail (job);
    EXPECT_TRUE (outcome.built) << outcome.error;
    EXPECT_TRUE (publisher.Publish (doc, nullptr, outcome, error)) << error;
    return outcome;
}

void CheckOverlay (lg::Id id, const lg::VolumetricDetailOutcome& outcome)
{
    lg::VolumetricOverlayInput input;
    input.details.push_back ({ id, outcome.mesh });
    input.materials = lg::DefaultVolumetricMaterials ();
    input.diagnostics = &outcome;
    ol::Layer layer;
    std::string error;
    ASSERT_TRUE (lg::BuildVolumetricOverlay (input, layer, error)) << error;
    size_t triangles = 0;
    std::set<uint32_t> colors;
    for (const auto& mesh : layer.meshes) {
        triangles += mesh.indices.size () / 3;
        colors.insert (mesh.rgba);
        EXPECT_EQ (mesh.points, outcome.mesh->positions);
    }
    EXPECT_EQ (triangles, outcome.mesh->TriangleCount ());
    EXPECT_EQ (colors.size (), layer.meshes.size ());
    ASSERT_EQ (layer.panels.size (), 1);
    EXPECT_EQ (layer.panels.front ().tab, "stats");
}

} // namespace

const ol::PanelItem* FindRow (const ol::Panel& panel, const std::string& label)
{
    const auto found = std::find_if (panel.items.begin (), panel.items.end (),
                                     [&] (const ol::PanelItem& item) { return item.text == label; });
    return found == panel.items.end () ? nullptr : &*found;
}

TEST (VolumetricDetailWorkflow, RefusalDiagnosticsKeepLastGoodGeometryAndReason)
{
    StoreReset reset;
    edit::Document doc;
    lg::Id id = 0;
    std::string error;
    ASSERT_TRUE (lg::StartLandscape (doc, { 4, 5, 6 }, error)) << error;
    ASSERT_TRUE (lg::AddVolumetricDetail (doc, lg::MakeVolumetricCavePreset (), id, error)) << error;
    lg::VolumetricDetailPublisher publisher;
    auto good = BuildAndPublish (publisher, doc, id, lg::VolumetricBuildLevel::Final);

    lg::VolumetricDetailRequest request;
    request.detail = id;
    request.options.maxTriangles = 1;
    request.options.strategy = lg::SdfSamplingStrategy::Dense;
    lg::VolumetricDetailJob job;
    ASSERT_TRUE (publisher.Prepare (doc, request, job, error)) << error;
    auto refused = lg::RunVolumetricDetail (job);
    ASSERT_FALSE (refused.built);
    ASSERT_EQ (refused.mesh, nullptr);
    ASSERT_TRUE (publisher.Publish (doc, nullptr, refused, error)) << error;
    const auto* latest = publisher.LatestOutcome ();
    ASSERT_NE (latest, nullptr);
    ASSERT_EQ (latest->mesh, nullptr);

    lg::VolumetricOverlayInput input;
    input.details.push_back ({ id, lg::EditorMeshStore::Get ().Get ("volumetric/" + std::to_string (id)) });
    input.materials = lg::DefaultVolumetricMaterials ();
    input.diagnostics = latest;
    ol::Layer layer;
    ASSERT_TRUE (lg::BuildVolumetricOverlay (input, layer, error)) << error;
    size_t triangles = 0;
    for (const auto& mesh : layer.meshes)
        triangles += mesh.indices.size () / 3;
    EXPECT_EQ (triangles, good.mesh->TriangleCount ());
    ASSERT_EQ (layer.panels.size (), 1);
    ASSERT_NE (FindRow (layer.panels.front (), "Reason"), nullptr);
    EXPECT_EQ (FindRow (layer.panels.front (), "Reason")->value, latest->error);
    EXPECT_EQ (FindRow (layer.panels.front (), "Triangles")->value, std::to_string (good.mesh->TriangleCount ()));
    ASSERT_NE (FindRow (layer.panels.front (), "Sampling diagnostics"), nullptr);
    EXPECT_EQ (FindRow (layer.panels.front (), "Sampling diagnostics")->value, "unavailable (build refused)");
    EXPECT_EQ (FindRow (layer.panels.front (), "Sampling"), nullptr);
    EXPECT_EQ (FindRow (layer.panels.front (), "Narrow band"), nullptr);
    EXPECT_EQ (FindRow (layer.panels.front (), "Evaluated / total points"), nullptr);
}

TEST (VolumetricDetailWorkflow, RockAndCavePlaceEditUndoReloadPreviewFinalAndOverlay)
{
    StoreReset reset;
    for (const bool cave : { false, true }) {
        edit::Document doc;
        std::string error;
        ASSERT_TRUE (lg::StartLandscape (doc, { 7, 8, 9 }, error)) << error;
        edit::Session session (doc);
        lg::Id id = 0;
        ASSERT_TRUE (session.Apply ([&] (edit::Document& candidate, std::string& e) {
            return lg::AddVolumetricDetail (candidate,
                                            cave ? lg::MakeVolumetricCavePreset () : lg::MakeVolumetricRockPreset (),
                                            id, e);
        })) << error;
        const auto placed = session.Doc ().landscape->volumetricDetails.front ();
        const auto placedLandscape = session.Doc ().landscape;
        const auto placedRevision = session.Doc ().revision;

        lg::VolumetricDetailPublisher publisher;
        auto preview = BuildAndPublish (publisher, session.Doc (), id, lg::VolumetricBuildLevel::Preview);
        CheckOverlay (id, preview);
        auto final = BuildAndPublish (publisher, session.Doc (), id, lg::VolumetricBuildLevel::Final);
        CheckOverlay (id, final);
        EXPECT_LT (preview.mesh->TriangleCount (), final.mesh->TriangleCount ());
        EXPECT_EQ (session.Doc ().landscape, placedLandscape);
        EXPECT_EQ (session.Doc ().revision, placedRevision);

        auto edited = session.Doc ().landscape->volumetricDetails.front ();
        edited.voxelSize *= 0.75;
        ASSERT_TRUE (session.Apply ([&] (edit::Document& candidate, std::string& e) {
            return lg::SetVolumetricDetail (candidate, edited, e);
        })) << error;
        const auto afterEdit = session.Doc ().landscape->volumetricDetails.front ();
        EXPECT_EQ (afterEdit.revision, placed.revision + 1);
        ASSERT_TRUE (session.Undo ());
        EXPECT_EQ (session.Doc ().landscape->volumetricDetails.front (), placed);
        ASSERT_TRUE (session.Redo ());
        EXPECT_EQ (session.Doc ().landscape->volumetricDetails.front (), afterEdit);

        std::string text;
        ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, text, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (text, loaded, error)) << error;
        ASSERT_EQ (loaded.landscape->volumetricDetails.size (), 1);
        EXPECT_EQ (loaded.landscape->volumetricDetails.front (), afterEdit);

        lg::VolumetricDetailPublisher reloadedPublisher;
        auto rebuilt = BuildAndPublish (reloadedPublisher, loaded, id, lg::VolumetricBuildLevel::Final);
        CheckOverlay (id, rebuilt);
        EXPECT_EQ (rebuilt.definitionHash, lg::HashVolumetricDetail (afterEdit));
        EXPECT_EQ (rebuilt.mesh->positions,
                   lg::EditorMeshStore::Get ().Get ("volumetric/" + std::to_string (id))->positions);
    }
}
