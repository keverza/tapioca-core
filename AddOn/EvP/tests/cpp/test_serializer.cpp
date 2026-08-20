#include "MeshFixtures.hpp"
#include "MeshSerializer.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace evptest;
using geomsrv::SerializeMesh;
using geomsrv::SerializeSnapshot;

// ---------------------------------------------------------------------------
// An INDEPENDENT msgpack reader, written from the msgpack spec — deliberately
// not the writer's mirror image and not the vendored msgpack-c. If both halves
// shared an implementation (or an author's misreading of the format), a
// round-trip test would only prove self-consistency. This one can actually fail
// when the writer emits something a real msgpack decoder cannot read, which is
// the failure that would reach the Python client.
//
// Only the tags the serializer emits are implemented; anything else throws, so
// an unexpected tag is a loud failure rather than a silent skip.
// ---------------------------------------------------------------------------
namespace {

class MsgpackReader {
public:
    explicit MsgpackReader (const std::string& data) : buf (data) {}

    uint8_t Peek () const { return Byte (pos); }
    size_t  Offset () const { return pos; }
    bool    AtEnd () const { return pos >= buf.size (); }

    uint32_t ReadMapHeader ()
    {
        const uint8_t t = Take ();
        if ((t & 0xf0) == 0x80) return t & 0x0f;
        if (t == 0xde) return BE16 ();
        if (t == 0xdf) return BE32 ();
        throw std::runtime_error ("not a map header: " + Hex (t));
    }

    uint32_t ReadArrayHeader ()
    {
        const uint8_t t = Take ();
        if ((t & 0xf0) == 0x90) return t & 0x0f;
        if (t == 0xdc) return BE16 ();
        if (t == 0xdd) return BE32 ();
        throw std::runtime_error ("not an array header: " + Hex (t));
    }

    std::string ReadStr ()
    {
        const uint8_t t = Take ();
        uint32_t n = 0;
        if ((t & 0xe0) == 0xa0)      n = t & 0x1f;
        else if (t == 0xd9)          n = Take ();
        else if (t == 0xda)          n = BE16 ();
        else if (t == 0xdb)          n = BE32 ();
        else throw std::runtime_error ("not a str: " + Hex (t));
        Need (n);
        std::string s = buf.substr (pos, n);
        pos += n;
        return s;
    }

    uint64_t ReadUint64 ()
    {
        const uint8_t t = Take ();
        if (t < 0x80) return t;                       // positive fixint
        if (t == 0xcc) return Take ();
        if (t == 0xcd) return BE16 ();
        if (t == 0xce) return BE32 ();
        if (t == 0xcf) return BE64 ();
        throw std::runtime_error ("not a uint: " + Hex (t));
    }

    int32_t ReadInt32 ()
    {
        const uint8_t t = Take ();
        if (t < 0x80) return static_cast<int32_t> (t);            // positive fixint
        if (t >= 0xe0) return static_cast<int8_t> (t);            // negative fixint
        if (t == 0xd2) return static_cast<int32_t> (BE32 ());
        throw std::runtime_error ("not an int32: " + Hex (t));
    }

    std::string ReadBin ()
    {
        const uint8_t t = Take ();
        uint32_t n = 0;
        if (t == 0xc4)      n = Take ();
        else if (t == 0xc5) n = BE16 ();
        else if (t == 0xc6) n = BE32 ();
        else throw std::runtime_error ("not a bin: " + Hex (t));
        Need (n);
        std::string s = buf.substr (pos, n);
        pos += n;
        return s;
    }

    // bin body -> a typed vector, reading the raw bytes as native little-endian,
    // exactly as numpy.frombuffer('<f8'/'<u4'/'<i4') does on the Python side.
    template <typename T>
    std::vector<T> ReadBinAs ()
    {
        const std::string raw = ReadBin ();
        if (raw.size () % sizeof (T) != 0)
            throw std::runtime_error ("bin length is not a multiple of the element size");
        std::vector<T> out (raw.size () / sizeof (T));
        if (!out.empty ())
            std::memcpy (out.data (), raw.data (), raw.size ());
        return out;
    }

private:
    const std::string& buf;
    size_t pos = 0;

    static std::string Hex (uint8_t b)
    {
        static const char* d = "0123456789abcdef";
        return std::string ("0x") + d[b >> 4] + d[b & 0xf];
    }
    void Need (size_t n) const
    {
        if (pos + n > buf.size ())
            throw std::runtime_error ("truncated msgpack buffer");
    }
    uint8_t Byte (size_t i) const
    {
        if (i >= buf.size ()) throw std::runtime_error ("read past end");
        return static_cast<uint8_t> (buf[i]);
    }
    uint8_t Take () { const uint8_t b = Byte (pos); ++pos; return b; }
    uint16_t BE16 () { const uint16_t a = Take (); return static_cast<uint16_t> ((a << 8) | Take ()); }
    uint32_t BE32 ()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | Take ();
        return v;
    }
    uint64_t BE64 ()
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | Take ();
        return v;
    }
};

// Parse one packed mesh map back into a Mesh.
Mesh ReadMesh (MsgpackReader& r)
{
    const uint32_t fields = r.ReadMapHeader ();
    EXPECT_EQ (fields, 6u);

    Mesh m;
    for (uint32_t i = 0; i < fields; ++i) {
        const std::string key = r.ReadStr ();
        if (key == "guid")             m.guid = r.ReadStr ();
        else if (key == "type")        m.elemType = r.ReadInt32 ();
        else if (key == "vertices")    m.vertices = r.ReadBinAs<double> ();
        else if (key == "normals")     m.normals = r.ReadBinAs<float> ();
        else if (key == "triangles")   m.triangles = r.ReadBinAs<uint32_t> ();
        else if (key == "triMaterial") m.triMaterial = r.ReadBinAs<int32_t> ();
        else throw std::runtime_error ("unexpected mesh key: " + key);
    }
    return m;
}

struct ParsedSnapshot {
    uint64_t          id = 0;
    std::string       scope;
    std::vector<Mesh> meshes;
};

ParsedSnapshot ParseSnapshot (const std::string& bytes)
{
    MsgpackReader r (bytes);
    ParsedSnapshot out;

    const uint32_t fields = r.ReadMapHeader ();
    EXPECT_EQ (fields, 3u);
    for (uint32_t i = 0; i < fields; ++i) {
        const std::string key = r.ReadStr ();
        if (key == "snapshotId")  out.id = r.ReadUint64 ();
        else if (key == "scope")  out.scope = r.ReadStr ();
        else if (key == "meshes") {
            const uint32_t n = r.ReadArrayHeader ();
            for (uint32_t k = 0; k < n; ++k)
                out.meshes.push_back (ReadMesh (r));
        } else throw std::runtime_error ("unexpected snapshot key: " + key);
    }
    EXPECT_TRUE (r.AtEnd ()) << "trailing bytes after the snapshot map";
    return out;
}

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
