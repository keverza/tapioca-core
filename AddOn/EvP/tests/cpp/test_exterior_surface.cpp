// User-confirmed exterior seeds, not camera/material-based classification.
#include "Geometry/ExteriorSurfaceRegion.hpp"
#include "ArchViz/SurfaceTakeoffLayer.hpp"
#include "MeshFixtures.hpp"
#include <gtest/gtest.h>

namespace {
using namespace geomsrv;
namespace ex = exposed;
namespace presentation = archviz::surfacetakeoff;
Mesh Box (const char* guid, double x, double y = 0, int material = 1)
{
    auto mesh = evptest::MakeBox (guid, x, y, 0, 1, .2, 1);
    mesh.elemType = 1;
    mesh.triSourceBody.assign (12, 1);
    mesh.triMaterial.assign (12, material);
    mesh.triSurfaceAttribute.assign (12, material == 1 ? 100 : 200);
    for (int i = 1; i <= 6; ++i)
        mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { i, i });
    return mesh;
}
Snapshot Capture ()
{
    Snapshot snapshot;
    snapshot.id = 1;
    snapshot.captureStamp = 2;
    snapshot.meshes = { Box ("left", 0), Box ("right", 1, 0, 2), Box ("detached", 3), Box ("interior", 0, 1) };
    snapshot.surfaceAttributes[100] = { "Stucco red", "red-guid" };
    snapshot.surfaceAttributes[200] = { "Stucco yellow", "yellow-guid" };
    snapshot.materialIdentity[1] = { "Red pool", 100 };
    snapshot.materialIdentity[2] = { "Yellow pool", 200 };
    return snapshot;
}
ex::ExteriorSeed Seed ()
{
    return { 0, { .5, 0, .5 }, { 0, -1, 0 } };
}
} // namespace

TEST (ExteriorSurface, ConnectedFacadeDoesNotExpandToBackInteriorOrDetachedSameMaterial)
{
    auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces;
    std::string error;
    ASSERT_TRUE (ex::ResolveFacadeRegion (snapshot, { 0, 1, 2, 3 }, { Seed () }, faces, error)) << error;
    ASSERT_EQ (faces.size (), 2u);
    EXPECT_EQ (faces[0], (ex::FaceKey { { 0, 1 }, 3 }));
    EXPECT_EQ (faces[1], (ex::FaceKey { { 1, 1 }, 3 }));
}

TEST (ExteriorSurface, AllTargetsIncludesBothSidesEndsTopsAndDisconnectedInteriorWallsWithoutSeeds)
{
    auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces;
    std::string error;
    ASSERT_TRUE (ex::ResolveAllTargetFaces (snapshot, { 3, 1, 0, 2, 0 }, faces, error)) << error;
    EXPECT_EQ (faces.size (), 24u);
    ex::SurfaceSelection selection;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, faces, std::set<int32_t> { 100, 200 }, selection, error))
        << error;
    ex::Request request;
    request.snapshotId = 1;
    request.captureStamp = 2;
    request.coverScopeId = "all-walls";
    request.faces = faces;
    request.materialPoolFilter = selection.materialPoolFilter;
    request.covers = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } };
    ex::Result result;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    // Four 1 x .2 x 1 solids: 4*2.8 minus the shared .2 m2 end twice.
    // Fixed-precision planar booleans use the suite's 1e-6 m2 area tolerance.
    EXPECT_NEAR (result.areas.selectedExposedArea, 10.8, 1e-6);
    EXPECT_EQ (result.byElementGuid.size (), 4u);
    presentation::Presentation output;
    ASSERT_TRUE (presentation::Build (snapshot, result, selection, { 100, 200 }, {}, "", output, error,
                                      { 0xE5484DFFu, 0xF6C945FFu }, true))
        << error;
    EXPECT_NEAR (output.rows[0].areaM2, 8.2, 1e-6);
    EXPECT_NEAR (output.rows[1].areaM2, 2.6, 1e-6);
    EXPECT_NE (output.layer.panels[0].items[0].text.find ("includes interiors"), std::string::npos);
}

