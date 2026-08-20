#include "MeshSerializer.hpp"

// Minimal msgpack writer. We hand-roll the few type tags we need instead of
// pulling in msgpack-cxx, which clashes with Archicad's /Zc:wchar_t- ABI.
// msgpack integers in headers are BIG-endian; bin *bodies* are opaque raw bytes
// (we write native little-endian f64/u32/i32 and the Python client reads them
// with numpy '<f8'/'<u4'/'<i4').
namespace geomsrv {

namespace {

struct Writer {
    std::string s;

    void U8  (uint8_t b)  { s.push_back (static_cast<char> (b)); }
    void BE16 (uint16_t v) { U8 (v >> 8); U8 (v & 0xff); }
    void BE32 (uint32_t v) { U8 (v >> 24); U8 (v >> 16); U8 (v >> 8); U8 (v); }
    void BE64 (uint64_t v) { for (int i = 7; i >= 0; --i) U8 (static_cast<uint8_t> (v >> (i * 8))); }
    void Raw (const char* p, size_t n) { s.append (p, n); }

    void MapHeader (uint32_t n)
    {
        if (n < 16)          U8 (0x80 | n);
        else if (n < 0x10000) { U8 (0xde); BE16 (static_cast<uint16_t> (n)); }
        else                 { U8 (0xdf); BE32 (n); }
    }
    void ArrayHeader (uint32_t n)
    {
        if (n < 16)          U8 (0x90 | n);
        else if (n < 0x10000) { U8 (0xdc); BE16 (static_cast<uint16_t> (n)); }
        else                 { U8 (0xdd); BE32 (n); }
    }
    void Str (const std::string& str)
    {
        const size_t n = str.size ();
        if (n < 32)           U8 (0xa0 | static_cast<uint8_t> (n));
        else if (n < 0x100)   { U8 (0xd9); U8 (static_cast<uint8_t> (n)); }
        else if (n < 0x10000) { U8 (0xda); BE16 (static_cast<uint16_t> (n)); }
        else                  { U8 (0xdb); BE32 (static_cast<uint32_t> (n)); }
        Raw (str.data (), n);
    }
    void Uint (uint64_t v) { U8 (0xcf); BE64 (v); }        // uint64
    void Int32 (int32_t v) { U8 (0xd2); BE32 (static_cast<uint32_t> (v)); }
    template <typename T>
    void Bin (const std::vector<T>& v)                     // bin32 of raw bytes
    {
        const uint32_t bytes = static_cast<uint32_t> (v.size () * sizeof (T));
        U8 (0xc6); BE32 (bytes);
        Raw (reinterpret_cast<const char*> (v.data ()), bytes);
    }
};

void PackMesh (Writer& w, const Mesh& m)
{
    w.MapHeader (6);
    w.Str ("guid");        w.Str (m.guid);
    w.Str ("type");        w.Int32 (m.elemType);
    w.Str ("vertices");    w.Bin (m.vertices);
    w.Str ("normals");     w.Bin (m.normals);
    w.Str ("triangles");   w.Bin (m.triangles);
    w.Str ("triMaterial"); w.Bin (m.triMaterial);
}

} // namespace

std::string SerializeSnapshot (const Snapshot& snap)
{
    Writer w;
    w.MapHeader (3);
    w.Str ("snapshotId"); w.Uint (snap.id);
    w.Str ("scope");      w.Str (snap.scope);
    w.Str ("meshes");     w.ArrayHeader (static_cast<uint32_t> (snap.meshes.size ()));
    for (const auto& m : snap.meshes)
        PackMesh (w, m);
    return std::move (w.s);
}

std::string SerializeMesh (const Mesh& mesh)
{
    Writer w;
    w.MapHeader (1);
    w.Str ("mesh");
    PackMesh (w, mesh);
    return std::move (w.s);
}

} // namespace geomsrv
