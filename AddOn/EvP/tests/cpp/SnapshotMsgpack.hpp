#ifndef EVP_TESTS_SNAPSHOTMSGPACK_HPP
#define EVP_TESTS_SNAPSHOTMSGPACK_HPP

#include "Mesh.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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
//
// Shared by test_serializer.cpp and benchmark_suntrace.cpp, which replays a
// real model's GET /meshes dump. A malformed document throws.
// ---------------------------------------------------------------------------
namespace evptest {

using geomsrv::Mesh;

class MsgpackReader {
  public:
    explicit MsgpackReader (const std::string& data) : buf (data)
    {
    }

    uint8_t Peek () const
    {
        return Byte (pos);
    }
    size_t Offset () const
    {
        return pos;
    }
    bool AtEnd () const
    {
        return pos >= buf.size ();
    }

    uint32_t ReadMapHeader ()
    {
        const uint8_t t = Take ();
        if ((t & 0xf0) == 0x80)
            return t & 0x0f;
        if (t == 0xde)
            return BE16 ();
        if (t == 0xdf)
            return BE32 ();
        throw std::runtime_error ("not a map header: " + Hex (t));
    }

    uint32_t ReadArrayHeader ()
    {
        const uint8_t t = Take ();
        if ((t & 0xf0) == 0x90)
            return t & 0x0f;
        if (t == 0xdc)
            return BE16 ();
        if (t == 0xdd)
            return BE32 ();
        throw std::runtime_error ("not an array header: " + Hex (t));
    }

    std::string ReadStr ()
    {
        const uint8_t t = Take ();
        uint32_t n = 0;
        if ((t & 0xe0) == 0xa0)
            n = t & 0x1f;
        else if (t == 0xd9)
            n = Take ();
        else if (t == 0xda)
            n = BE16 ();
        else if (t == 0xdb)
            n = BE32 ();
        else
            throw std::runtime_error ("not a str: " + Hex (t));
        Need (n);
        std::string s = buf.substr (pos, n);
        pos += n;
        return s;
    }

    uint64_t ReadUint64 ()
    {
        const uint8_t t = Take ();
        if (t < 0x80)
            return t; // positive fixint
        if (t == 0xcc)
            return Take ();
        if (t == 0xcd)
            return BE16 ();
        if (t == 0xce)
            return BE32 ();
        if (t == 0xcf)
            return BE64 ();
        throw std::runtime_error ("not a uint: " + Hex (t));
    }

    int32_t ReadInt32 ()
    {
        const uint8_t t = Take ();
        if (t < 0x80)
            return static_cast<int32_t> (t); // positive fixint
        if (t >= 0xe0)
            return static_cast<int8_t> (t); // negative fixint
        if (t == 0xd2)
            return static_cast<int32_t> (BE32 ());
        throw std::runtime_error ("not an int32: " + Hex (t));
    }

    std::string ReadBin ()
    {
        const uint8_t t = Take ();
        uint32_t n = 0;
        if (t == 0xc4)
            n = Take ();
        else if (t == 0xc5)
            n = BE16 ();
        else if (t == 0xc6)
            n = BE32 ();
        else
            throw std::runtime_error ("not a bin: " + Hex (t));
        Need (n);
        std::string s = buf.substr (pos, n);
        pos += n;
        return s;
    }

    // bin body -> a typed vector, reading the raw bytes as native little-endian,
    // exactly as numpy.frombuffer('<f8'/'<u4'/'<i4') does on the Python side.
    template <typename T> std::vector<T> ReadBinAs ()
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
        if (i >= buf.size ())
            throw std::runtime_error ("read past end");
        return static_cast<uint8_t> (buf[i]);
    }
    uint8_t Take ()
    {
        const uint8_t b = Byte (pos);
        ++pos;
        return b;
    }
    uint16_t BE16 ()
    {
        const uint16_t a = Take ();
        return static_cast<uint16_t> ((a << 8) | Take ());
    }
    uint32_t BE32 ()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | Take ();
        return v;
    }
    uint64_t BE64 ()
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | Take ();
        return v;
    }
};

// Parse one packed mesh map back into a Mesh.
inline Mesh ReadMesh (MsgpackReader& r)
{
    const uint32_t fields = r.ReadMapHeader ();
    if (fields != 6u)
        throw std::runtime_error ("a mesh map has " + std::to_string (fields) + " fields, not 6");

    Mesh m;
    for (uint32_t i = 0; i < fields; ++i) {
        const std::string key = r.ReadStr ();
        if (key == "guid")
            m.guid = r.ReadStr ();
        else if (key == "type")
            m.elemType = r.ReadInt32 ();
        else if (key == "vertices")
            m.vertices = r.ReadBinAs<double> ();
        else if (key == "normals")
            m.normals = r.ReadBinAs<float> ();
        else if (key == "triangles")
            m.triangles = r.ReadBinAs<uint32_t> ();
        else if (key == "triMaterial")
            m.triMaterial = r.ReadBinAs<int32_t> ();
        else
            throw std::runtime_error ("unexpected mesh key: " + key);
    }
    return m;
}

struct ParsedSnapshot {
    uint64_t id = 0;
    std::string scope;
    std::vector<Mesh> meshes;
};

inline ParsedSnapshot ParseSnapshot (const std::string& bytes)
{
    MsgpackReader r (bytes);
    ParsedSnapshot out;

    const uint32_t fields = r.ReadMapHeader ();
    if (fields != 3u)
        throw std::runtime_error ("a snapshot map has " + std::to_string (fields) + " fields, not 3");
    for (uint32_t i = 0; i < fields; ++i) {
        const std::string key = r.ReadStr ();
        if (key == "snapshotId")
            out.id = r.ReadUint64 ();
        else if (key == "scope")
            out.scope = r.ReadStr ();
        else if (key == "meshes") {
            const uint32_t n = r.ReadArrayHeader ();
            for (uint32_t k = 0; k < n; ++k)
                out.meshes.push_back (ReadMesh (r));
        }
        else
            throw std::runtime_error ("unexpected snapshot key: " + key);
    }
    if (!r.AtEnd ())
        throw std::runtime_error ("trailing bytes after the snapshot map");
    return out;
}

} // namespace evptest

#endif