TEST (ExteriorSurface, WholeWallInventoryRefusesMalformedTargetsAndBudgetsWithoutPartialFaces)
{
    auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces;
    std::string error;
    EXPECT_FALSE (ex::ResolveAllTargetFaces (snapshot, {}, faces, error));
    EXPECT_FALSE (ex::ResolveAllTargetFaces (snapshot, { 99 }, faces, error));
    EXPECT_FALSE (ex::ResolveAllTargetFaces (snapshot, { 0 }, faces, error, {}, [] { return true; }));
    ex::Limits limits;
    limits.maxFaces = 6;
    EXPECT_FALSE (ex::ResolveAllTargetFaces (snapshot, { 0, 1 }, faces, error, limits));
    EXPECT_TRUE (faces.empty ());
    snapshot.meshes[1].triSourceBody.clear ();
    EXPECT_FALSE (ex::ResolveAllTargetFaces (snapshot, { 0, 1 }, faces, error));
    EXPECT_TRUE (faces.empty ());
}
TEST (ExteriorSurface, PointOnlyContactDoesNotCrossToAnotherFacade)
{
    auto snapshot = Capture ();
    for (size_t i = 2; i < snapshot.meshes[1].vertices.size (); i += 3)
        snapshot.meshes[1].vertices[i] += 1;
    std::vector<ex::FaceKey> faces;
    std::string error;
    ASSERT_TRUE (ex::ResolveFacadeRegion (snapshot, { 0, 1 }, { Seed () }, faces, error)) << error;
    EXPECT_EQ (faces.size (), 1u);
}

