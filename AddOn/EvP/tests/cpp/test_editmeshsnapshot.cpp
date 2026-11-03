// #139 M0 stage 1: the common published-mesh snapshot. These tests pin the
// contract: closed conversions to/from geomsrv::Mesh (fails closed, never
// guesses), recomputed bounds, identity-ordering source remaps, the
// geometry-only deterministic content hash, and the derived editor store —
// including the invariant that publishing editor geometry never touches the
// native-capture MeshStore.
#include "Edit/EditMeshStore.hpp"
#include "Edit/EditPublishedMesh.hpp"

#include "Geometry/MeshStore.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace lg = tapioca::geometry;

namespace {

lg::PublishedMesh Box ()
{
    lg::PublishedMesh mesh;
    std::string error;
    EXPECT_TRUE (lg::FromGeomsrvMesh (evptest::MakeBox ("box", 0, 0, 0), mesh, error));
    return mesh;
}

} // namespace

TEST (EditPublishedMesh, FromGeomsrvBoxCarriesGeometryIdentityAndHealth)
{
    auto mesh = Box ();
    EXPECT_TRUE (mesh.valid);
    EXPECT_TRUE (mesh.diagnostics.empty ());
    EXPECT_EQ (mesh.object, "box");
    EXPECT_EQ (mesh.VertexCount (), 8);
    EXPECT_EQ (mesh.TriangleCount (), 12);
    // Bounds recomputed from positions, not copied from the source AABB.
    ASSERT_TRUE (mesh.bounds.Valid ());
    EXPECT_DOUBLE_EQ (mesh.bounds.mn[0], 0.0);
    EXPECT_DOUBLE_EQ (mesh.bounds.mx[0], 1.0);
    // Identity-ordering remaps for a full-resolution publish.
    ASSERT_EQ (mesh.vertexSource.size (), 8);
    ASSERT_EQ (mesh.triangleSource.size (), 12);
    for (uint32_t i = 0; i < 8; ++i)
        EXPECT_EQ (mesh.vertexSource[i], i);
    for (uint32_t i = 0; i < 12; ++i)
        EXPECT_EQ (mesh.triangleSource[i], i);
}

TEST (EditPublishedMesh, GeomsrvRoundTripPreservesGeometry)
{
    auto mesh = Box ();
    mesh.normals.assign (mesh.positions.size (), 0.0f);
    for (size_t i = 0; i + 2 < mesh.positions.size (); i += 3) {
        mesh.normals[i] = 0.0f;
        mesh.normals[i + 2] = 1.0f; // upright, normalised
    }
    mesh.triMaterial.assign (mesh.TriangleCount (), 7);

    geomsrv::Mesh converted;
    std::string error;
    ASSERT_TRUE (lg::ToGeomsrvMesh (mesh, converted, error));
    EXPECT_EQ (converted.guid, "box");
    EXPECT_EQ (converted.elemType, 1);
    EXPECT_EQ (converted.vertices, mesh.positions);
    EXPECT_EQ (converted.normals, mesh.normals);
    EXPECT_EQ (converted.triangles, mesh.triangles);
    ASSERT_EQ (converted.triMaterial.size (), mesh.TriangleCount ());
    EXPECT_EQ (converted.triMaterial.front (), 7);

    lg::PublishedMesh back;
    ASSERT_TRUE (lg::FromGeomsrvMesh (converted, back, error));
    EXPECT_EQ (back.positions, mesh.positions);
    EXPECT_EQ (back.normals, mesh.normals);
    EXPECT_EQ (back.triangles, mesh.triangles);
    EXPECT_EQ (back.triMaterial, mesh.triMaterial);
}

