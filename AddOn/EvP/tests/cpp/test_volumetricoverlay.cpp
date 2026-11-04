#include "ArchViz/VolumetricOverlay.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <set>

namespace lg = tapioca::geometry;
namespace ol = geomsrv::archviz::overlaylayers;

namespace {

std::shared_ptr<lg::PublishedMesh> Tetrahedron (lg::Id id)
{
    auto mesh = std::make_shared<lg::PublishedMesh> ();
    mesh->object = "volumetric/" + std::to_string (id);
    mesh->family = lg::kVolumetricDetailMeshFamily;
    mesh->revision = 3;
    mesh->definitionHash = 99;
    mesh->positions = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    mesh->triangles = { 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3 };
    mesh->triMaterial = { 2, 1, 2, 1 };
    mesh->vertexSource = { 0, 1, 2, 3 };
    mesh->triangleSource = { 0, 1, 2, 3 };
    for (size_t index = 0; index < mesh->positions.size (); index += 3)
        mesh->bounds.Expand (mesh->positions[index], mesh->positions[index + 1], mesh->positions[index + 2]);
    mesh->valid = true;
    return mesh;
}

lg::VolumetricDetailOutcome Outcome (const std::shared_ptr<const lg::PublishedMesh>& mesh, lg::Id id)
{
    lg::VolumetricDetailOutcome outcome;
    outcome.detailId = id;
    outcome.revision = mesh->revision;
    outcome.definitionHash = mesh->definitionHash;
    outcome.level = lg::VolumetricBuildLevel::Preview;
    outcome.voxelSize = 0.4;
    outcome.mesh = mesh;
    outcome.contentHash = lg::HashPublishedMesh (*mesh);
    outcome.built = true;
    auto& diagnostics = outcome.diagnostics;
    diagnostics.totalPoints = 100;
    diagnostics.evaluatedPoints = 40;
    diagnostics.storedSparsePoints = 30;
    diagnostics.regions = 10;
    diagnostics.skippedRegions = 5;
    diagnostics.cells = 80;
    diagnostics.activeCells = 20;
    diagnostics.triangles = mesh->TriangleCount ();
    diagnostics.lipschitz = 1.5;
    diagnostics.actualSpacing = { 0.2, 0.2, 0.2 };
    diagnostics.strategy = lg::SdfSamplingStrategy::Sparse;
    diagnostics.slabCells = 64;
    return outcome;
}

lg::VolumetricOverlayInput Input (lg::Id id = 42)
{
    lg::VolumetricOverlayInput input;
    input.details.push_back ({ id, Tetrahedron (id) });
    input.materials = lg::DefaultVolumetricMaterials ();
    return input;
}

const ol::PanelItem* FindRow (const ol::Panel& panel, const std::string& label)
{
    const auto found = std::find_if (panel.items.begin (), panel.items.end (),
                                     [&] (const ol::PanelItem& item) { return item.text == label; });
    return found == panel.items.end () ? nullptr : &*found;
}

} // namespace