TEST (ExteriorSurface, SeedInWindowHoleRefusesButSurroundingPlaneRemainsSelectable)
{
    auto snapshot = Capture ();
    Mesh ring;
    ring.guid = "opening";
    const auto quad = [&] (double x0, double x1, double z0, double z1) {
        const uint32_t at = uint32_t (ring.VertexCount ());
        ring.vertices.insert (ring.vertices.end (), { x0, 0, z0, x1, 0, z0, x1, 0, z1, x0, 0, z1 });
        ring.triangles.insert (ring.triangles.end (), { at, at + 1, at + 2, at, at + 2, at + 3 });
        ring.triSourceBody.insert (ring.triSourceBody.end (), { 1, 1 });
        ring.triSourcePgon.insert (ring.triSourcePgon.end (), { 3, 3 });
    };
    quad (0, 1, 0, .4);
    quad (0, 1, .6, 1);
    quad (0, .4, .4, .6);
    quad (.6, 1, .4, .6);
    snapshot.meshes = { ring };
    std::vector<ex::FaceKey> faces;
    std::string error;
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0 }, { Seed () }, faces, error));
    auto seed = Seed ();
    seed.point[0] = .2;
    ASSERT_TRUE (ex::ResolveFacadeRegion (snapshot, { 0 }, { seed }, faces, error)) << error;
    EXPECT_EQ (faces.size (), 1u);
}
TEST (ExteriorSurface, MultipleSeedsAndRequestOrderDoNotDoubleCount)
{
    auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces;
    std::string error;
    auto other = Seed ();
    other.meshIndex = 2;
    other.point[0] = 3.5;
    ASSERT_TRUE (ex::ResolveFacadeRegion (snapshot, { 2, 1, 0, 0 }, { other, Seed (), Seed () }, faces, error))
        << error;
    EXPECT_EQ (faces.size (), 3u);
}
TEST (ExteriorSurface, InvalidAmbiguousAndAbsentSeedsRefuseAtomically)
{
    auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces { { { 99, 1 }, 1 } };
    std::string error;
    auto seed = Seed ();
    seed.point[0] = 100;
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0 }, { seed }, faces, error));
    EXPECT_TRUE (faces.empty ());
    seed = Seed ();
    seed.normal = { 0, 1, 0 };
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0 }, { seed }, faces, error));
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 1 }, { Seed () }, faces, error));
    auto& mesh = snapshot.meshes[0];
    const auto indices = mesh.triangles;
    mesh.triangles.insert (mesh.triangles.end (), indices.begin (), indices.end ());
    mesh.triMaterial.insert (mesh.triMaterial.end (), 12, 1);
    mesh.triSourceBody.insert (mesh.triSourceBody.end (), 12, 2);
    for (int i = 1; i <= 6; ++i)
        mesh.triSourcePgon.insert (mesh.triSourcePgon.end (), { i, i });
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0 }, { Seed () }, faces, error));
    EXPECT_TRUE (faces.empty ());
}
TEST (ExteriorSurface, CancellationBudgetsAndMalformedProvenanceRefuse)
{
    auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces;
    std::string error;
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0, 1 }, { Seed () }, faces, error, {}, [] { return true; }));
    ex::Limits limits;
    limits.maxWork = 1;
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0, 1 }, { Seed () }, faces, error, limits));
    limits = {};
    limits.maxFaces = 1;
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0, 1 }, { Seed () }, faces, error, limits));
    snapshot.meshes[1].triSourcePgon.clear ();
    EXPECT_FALSE (ex::ResolveFacadeRegion (snapshot, { 0, 1 }, { Seed () }, faces, error));
    EXPECT_TRUE (faces.empty ());
}
TEST (ExteriorSurface, RedYellowOverlayAndPanelUseExactSelectedFragmentTriangles)
{
    const auto snapshot = Capture ();
    std::vector<ex::FaceKey> faces;
    std::string error;
    ASSERT_TRUE (ex::ResolveFacadeRegion (snapshot, { 0, 1 }, { Seed () }, faces, error)) << error;
    ex::SurfaceSelection selection;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, faces, std::set<int32_t> { 100, 200 }, selection, error))
        << error;
    ex::Request request;
    request.snapshotId = 1;
    request.captureStamp = 2;
    request.coverScopeId = "none";
    request.faces = faces;
    request.materialPoolFilter = selection.materialPoolFilter;
    ex::Result result;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    presentation::Presentation output;
    ASSERT_TRUE (presentation::Build (snapshot, result, selection, { 100, 200 }, { { 100, 999 } },
                                      "Not equivalent scope", output, error))
        << error;
    ASSERT_EQ (output.rows.size (), 2u);
    ASSERT_EQ (output.layer.meshes.size (), result.fragments.size ());
    EXPECT_NE (output.rows[0].rgba, output.rows[1].rgba);
    EXPECT_DOUBLE_EQ (output.rows[0].areaM2, 1);
    EXPECT_DOUBLE_EQ (output.rows[1].areaM2, 1);
    EXPECT_DOUBLE_EQ (output.areaM2, 2);
    EXPECT_EQ (output.triangles, 4u);
    EXPECT_EQ (output.layer.views, archviz::overlaylayers::Views::ThreeD);
    EXPECT_EQ (output.layer.panels.size (), 2u);
    for (size_t i = 0; i < result.fragments.size (); ++i) {
        EXPECT_EQ (output.layer.meshes[i].points, result.fragments[i].points);
        EXPECT_EQ (output.layer.meshes[i].indices, result.fragments[i].triangles);
        EXPECT_TRUE (output.layer.meshes[i].styled);
    }
    result.areas.selectedExposedArea = 999;
    EXPECT_FALSE (presentation::Build (snapshot, result, selection, { 100, 200 }, {}, "", output, error));
    EXPECT_TRUE (output.layer.meshes.empty ());
    EXPECT_TRUE (output.rows.empty ());
}