TEST (EditPublishedMesh, ConversionFailsClosedOnMalformedInput)
{
    lg::PublishedMesh mesh = Box ();
    std::string error;

    {
        geomsrv::Mesh bad = evptest::MakeOutOfRangeIndices ();
        lg::PublishedMesh out;
        EXPECT_FALSE (lg::FromGeomsrvMesh (bad, out, error));
        EXPECT_FALSE (error.empty ());
    }
    {
        geomsrv::Mesh bad = evptest::MakeNaNTriangle ();
        lg::PublishedMesh out;
        EXPECT_FALSE (lg::FromGeomsrvMesh (bad, out, error));
        EXPECT_FALSE (error.empty ());
    }
    {
        geomsrv::Mesh bad = evptest::MakeBox ("bad", 0, 0, 0);
        bad.triangles.push_back (99); // cardinality break
        lg::PublishedMesh out;
        EXPECT_FALSE (lg::FromGeomsrvMesh (bad, out, error));
        EXPECT_FALSE (error.empty ());
    }
    {
        lg::PublishedMesh bad = mesh;
        bad.triangles[0] = 42; // out of range on the published side
        geomsrv::Mesh out;
        EXPECT_FALSE (lg::ToGeomsrvMesh (bad, out, error));
        EXPECT_FALSE (error.empty ());
    }
    // The validated mesh is untouched by every refusal above.
    EXPECT_TRUE (mesh.valid);
    EXPECT_EQ (mesh.VertexCount (), 8);
}

TEST (EditPublishedMesh, ContentHashCoversGeometryAndIsDeterministic)
{
    auto a = Box ();
    auto b = Box ();
    EXPECT_EQ (lg::HashPublishedMesh (a), lg::HashPublishedMesh (b));

    // Identity is not content: another object with the same geometry hashes equal.
    b.object = "other-object";
    b.revision = 9;
    EXPECT_EQ (lg::HashPublishedMesh (a), lg::HashPublishedMesh (b));

    // Every geometry-affecting channel changes the hash.
    b = Box ();
    b.positions[0] += 1e-9;
    EXPECT_NE (lg::HashPublishedMesh (a), lg::HashPublishedMesh (b));
    b = Box ();
    b.triangles[0] = 3;
    EXPECT_NE (lg::HashPublishedMesh (a), lg::HashPublishedMesh (b));
    b = Box ();
    b.triMaterial[0] = 5;
    EXPECT_NE (lg::HashPublishedMesh (a), lg::HashPublishedMesh (b));
}

TEST (EditMeshStore, PublishesByObjectAndKeepsNativeCaptureStoreUntouched)
{
    auto& store = lg::EditorMeshStore::Get ();
    store.Release ();
    ASSERT_TRUE (store.Objects ().empty ());

    auto mesh = Box ();
    mesh.object = "mesh/a";
    auto published = std::make_shared<const lg::PublishedMesh> (mesh);
    store.Publish (published);

    auto fetched = store.Get ("mesh/a");
    ASSERT_TRUE (fetched != nullptr);
    EXPECT_EQ (fetched->VertexCount (), 8);
    EXPECT_TRUE (store.Get ("mesh/missing") == nullptr);
    ASSERT_EQ (store.Objects ().size (), 1);
    EXPECT_EQ (store.Objects ().front (), "mesh/a");
    EXPECT_GT (store.Bytes (), 0);

    // Atomic replacement, not mutation: the reader's snapshot survives.
    auto replacementMesh = Box ();
    replacementMesh.object = "mesh/a";
    auto replacement = std::make_shared<const lg::PublishedMesh> (replacementMesh);
    store.Publish (replacement);
    EXPECT_EQ (store.Get ("mesh/a")->VertexCount (), 8);
    EXPECT_EQ (fetched.use_count (), 2) << "Old snapshot stays alive for its readers.";

    store.Remove ("mesh/a");
    EXPECT_TRUE (store.Get ("mesh/a") == nullptr);

    // The native-capture authority was never touched by editor publication.
    EXPECT_TRUE (geomsrv::MeshStore::Get ().Current () == nullptr);
    EXPECT_TRUE (geomsrv::MeshStore::Get ().Shared () == nullptr);
    store.Release ();
}
