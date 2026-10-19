#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/Mesh.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace {
using namespace tapioca::geometry;

geomsrv::Snapshot Fixture ()
{
    geomsrv::Snapshot snapshot;
    snapshot.id = 42;
    snapshot.captureStamp = 7;
    snapshot.completeModel = true;
    geomsrv::Mesh mesh;
    mesh.guid = "synthetic-value-fixture";
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    mesh.triangles = { 0, 1, 2 };
    mesh.triMaterial = { 1 };
    mesh.triWireEdges = { 7 };
    mesh.triSourceBody = { 1 };
    mesh.triSourcePgon = { 4 };
    mesh.triSurfaceAttribute = { 42 };
    mesh.bounds.Expand (0, 0, 0);
    mesh.bounds.Expand (1, 1, 0);
    snapshot.meshes.push_back (mesh);
    snapshot.materialTransparency[1] = 0.5;
    snapshot.materialIdentity[1] = { "material", 42 };
    snapshot.surfaceAttributes[42] = { "surface", "12345678-ABCD-1234-ABCD-123456789ABC" };
    return snapshot;
}

void Same (const geomsrv::Snapshot& actual, const geomsrv::Snapshot& expected)
{
    EXPECT_EQ (actual.id, expected.id);
    EXPECT_EQ (actual.captureStamp, expected.captureStamp);
    EXPECT_EQ (actual.scope, expected.scope);
    EXPECT_EQ (actual.completeModel, expected.completeModel);
    EXPECT_EQ (actual.materialTransparency, expected.materialTransparency);
    EXPECT_EQ (actual.materialIdentity, expected.materialIdentity);
    EXPECT_EQ (actual.surfaceAttributes, expected.surfaceAttributes);
    ASSERT_EQ (actual.meshes.size (), expected.meshes.size ());
    for (size_t index = 0; index < actual.meshes.size (); ++index) {
        const auto& a = actual.meshes[index];
        const auto& b = expected.meshes[index];
        EXPECT_EQ (a.guid, b.guid);
        EXPECT_EQ (a.elemType, b.elemType);
        EXPECT_EQ (a.vertices, b.vertices);
        EXPECT_EQ (a.normals, b.normals);
        EXPECT_EQ (a.triangles, b.triangles);
        EXPECT_EQ (a.triMaterial, b.triMaterial);
        EXPECT_EQ (a.triWireEdges, b.triWireEdges);
        EXPECT_EQ (a.triSourceBody, b.triSourceBody);
        EXPECT_EQ (a.triSourcePgon, b.triSourcePgon);
        EXPECT_EQ (a.triSurfaceAttribute, b.triSurfaceAttribute);
        for (size_t axis = 0; axis < 3; ++axis) {
            EXPECT_EQ (a.bounds.mn[axis], b.bounds.mn[axis]);
            EXPECT_EQ (a.bounds.mx[axis], b.bounds.mx[axis]);
        }
    }
}
} // namespace

TEST (GeometryRustValue, CompleteCatalogAndBoundsRoundTripIsImmutable)
{
    const auto snapshot = Fixture ();
    const auto before = snapshot;
    std::string error;
    ASSERT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
    EXPECT_TRUE (error.empty ());
    ASSERT_TRUE (ValidateMeshValue (snapshot.meshes.front (), error)) << error;
    Same (snapshot, before);
}

TEST (GeometryRustValue, EmptySyntheticAndUnknownCatalogsRemainExplicit)
{
    std::string error;
    geomsrv::Snapshot snapshot;
    ASSERT_TRUE (ValidateSnapshotValue (snapshot, {}, error)) << error;
    snapshot = Fixture ();
    snapshot.materialIdentity.clear ();
    snapshot.materialTransparency.clear ();
    snapshot.surfaceAttributes.clear ();
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, {}, error)) << error;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 7, true }, error));
    EXPECT_FALSE (error.empty ());
}

TEST (GeometryRustValue, CapturedFramingCannotBeInferredOrConfusedWithEditorRevision)
{
    auto snapshot = Fixture ();
    std::string error;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 8, true }, error));
    snapshot.captureStamp = 0;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 7, false }, error));
    snapshot = Fixture ();
    snapshot.scope = "selection";
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, {}, error));
    snapshot.completeModel = false;
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
}

