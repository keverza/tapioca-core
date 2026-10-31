#include "MeshFixtures.hpp"
#include "MeshSerializer.hpp"
#include "SnapshotMsgpack.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace evptest;
using geomsrv::SerializeMesh;
using geomsrv::SerializeSnapshot;

namespace {

void ExpectMeshEq (const Mesh& got, const Mesh& want)
{
    EXPECT_EQ (got.guid, want.guid);
    EXPECT_EQ (got.elemType, want.elemType);
    EXPECT_EQ (got.vertices, want.vertices);
    EXPECT_EQ (got.normals, want.normals);
    EXPECT_EQ (got.triangles, want.triangles);
    EXPECT_EQ (got.triMaterial, want.triMaterial);
}

} // namespace

TEST (MeshSerializer, SnapshotRoundTripsThroughAnIndependentDecoder)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0, 1, 1, 1, /*elemType=*/7),
                                      MakeBox ("b", 10, 2, 3, 2, 3, 4, /*elemType=*/9) },
                                    /*id=*/12345, /*scope=*/"selection");

    ParsedSnapshot got;
    ASSERT_NO_THROW (got = ParseSnapshot (SerializeSnapshot (snap)));

    EXPECT_EQ (got.id, 12345u);
    EXPECT_EQ (got.scope, "selection");
    ASSERT_EQ (got.meshes.size (), 2u);
    ExpectMeshEq (got.meshes[0], snap.meshes[0]);
    ExpectMeshEq (got.meshes[1], snap.meshes[1]);
}

TEST (MeshSerializer, NormalsSurviveAsFloat32)
{
    Mesh m = MakeBox ("n", 0, 0, 0);
    m.normals.assign (m.vertices.size (), 0.0f);
    for (size_t i = 2; i < m.normals.size (); i += 3)
        m.normals[i] = 1.0f;

    const auto parsed = ParseSnapshot (SerializeSnapshot (MakeSnapshot ({ m })));
    ASSERT_EQ (parsed.meshes.size (), 1u);
    EXPECT_EQ (parsed.meshes[0].normals, m.normals);
}

TEST (MeshSerializer, SingleMeshIsWrappedInAOneKeyMap)
{
    const Mesh m = MakeBox ("solo", 1, 2, 3);
    const std::string bytes = SerializeMesh (m);

    MsgpackReader r (bytes);
    ASSERT_EQ (r.ReadMapHeader (), 1u);
    EXPECT_EQ (r.ReadStr (), "mesh");
    const Mesh got = ReadMesh (r);
    ExpectMeshEq (got, m);
    EXPECT_TRUE (r.AtEnd ());
}

TEST (MeshSerializer, EmptySnapshotIsAValidEmptyDocument)
{
    const geomsrv::Snapshot snap;
    ParsedSnapshot got;
    ASSERT_NO_THROW (got = ParseSnapshot (SerializeSnapshot (snap)));

    EXPECT_EQ (got.id, 0u);
    EXPECT_EQ (got.scope, "all");
    EXPECT_TRUE (got.meshes.empty ());
}

TEST (MeshSerializer, MeshWithNoGeometrySerializesToEmptyBins)
{
    const auto snap = MakeSnapshot ({ MakeEmpty ("nothing") });
    const auto got = ParseSnapshot (SerializeSnapshot (snap));

    ASSERT_EQ (got.meshes.size (), 1u);
    EXPECT_EQ (got.meshes[0].guid, "nothing");
    EXPECT_TRUE (got.meshes[0].vertices.empty ());
    EXPECT_TRUE (got.meshes[0].triangles.empty ());
}

// A GUID longer than 31 bytes crosses out of fixstr into str8 — the tag-width
// boundary the writer picks by size.
TEST (MeshSerializer, LongGuidCrossesTheStrTagBoundary)
{
    for (const size_t len : { size_t { 0 }, size_t { 31 }, size_t { 32 }, size_t { 300 } }) {
        Mesh m = MakeBox (std::string (len, 'g'), 0, 0, 0);
        const auto got = ParseSnapshot (SerializeSnapshot (MakeSnapshot ({ m })));
        ASSERT_EQ (got.meshes.size (), 1u) << "len=" << len;
        EXPECT_EQ (got.meshes[0].guid.size (), len);
        EXPECT_EQ (got.meshes[0].guid, m.guid);
    }
}

TEST (MeshSerializer, NegativeElementTypeRoundTrips)
{
    const Mesh m = MakeBox ("neg", 0, 0, 0, 1, 1, 1, /*elemType=*/-42);
    const auto got = ParseSnapshot (SerializeSnapshot (MakeSnapshot ({ m })));
    ASSERT_EQ (got.meshes.size (), 1u);
    EXPECT_EQ (got.meshes[0].elemType, -42);
}

TEST (MeshSerializer, LargeSnapshotIdRoundTrips)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0) }, /*id=*/0xFFFFFFFFFFFFFFFFull);
    const auto got = ParseSnapshot (SerializeSnapshot (snap));
    EXPECT_EQ (got.id, 0xFFFFFFFFFFFFFFFFull);
}

// Truncating the buffer must make the DECODER fail cleanly. This is the
// "truncated/garbage buffer -> clean failure, no OOB read" case: the reader
// throws instead of walking off the end, and ASan would catch it if it did.
TEST (MeshSerializer, TruncatedBufferFailsCleanlyWithoutOverreading)
{
    const auto snap = MakeSnapshot ({ MakeBox ("a", 0, 0, 0) });
    const std::string full = SerializeSnapshot (snap);
    ASSERT_GT (full.size (), 8u);

    for (size_t cut : { size_t { 1 }, full.size () / 4, full.size () / 2, full.size () - 1 }) {
        const std::string chopped = full.substr (0, cut);
        EXPECT_THROW (ParseSnapshot (chopped), std::runtime_error) << "cut=" << cut;
    }
}

TEST (MeshSerializer, DegenerateMeshesSerializeAndParse)
{
    const auto snap = MakeDegenerateSnapshot ();
    ParsedSnapshot got;
    ASSERT_NO_THROW (got = ParseSnapshot (SerializeSnapshot (snap)));
    EXPECT_EQ (got.meshes.size (), snap.meshes.size ());

    // NaN does not compare equal to itself, so check the bit pattern survived.
    for (size_t i = 0; i < snap.meshes.size (); ++i) {
        ASSERT_EQ (got.meshes[i].vertices.size (), snap.meshes[i].vertices.size ());
        for (size_t k = 0; k < snap.meshes[i].vertices.size (); ++k) {
            const double a = got.meshes[i].vertices[k], b = snap.meshes[i].vertices[k];
            EXPECT_EQ (std::memcmp (&a, &b, sizeof (double)), 0)
                << "mesh " << i << " vertex " << k;
        }
    }
}