TEST (VolumetricOverlay, MaterialSplitPreservesEveryTriangleAndDiagnosticsExplainSampling)
{
    auto input = Input ();
    auto outcome = Outcome (input.details.front ().mesh, 42);
    input.diagnostics = &outcome;
    ol::Layer layer;
    std::string error;
    ASSERT_TRUE (lg::BuildVolumetricOverlay (input, layer, error)) << error;
    EXPECT_EQ (layer.name, "tapioca.volumetric-details");
    EXPECT_EQ (layer.views, ol::Views::ThreeD);
    EXPECT_TRUE (layer.solid);
    ASSERT_EQ (layer.meshes.size (), 2);
    EXPECT_EQ (layer.meshes[0].indices, (std::vector<uint32_t> { 0, 1, 3, 2, 0, 3 }));
    EXPECT_EQ (layer.meshes[1].indices, (std::vector<uint32_t> { 0, 2, 1, 1, 2, 3 }));
    size_t triangles = 0;
    std::set<int32_t> materials;
    for (const auto& mesh : layer.meshes) {
        EXPECT_EQ (mesh.points, input.details.front ().mesh->positions);
        EXPECT_EQ (mesh.indices.size () % 3, 0);
        triangles += mesh.indices.size () / 3;
        EXPECT_EQ (mesh.style.shading, ol::Shading::Lit);
        ASSERT_FALSE (mesh.hoverRows.empty ());
        materials.insert (mesh.rgba == input.materials[0].previewRgba ? 1 : 2);
    }
    EXPECT_EQ (triangles, input.details.front ().mesh->TriangleCount ());
    EXPECT_EQ (materials, (std::set<int32_t> { 1, 2 }));

    ASSERT_EQ (layer.panels.size (), 1);
    const auto& panel = layer.panels.front ();
    EXPECT_EQ (panel.tab, "stats");
    ASSERT_NE (FindRow (panel, "Evaluated / total points"), nullptr);
    EXPECT_EQ (FindRow (panel, "Evaluated / total points")->value, "40 / 100");
    EXPECT_EQ (FindRow (panel, "Skipped / total regions")->value, "5 / 10");
    EXPECT_EQ (FindRow (panel, "Narrow band")->value, "available");
    EXPECT_EQ (FindRow (panel, "Lipschitz bound")->value, "1.5");
    EXPECT_EQ (FindRow (panel, "Cell size")->value, "0.2 / 0.2 / 0.2 m");
    EXPECT_NE (FindRow (panel, "Health")->value.find ("closed manifold"), std::string::npos);
    EXPECT_NE (FindRow (panel, "Bounds"), nullptr);
}

TEST (VolumetricOverlay, UnmappedMaterialsAndMalformedInputsRefuseAtomically)
{
    auto input = Input ();
    auto mesh = Tetrahedron (42);
    input.details.front ().mesh = mesh;
    mesh->triMaterial[0] = 99;
    ol::Layer layer;
    layer.name = "unchanged";
    std::string error;
    EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error));
    EXPECT_NE (error.find ("material 99"), std::string::npos);
    EXPECT_EQ (layer.name, "unchanged");

    mesh->triMaterial[0] = 1;
    input.materials.push_back (input.materials.front ());
    EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error));
    EXPECT_EQ (layer.name, "unchanged");

    input = Input ();
    input.display.only = 100;
    EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error));
    EXPECT_EQ (layer.name, "unchanged");

    input = Input ();
    auto invalidMesh = Tetrahedron (42);
    invalidMesh->triangles[0] = 99;
    input.details.front ().mesh = invalidMesh;
    EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error));
    EXPECT_EQ (layer.name, "unchanged");
}

TEST (VolumetricOverlay, HiddenDetailsStillRequireTotalMaterialMapping)
{
    auto input = Input ();
    input.details.push_back ({ 43, Tetrahedron (43) });
    auto hidden = Tetrahedron (43);
    input.details.back ().mesh = hidden;
    hidden->triMaterial[0] = 77;
    input.display.only = 42;
    ol::Layer layer;
    std::string error;
    EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error));
    EXPECT_NE (error.find ("material 77"), std::string::npos);
}