TEST (GeometryRustValue, BadBoundsAndSidecarValuesRefuseWithoutRepair)
{
    for (int kind = 0; kind < 4; ++kind) {
        auto snapshot = Fixture ();
        auto& mesh = snapshot.meshes.front ();
        if (kind == 0)
            mesh.bounds.mn[1] = 2;
        else if (kind == 1)
            mesh.bounds.mx[2] = std::numeric_limits<double>::quiet_NaN ();
        else if (kind == 2)
            mesh.triWireEdges.front () = 8;
        else
            mesh.triSurfaceAttribute.front () = -1;
        const auto before = snapshot;
        std::string error;
        EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 7, true }, error));
        EXPECT_FALSE (error.empty ());
        // NaN is compared separately, not mistaken for a mutation.
        if (kind != 1)
            Same (snapshot, before);
        else
            EXPECT_TRUE (std::isnan (mesh.bounds.mx[2]));
    }
}

TEST (GeometryRustValue, InvalidCatalogValuesAndMissingStrictReferencesRefuse)
{
    for (int kind = 0; kind < 5; ++kind) {
        auto snapshot = Fixture ();
        if (kind == 0)
            snapshot.materialTransparency[1] = 1.01;
        else if (kind == 1)
            snapshot.materialTransparency[1] = std::numeric_limits<double>::infinity ();
        else if (kind == 2)
            snapshot.surfaceAttributes[42].guid = "not-a-native-guid";
        else if (kind == 3)
            snapshot.meshes.front ().triSurfaceAttribute.front () = 43;
        else
            snapshot.meshes.front ().triMaterial.front () = 2;
        const auto before = snapshot;
        std::string error;
        EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 7, true }, error));
        EXPECT_FALSE (error.empty ());
        Same (snapshot, before);
    }
}

TEST (GeometryRustValue, LegacyGateStillDoesNotCertifyCompleteValues)
{
    auto snapshot = Fixture ();
    snapshot.meshes.front ().bounds.mn[0] = 2;
    snapshot.materialTransparency[1] = -100;
    std::string error;
    EXPECT_TRUE (ValidateSnapshot (snapshot, error)) << error;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, {}, error));
}

TEST (GeometryRustValue, PoolIdentityAndSparseTransparencyNeverInferFaceSurfaces)
{
    auto snapshot = Fixture ();
    snapshot.materialTransparency.clear ();                // Missing is unknown, not malformed.
    snapshot.materialIdentity[1].externalReference = -101; // Pen identity is not a surface key.
    std::string error;
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
    snapshot.materialIdentity[1].externalReference = 999; // Also need not be in the project surface catalog.
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
    snapshot.meshes.front ().triMaterial.front () = 0;         // No material, never pool entry 1.
    snapshot.meshes.front ().triSurfaceAttribute.front () = 0; // Unknown, never inferred from pool identity.
    snapshot.materialIdentity.clear ();
    snapshot.surfaceAttributes.clear ();
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
    snapshot.meshes.front ().triMaterial.front () = -1;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, {}, error));
}

TEST (GeometryRustValue, DetachedValueValidationIsNotTheHostPublicationGate)
{
    auto snapshot = Fixture ();
    std::string error;
    snapshot.id = 0; // Host cached/current consumers independently require a real ID.
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
    snapshot.captureStamp = 0; // Direct complete captures may be detached from model-watch.
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, {}, error)) << error;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 7, false }, error));
}

TEST (GeometryRustValue, FilteredSnapshotRetainsUnknownCatalogsAndRawDisplayNames)
{
    auto snapshot = Fixture ();
    snapshot.completeModel = false;
    snapshot.materialIdentity.clear ();
    snapshot.surfaceAttributes.clear ();
    std::string error;
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, false }, error)) << error;
    EXPECT_FALSE (ValidateSnapshotValue (snapshot, { 7, true }, error));
    snapshot = Fixture ();
    snapshot.surfaceAttributes[42].name.clear ();
    snapshot.materialIdentity[1].name = " mixed CASE \xC3\xA9 ";
    const auto before = snapshot;
    EXPECT_TRUE (ValidateSnapshotValue (snapshot, { 7, true }, error)) << error;
    Same (snapshot, before);
}

TEST (GeometryRustValue, CLayoutAndUnknownFlagAreChecked)
{
    static_assert (sizeof (TapiocaAabb) == 48);
    static_assert (sizeof (TapiocaMeshValueView) == 200);
    static_assert (sizeof (TapiocaSnapshotValueView) == 104);
    static_assert (sizeof (TapiocaSnapshotValueRequest) == 120);
    TapiocaSnapshotValueRequest request {};
    request.requireCatalogs = 2;
    char error[256] {};
    EXPECT_EQ (tapioca_validate_snapshot_value (&request, error, sizeof (error)), TAPIOCA_ERR_ARG);
}