TEST (ExteriorSurface, MaterialListSwatchesAndDirectionFiltersUseCachedClippedGeometry)
{
    auto snapshot = Capture ();
    snapshot.surfaceAttributes[300] = { "Unused finish", "unused" };
    std::vector<ex::FaceKey> faces;
    std::string error;
    ASSERT_TRUE (ex::ResolveAllTargetFaces (snapshot, { 0, 1, 2, 3 }, faces, error)) << error;
    ex::SurfaceSelection selection;
    ASSERT_TRUE (ex::ResolveSurfaceSelection (snapshot, faces, std::set<int32_t> { 100, 200, 300 }, selection, error))
        << error;
    ex::Request request;
    request.snapshotId = 1;
    request.captureStamp = 2;
    request.coverScopeId = "all covers remain";
    request.faces = faces;
    request.covers = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } };
    request.materialPoolFilter = selection.materialPoolFilter;
    request.contactToleranceM = .001;
    ex::Result result;
    ASSERT_TRUE (ex::Calculate (snapshot, request, result, error)) << error;
    presentation::Presentation data;
    ASSERT_TRUE (presentation::Build (snapshot, result, selection, { 100, 200, 300 }, {}, "", data, error)) << error;
    ASSERT_EQ (data.rows.size (), 3u);
    auto state = presentation::InitialUi (data, "revision-9");
    const auto areas = presentation::UiAreas (data, state);
    EXPECT_NEAR (areas.at (100), 8.2, 1e-6);
    EXPECT_NEAR (areas.at (200), 2.6, 1e-6);
    EXPECT_EQ (areas.at (300), 0);
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "swatch", presentation::UiId (state, "highlight", 200), 1));
    const auto isolated = presentation::UiLayer (data, state);
    EXPECT_LT (isolated.meshes.size (), data.layer.meshes.size ());
    EXPECT_EQ (presentation::UiAreas (data, state), areas); // focusing never changes measured quantities
    EXPECT_EQ (state.focus, 200);
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "checkbox", presentation::UiId (state, "top"), 0));
    EXPECT_NEAR (presentation::UiAreas (data, state).at (200), 2.4, 1e-6);
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "checkbox", presentation::UiId (state, "bottom"), 0));
    EXPECT_NEAR (presentation::UiAreas (data, state).at (200), 2.2, 1e-6); // beam/contact end still subtracted
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "checkbox", presentation::UiId (state, "choose", 200), 0));
    EXPECT_TRUE (state.selected.contains (200)); // pending list is not a partly applied calculation
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "button", presentation::UiId (state, "calculate"), 1));
    EXPECT_FALSE (state.selected.contains (200));
    EXPECT_FALSE (state.focus.has_value ());
    EXPECT_FALSE (presentation::ApplyUiChange (data, state, "swatch", "old-revision:highlight:100", 1));
    EXPECT_FALSE (presentation::ApplyUiChange (data, state, "checkbox", presentation::UiId (state, "top"), 42));
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "checkbox", presentation::UiId (state, "sides"), 0));
    EXPECT_TRUE (presentation::UiLayer (data, state).meshes.empty ());
    EXPECT_DOUBLE_EQ (data.areaM2, result.areas.selectedExposedArea);
    EXPECT_TRUE (archviz::overlaylayers::Validate (presentation::UiLayer (data, state)).empty ());
    // Schedule-style rules filter target fragments, never the cover bodies used
    // by the earlier calculation. An Interior wall still covers an Exterior end.
    for (auto& [guid, element] : data.elements) {
        element.layer = guid == "left" ? "Exterior" : "Interior";
        element.story = "Ground";
        element.id = guid;
        element.detailsAvailable = true;
    }
    state = presentation::InitialUi (data, "new-capture");
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "button", presentation::UiId (state, "addCriterion"), 1));
    ASSERT_TRUE (
        presentation::ApplyUiChange (data, state, "criteriaField", presentation::UiId (state, "rule", 1), 1)); // Layer
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "criteriaValue", presentation::UiId (state, "rule", 1), 1,
                                              "Exterior"));
    EXPECT_NEAR (presentation::UiAreas (data, state).at (100), 8.2, 1e-6); // draft is not applied
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "button", presentation::UiId (state, "applyCriteria"), 1));
    EXPECT_NEAR (presentation::UiAreas (data, state).at (100), 2.6, 1e-6);
    EXPECT_NEAR (presentation::UiAreas (data, state).at (200), 0, 1e-6);
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "criteriaOpen", presentation::UiId (state, "rule", 1), 1));
    ASSERT_TRUE (presentation::ApplyUiChange (data, state, "button", presentation::UiId (state, "applyCriteria"), 1));
    EXPECT_FALSE (state.criteriaError.empty ());
    EXPECT_NEAR (presentation::UiAreas (data, state).at (100), 2.6, 1e-6); // invalid parentheses keep last good query
}