TEST (VolumetricOverlay, ShowOnlyAndClearAreTransientAndUseExistingFocusAndShading)
{
    auto input = Input ();
    input.details.push_back ({ 43, Tetrahedron (43) });
    lg::VolumetricDisplayState state;
    geomsrv::archviz::editlayers::Feedback feedback;
    ol::Layer layer;
    std::string error;

    lg::ShowDetail (state, 42, feedback);
    EXPECT_EQ (feedback.focus, nullptr);
    input.display = state;
    ASSERT_TRUE (lg::BuildVolumetricOverlay (input, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 4);
    EXPECT_EQ (layer.meshes[0].style.shading, ol::Shading::Xray);
    EXPECT_EQ (layer.meshes[1].style.shading, ol::Shading::Xray);
    EXPECT_EQ (layer.meshes[2].style.shading, ol::Shading::Lit);
    EXPECT_EQ (layer.meshes[0].style.behind, ol::Behind::Show);

    lg::OnlyDetail (state, 42, feedback);
    ASSERT_NE (feedback.focus, nullptr);
    EXPECT_EQ (*feedback.focus, (std::vector<lg::Id> { 42 }));
    input.display = state;
    ASSERT_TRUE (lg::BuildVolumetricOverlay (input, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 2);
    EXPECT_EQ (layer.meshes[0].style.shading, ol::Shading::Ghost);
    EXPECT_EQ (layer.meshes[1].style.shading, ol::Shading::Ghost);

    lg::ClearDetailDisplay (state, feedback);
    EXPECT_EQ (feedback.focus, nullptr);
    input.display = state;
    ASSERT_TRUE (lg::BuildVolumetricOverlay (input, layer, error)) << error;
    ASSERT_EQ (layer.meshes.size (), 4);
    EXPECT_TRUE (std::all_of (layer.meshes.begin (), layer.meshes.end (),
                              [] (const ol::Mesh& mesh) { return mesh.style.shading == ol::Shading::Lit; }));
}

TEST (VolumetricOverlay, DiagnosticsCannotDescribeADifferentSnapshot)
{
    auto input = Input ();
    auto outcome = Outcome (input.details.front ().mesh, 42);
    auto other = Tetrahedron (42);
    other->positions[0] = 2.0;
    other->bounds = {};
    for (size_t index = 0; index < other->positions.size (); index += 3)
        other->bounds.Expand (other->positions[index], other->positions[index + 1], other->positions[index + 2]);
    outcome.mesh = other;
    outcome.contentHash = lg::HashPublishedMesh (*other);
    input.diagnostics = &outcome;
    ol::Layer layer;
    layer.name = "unchanged";
    std::string error;
    EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error));
    EXPECT_NE (error.find ("Mismatch"), std::string::npos);
    EXPECT_EQ (layer.name, "unchanged");
}

TEST (VolumetricOverlay, EveryBoundsAxisMustMatchThePositions)
{
    for (const int variant : { 0, 1, 2 }) {
        auto input = Input ();
        auto mesh = Tetrahedron (42);
        if (variant == 0)
            mesh->bounds.mn[1] = std::numeric_limits<double>::quiet_NaN ();
        else if (variant == 1) {
            mesh->bounds.mn[2] = 2;
            mesh->bounds.mx[2] = -2;
        }
        else {
            mesh->bounds = {};
            mesh->bounds.Expand (100, 100, 100);
            mesh->bounds.Expand (101, 101, 101);
        }
        input.details.front ().mesh = mesh;
        ol::Layer layer;
        layer.name = "unchanged";
        std::string error;
        EXPECT_FALSE (lg::BuildVolumetricOverlay (input, layer, error)) << variant;
        EXPECT_EQ (layer.name, "unchanged");
    }
}

TEST (VolumetricOverlay, DiagnosticsRefusalLeavesPanelUntouched)
{
    auto mesh = Tetrahedron (42);
    auto outcome = Outcome (mesh, 42);
    outcome.built = false;
    outcome.mesh.reset ();
    outcome.contentHash = 0;
    outcome.error = "TriangleBudget: test refusal";
    ol::Panel panel;
    std::string error;
    ASSERT_TRUE (lg::BuildVolumetricDiagnosticsPanel (outcome, *mesh, panel, error)) << error;
    ASSERT_NE (FindRow (panel, "Reason"), nullptr);
    EXPECT_EQ (FindRow (panel, "Reason")->value, outcome.error);
    EXPECT_EQ (FindRow (panel, "Triangles")->value, "4");

    auto malformed = *mesh;
    malformed.positions.clear ();
    ol::Panel original = panel;
    EXPECT_FALSE (lg::BuildVolumetricDiagnosticsPanel (outcome, malformed, panel, error));
    EXPECT_EQ (panel.title, original.title);
    EXPECT_EQ (panel.items.size (), original.items.size ());
}
