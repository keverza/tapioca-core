#include "Geometry/GeometryRustTerrain.hpp"

#include "GeometryRustFfi.h"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace {

using geomsrv::Mesh;
using geomsrv::Snapshot;

TapiocaMeshView RawMeshView (const Mesh& mesh)
{
    TapiocaMeshView view {};
    view.guid = TapiocaTextView { mesh.guid.data (), mesh.guid.size () };
    view.elemType = mesh.elemType;
    view.vertices = TapiocaDoubleView { mesh.vertices.data (), mesh.vertices.size () };
    view.normals = TapiocaFloatView { mesh.normals.data (), mesh.normals.size () };
    view.triangles = TapiocaIndexView { mesh.triangles.data (), mesh.triangles.size () };
    view.triMaterial = TapiocaIntView { mesh.triMaterial.data (), mesh.triMaterial.size () };
    view.triWireEdges = TapiocaByteView { mesh.triWireEdges.data (), mesh.triWireEdges.size () };
    view.triSourceBody = TapiocaIntView { mesh.triSourceBody.data (), mesh.triSourceBody.size () };
    view.triSourcePgon = TapiocaIntView { mesh.triSourcePgon.data (), mesh.triSourcePgon.size () };
    view.triSurfaceAttribute = TapiocaIntView { mesh.triSurfaceAttribute.data (), mesh.triSurfaceAttribute.size () };
    return view;
}

void ExpectMeshArraysUnchanged (const Mesh& before, const Mesh& after)
{
    EXPECT_EQ (before.guid, after.guid);
    EXPECT_EQ (before.elemType, after.elemType);
    EXPECT_EQ (before.vertices, after.vertices);
    EXPECT_EQ (before.normals, after.normals);
    EXPECT_EQ (before.triangles, after.triangles);
    EXPECT_EQ (before.triMaterial, after.triMaterial);
    EXPECT_EQ (before.triWireEdges, after.triWireEdges);
    EXPECT_EQ (before.triSourceBody, after.triSourceBody);
    EXPECT_EQ (before.triSourcePgon, after.triSourcePgon);
    EXPECT_EQ (before.triSurfaceAttribute, after.triSurfaceAttribute);
}

void ExpectErrorPrefix (const std::string& error, const std::string& prefix)
{
    EXPECT_EQ (0U, error.rfind (prefix, 0)) << error;
}

} // namespace

TEST (GeometryRustTerrain, ReadRustAbiVersionReportsSupportedVersion)
{
    uint32_t version = 0;
    std::string error = "stale";
    ASSERT_TRUE (tapioca::geometry::ReadRustAbiVersion (version, error)) << error;
    EXPECT_EQ (1U, version);
    EXPECT_TRUE (error.empty ());
}

TEST (GeometryRustTerrain, EmptyMeshIsValid)
{
    const Mesh empty = evptest::MakeEmpty ("");
    std::string error = "stale";
    EXPECT_TRUE (tapioca::geometry::ValidateMesh (empty, error)) << error;
    EXPECT_TRUE (error.empty ());
}

TEST (GeometryRustTerrain, EmptySnapshotIsValid)
{
    const Snapshot empty;
    std::string error = "stale";
    EXPECT_TRUE (tapioca::geometry::ValidateSnapshot (empty, error)) << error;
    EXPECT_TRUE (error.empty ());
}

TEST (GeometryRustTerrain, ValidMeshWithAllSidecarsIsNotModified)
{
    Mesh mesh = evptest::MakeSingleTriangle ("sidecars");
    mesh.normals = { 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F };
    mesh.triMaterial = { 7 };
    mesh.triWireEdges = { 0x05 };
    mesh.triSourceBody = { 1 };
    mesh.triSourcePgon = { 2 };
    mesh.triSurfaceAttribute = { 42 };
    const Mesh before = mesh;

    std::string error;
    ASSERT_TRUE (tapioca::geometry::ValidateMesh (mesh, error)) << error;
    EXPECT_TRUE (error.empty ());
    ExpectMeshArraysUnchanged (before, mesh);
}

