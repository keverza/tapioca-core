#ifndef EVP_TESTS_MESHFIXTURES_HPP
#define EVP_TESTS_MESHFIXTURES_HPP

#include "Mesh.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

// Shared mesh builders for the L2 geometry tests.
//
// TWO KINDS of fixture live here, and the distinction matters (CLAUDE.md: never
// validate a geometry algorithm on synthetic data alone):
//
//   1. ANALYTIC solids — a box whose slice/clash/ray answers are known from
//      arithmetic, not from a previous run of this code. These are regression and
//      correctness anchors, not a substitute for real model data.
//   2. DEGENERATE inputs — empty, single-triangle, sliver, duplicate-vertex,
//      NaN/Inf. A real Archicad dump will never contain most of these, which is
//      exactly why they must be constructed: they are the crash cases.
//
// What is NOT here: a real captured snapshot. Replaying a real dump is L3 and
// needs a mesh-level snapshot fixture, which this clone does not have — see
// docs/guides/testing.md §2/§3. LoadSnapshotMsgpack (test_serializer.cpp) is the reader such
// a fixture would use.
namespace evptest {

using geomsrv::Aabb;
using geomsrv::Mesh;
using geomsrv::Snapshot;

// Recompute the AABB from the vertices, the way GeometryExtractor does.
inline void RecomputeBounds (Mesh& m)
{
    m.bounds = Aabb {};
    for (size_t i = 0; i + 2 < m.vertices.size (); i += 3)
        m.bounds.Expand (m.vertices[i], m.vertices[i + 1], m.vertices[i + 2]);
}

// Axis-aligned box [ox,ox+sx] x [oy,oy+sy] x [oz,oz+sz], closed, outward-facing.
// 8 vertices, 12 triangles. Winding is counter-clockwise seen from outside, so
// the geometric normal points out — RaycastAll's enter/exit flag depends on it.
inline Mesh MakeBox (const std::string& guid,
                     double ox, double oy, double oz,
                     double sx = 1.0, double sy = 1.0, double sz = 1.0,
                     int32_t elemType = 1)
{
    Mesh m;
    m.guid = guid;
    m.elemType = elemType;

    const double x0 = ox, x1 = ox + sx;
    const double y0 = oy, y1 = oy + sy;
    const double z0 = oz, z1 = oz + sz;

    m.vertices = {
        x0, y0, z0,   // 0
        x1, y0, z0,   // 1
        x1, y1, z0,   // 2
        x0, y1, z0,   // 3
        x0, y0, z1,   // 4
        x1, y0, z1,   // 5
        x1, y1, z1,   // 6
        x0, y1, z1,   // 7
    };

    m.triangles = {
        0, 2, 1,  0, 3, 2,   // bottom (-Z)
        4, 5, 6,  4, 6, 7,   // top    (+Z)
        0, 1, 5,  0, 5, 4,   // front  (-Y)
        1, 2, 6,  1, 6, 5,   // right  (+X)
        2, 3, 7,  2, 7, 6,   // back   (+Y)
        3, 0, 4,  3, 4, 7,   // left   (-X)
    };

    m.triMaterial.assign (m.triangles.size () / 3, 0);
    RecomputeBounds (m);
    return m;
}

// A box with no per-vertex normals — QueryEngine must fall back to the geometric
// face normal. MakeBox already omits normals; this names the intent at call sites.
inline Mesh MakeBoxWithoutNormals (const std::string& guid, double ox, double oy, double oz,
                                   double s = 1.0)
{
    return MakeBox (guid, ox, oy, oz, s, s, s);
}

// ---- degenerate inputs -----------------------------------------------------

inline Mesh MakeEmpty (const std::string& guid = "empty")
{
    Mesh m;
    m.guid = guid;
    return m;   // no vertices, no triangles, invalid (default) AABB
}

// One triangle in the z=0 plane.
inline Mesh MakeSingleTriangle (const std::string& guid = "tri")
{
    Mesh m;
    m.guid = guid;
    m.vertices = { 0,0,0,  1,0,0,  0,1,0 };
    m.triangles = { 0, 1, 2 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

// Zero-area: three collinear points. Every normal/area computation divides by 0.
inline Mesh MakeZeroAreaTriangle (const std::string& guid = "zeroarea")
{
    Mesh m;
    m.guid = guid;
    m.vertices = { 0,0,0,  1,0,0,  2,0,0 };
    m.triangles = { 0, 1, 2 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

// Sliver: technically non-degenerate, but area ~1e-12 — the case that turns into
// a zero-area triangle after normalisation.
inline Mesh MakeSliverTriangle (const std::string& guid = "sliver")
{
    Mesh m;
    m.guid = guid;
    m.vertices = { 0,0,0,  1,0,0,  0.5, 1e-12, 0 };
    m.triangles = { 0, 1, 2 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

// All three indices reference the same vertex.
inline Mesh MakeDuplicateVertexTriangle (const std::string& guid = "dupvert")
{
    Mesh m;
    m.guid = guid;
    m.vertices = { 1,2,3,  1,2,3,  1,2,3 };
    m.triangles = { 0, 1, 2 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

inline Mesh MakeNaNTriangle (const std::string& guid = "nan")
{
    Mesh m;
    m.guid = guid;
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    m.vertices = { 0,0,0,  1,0,0,  nan, nan, nan };
    m.triangles = { 0, 1, 2 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

inline Mesh MakeInfTriangle (const std::string& guid = "inf")
{
    Mesh m;
    m.guid = guid;
    const double inf = std::numeric_limits<double>::infinity ();
    m.vertices = { 0,0,0,  1,0,0,  inf, 0, 0 };
    m.triangles = { 0, 1, 2 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

// Triangle indices pointing past the end of `vertices`. Nothing upstream should
// ever produce this, which is why an engine must not read out of bounds if it does.
inline Mesh MakeOutOfRangeIndices (const std::string& guid = "oob")
{
    Mesh m;
    m.guid = guid;
    m.vertices = { 0,0,0,  1,0,0,  0,1,0 };
    m.triangles = { 0, 1, 99 };
    m.triMaterial = { 0 };
    RecomputeBounds (m);
    return m;
}

// ---- snapshots -------------------------------------------------------------

inline Snapshot MakeSnapshot (std::vector<Mesh> meshes, uint64_t id = 1,
                              const std::string& scope = "all")
{
    Snapshot s;
    s.id = id;
    s.scope = scope;
    s.meshes = std::move (meshes);
    return s;
}

// Every degenerate mesh in one snapshot — the "does the whole pipeline survive
// garbage" case each engine's suite runs.
inline Snapshot MakeDegenerateSnapshot ()
{
    return MakeSnapshot ({
        MakeEmpty (),
        MakeSingleTriangle (),
        MakeZeroAreaTriangle (),
        MakeSliverTriangle (),
        MakeDuplicateVertexTriangle (),
        MakeNaNTriangle (),
        MakeInfTriangle (),
    });
}

} // namespace evptest

#endif