TEST (GeometryRustTerrain, NonTripleScalarLengthsAreRejected)
{
    Mesh badVertices = evptest::MakeSingleTriangle ("bad-vertices");
    badVertices.vertices.push_back (4.0);
    std::string error;
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (badVertices, error));
    ExpectErrorPrefix (error, "MeshArrayLength");

    Mesh badTriangles = evptest::MakeSingleTriangle ("bad-triangles");
    badTriangles.triangles.pop_back ();
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (badTriangles, error));
    ExpectErrorPrefix (error, "MeshArrayLength");
}

TEST (GeometryRustTerrain, InvalidIndicesAndNonFiniteValuesAreRejected)
{
    std::string error;
    const Mesh indices = evptest::MakeOutOfRangeIndices ("indices");
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (indices, error));
    ExpectErrorPrefix (error, "MeshIndexOutOfBounds");

    const Mesh nan = evptest::MakeNaNTriangle ("nan");
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (nan, error));
    ExpectErrorPrefix (error, "MeshNonFinite");

    Mesh badNormals = evptest::MakeSingleTriangle ("bad-normals");
    badNormals.normals = { 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (badNormals, error));
    ExpectErrorPrefix (error, "MeshSidecarLength");
}

TEST (GeometryRustTerrain, OptionalTriangleSidecarsMustBeEmptyOrFull)
{
    std::string error;

    Mesh badMaterial = evptest::MakeSingleTriangle ("bad-material");
    badMaterial.triMaterial = { 0, 1 };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (badMaterial, error));
    ExpectErrorPrefix (error, "MeshSidecarLength");

    Mesh badWire = evptest::MakeSingleTriangle ("bad-wire");
    badWire.triWireEdges = { 1, 2 };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (badWire, error));
    ExpectErrorPrefix (error, "MeshSidecarLength");

    Mesh badSurface = evptest::MakeSingleTriangle ("bad-surface");
    badSurface.triSurfaceAttribute = { 1, 2 };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (badSurface, error));
    ExpectErrorPrefix (error, "MeshSidecarLength");
}

TEST (GeometryRustTerrain, SourceSidecarsMustBePairedAndPositive)
{
    std::string error;

    Mesh bodyOnly = evptest::MakeSingleTriangle ("body-only");
    bodyOnly.triSourceBody = { 1 };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (bodyOnly, error));
    ExpectErrorPrefix (error, "MeshSourceSidecarInvalid");

    Mesh pgonOnly = evptest::MakeSingleTriangle ("pgon-only");
    pgonOnly.triSourcePgon = { 1 };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (pgonOnly, error));
    ExpectErrorPrefix (error, "MeshSourceSidecarInvalid");

    Mesh positive = evptest::MakeSingleTriangle ("positive-source");
    positive.triSourceBody = { 1 };
    positive.triSourcePgon = { 2 };
    EXPECT_TRUE (tapioca::geometry::ValidateMesh (positive, error)) << error;

    Mesh nonPositive = evptest::MakeSingleTriangle ("non-positive-source");
    nonPositive.triSourceBody = { 0 };
    nonPositive.triSourcePgon = { 2 };
    EXPECT_FALSE (tapioca::geometry::ValidateMesh (nonPositive, error));
    ExpectErrorPrefix (error, "MeshSourceSidecarInvalid");
}

TEST (GeometryRustTerrain, LargeWorldCoordinatesAreAccepted)
{
    Mesh mesh = evptest::MakeSingleTriangle ("large-world");
    mesh.vertices = { 1.0e9, -2.0e9, 3.0e6, 1.000001e9, -2.0e9, 3.0e6, 1.0e9, -1.999999e9, 3.0e6 };

    std::string error;
    EXPECT_TRUE (tapioca::geometry::ValidateMesh (mesh, error)) << error;
}

TEST (GeometryRustTerrain, DegenerateTrianglesPassGenericGate)
{
    std::string error;
    EXPECT_TRUE (tapioca::geometry::ValidateMesh (evptest::MakeZeroAreaTriangle ("zero"), error)) << error;
    EXPECT_TRUE (tapioca::geometry::ValidateMesh (evptest::MakeSliverTriangle ("sliver"), error)) << error;
    EXPECT_TRUE (tapioca::geometry::ValidateMesh (evptest::MakeDuplicateVertexTriangle ("duplicate"), error)) << error;
}

TEST (GeometryRustTerrain, SnapshotRejectsDuplicateNonemptyGuids)
{
    const Snapshot snapshot =
        evptest::MakeSnapshot ({ evptest::MakeEmpty ("duplicate"), evptest::MakeSingleTriangle ("duplicate") });

    std::string error;
    EXPECT_FALSE (tapioca::geometry::ValidateSnapshot (snapshot, error));
    ExpectErrorPrefix (error, "SnapshotDuplicateGuid");
}

TEST (GeometryRustTerrain, SnapshotAllowsRepeatedBlankGuids)
{
    const Snapshot snapshot = evptest::MakeSnapshot ({ evptest::MakeEmpty (""), evptest::MakeEmpty ("") });

    std::string error;
    EXPECT_TRUE (tapioca::geometry::ValidateSnapshot (snapshot, error)) << error;
}

TEST (GeometryRustTerrain, SnapshotLeavesMaterialCatalogsUntouched)
{
    Snapshot snapshot = evptest::MakeSnapshot (
        { evptest::MakeSingleTriangle ("first"), evptest::MakeBox ("second", 3.0, 0.0, 0.0) }, 17, "selection");
    snapshot.captureStamp = 23;
    snapshot.completeModel = true;
    snapshot.materialTransparency = { { 0, 0.25 }, { 7, 0.75 } };
    snapshot.materialIdentity = { { 0, { "Glass", 12 } }, { 7, { "Steel", -3 } } };
    snapshot.surfaceAttributes = { { 42, { "Concrete", "surface-guid" } } };

    const auto transparency = snapshot.materialTransparency;
    const auto identity = snapshot.materialIdentity;
    const auto surfaces = snapshot.surfaceAttributes;

    std::string error;
    ASSERT_TRUE (tapioca::geometry::ValidateSnapshot (snapshot, error)) << error;
    EXPECT_EQ (transparency, snapshot.materialTransparency);
    EXPECT_EQ (identity, snapshot.materialIdentity);
    EXPECT_EQ (surfaces, snapshot.surfaceAttributes);
    EXPECT_EQ (2U, snapshot.materialTransparency.size ());
}

TEST (GeometryRustTerrain, DirectAbiRejectsMissingDescriptorAndInvalidBoolean)
{
    std::array<char, 256> error {};

    EXPECT_EQ (TAPIOCA_ERR_ARG, tapioca_validate_mesh (nullptr, error.data (), error.size ()));
    EXPECT_NE ('\0', error[0]);
    EXPECT_EQ (0U, std::string (error.data ()).rfind ("Null or misaligned", 0)) << error.data ();

    error.fill ('\0');
    const std::string scope = "all";
    TapiocaSnapshotView snapshot {};
    snapshot.scope = TapiocaTextView { scope.data (), scope.size () };
    snapshot.completeModel = 2;
    EXPECT_EQ (TAPIOCA_ERR_ARG, tapioca_validate_snapshot (&snapshot, error.data (), error.size ()));
    EXPECT_NE (std::string::npos, std::string (error.data ()).find ("InvalidBoolean")) << error.data ();
}

TEST (GeometryRustTerrain, DirectAbiReadsSnapshotArrayScalarViews)
{
    Mesh first = evptest::MakeSingleTriangle ("direct-first");
    first.normals = { 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F };
    Mesh second = evptest::MakeEmpty ("direct-second");

    std::vector<TapiocaMeshView> meshes { RawMeshView (first), RawMeshView (second) };
    const std::string scope = "selection";
    TapiocaSnapshotView snapshot {};
    snapshot.id = 9;
    snapshot.captureStamp = 11;
    snapshot.scope = TapiocaTextView { scope.data (), scope.size () };
    snapshot.completeModel = 1;
    snapshot.meshes = TapiocaMeshArrayView { meshes.data (), meshes.size () };

    std::array<char, 256> error {};
    ASSERT_EQ (TAPIOCA_OK, tapioca_validate_snapshot (&snapshot, error.data (), error.size ())) << error.data ();

    meshes[1].guid = meshes[0].guid;
    EXPECT_EQ (TAPIOCA_ERR_STATE, tapioca_validate_snapshot (&snapshot, error.data (), error.size ()));
    EXPECT_EQ (0U, std::string (error.data ()).rfind ("SnapshotDuplicateGuid", 0)) << error.data ();
}
