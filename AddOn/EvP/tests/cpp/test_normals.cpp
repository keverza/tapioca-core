#include "VertexWeld.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using geomsrv::ComputeSmoothNormals;
using geomsrv::Mesh;
using geomsrv::NormalizeVector;
using geomsrv::NormalsAreUsable;
using geomsrv::TriangleNormal;
using geomsrv::VertexWelder;

// ---------------------------------------------------------------------------
// These tests pin the shading bug that made every Archicad box render as a
// smeared blob in the three.js/WebGi viewers: ModelerAPI hands out ONE shared
// vertex list per body, and averaging adjacent face normals into it gives a
// cube's corners diagonal normals. Interpolated across a face, that is sphere
// shading — and the N.L term goes negative while the face is still lit.
//
// A cube is the whole argument, so the fixture is a cube with the same shared
// 8-vertex topology ModelerAPI produces.
// ---------------------------------------------------------------------------

namespace {

// Unit cube [0,1]^3, 8 shared vertices, 12 triangles, outward winding — the
// topology ModelerAPI::MeshBody gives us for a box.
Mesh SharedVertexCube ()
{
    Mesh m;
    m.vertices = {
        0,0,0,  1,0,0,  1,1,0,  0,1,0,   // z = 0
        0,0,1,  1,0,1,  1,1,1,  0,1,1,   // z = 1
    };
    m.triangles = {
        0,3,2, 0,2,1,   // bottom  (-Z)
        4,5,6, 4,6,7,   // top     (+Z)
        0,1,5, 0,5,4,   // front   (-Y)
        1,2,6, 1,6,5,   // right   (+X)
        2,3,7, 2,7,6,   // back    (+Y)
        3,0,4, 3,4,7,   // left    (-X)
    };
    m.triMaterial.assign (m.triangles.size () / 3, 0);
    return m;
}

// The six outward face normals, indexed the same way as the triangle groups
// above (2 triangles per face).
const double kFaceNormals[6][3] = {
    { 0,  0, -1},   // bottom
    { 0,  0,  1},   // top
    { 0, -1,  0},   // front
    { 1,  0,  0},   // right
    { 0,  1,  0},   // back
    {-1,  0,  0},   // left
};

double Dot (const float* n, const double* m)
{
    return n[0]*m[0] + n[1]*m[1] + n[2]*m[2];
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The bug, pinned. ComputeSmoothNormals survives only as a fallback, so its
//    wrongness on hard edges is a documented property, not a regression.
// ---------------------------------------------------------------------------
TEST (SmoothNormals, WeldedCubeCornersPointDiagonally)
{
    Mesh m = SharedVertexCube ();
    ComputeSmoothNormals (m);

    ASSERT_EQ (m.normals.size (), m.vertices.size ());

    // Vertex 0 is a corner of three faces (-X, -Y, -Z), so its normal points
    // diagonally inward-ish instead of along any of them.
    //
    // It is NOT the symmetric (-1,-1,-1)/sqrt(3): the weights are unequal.
    // Each quad face is fan-triangulated into two triangles, and a given corner
    // belongs to both of them on some faces but only one on others — vertex 0
    // is in both bottom triangles and both front triangles, but only in one of
    // the two left-face triangles. Area weighting therefore counts the left
    // face half as much, giving (-1,-2,-2)/3. The exact number matters less
    // than the fact that it is nowhere near a face normal.
    EXPECT_NEAR (m.normals[0], -1.0/3.0, 1e-6);
    EXPECT_NEAR (m.normals[1], -2.0/3.0, 1e-6);
    EXPECT_NEAR (m.normals[2], -2.0/3.0, 1e-6);

    // The damage in one number: every vertex normal is more than 30 degrees off
    // every face it belongs to. Interpolate that across a face and the surface
    // shades like a sphere — and N.L flips sign while the face is still lit,
    // which is the "sun looks like it is underground" symptom.
    for (size_t v = 0; v + 2 < m.normals.size (); v += 3) {
        double best = 0.0;
        for (const auto& f : kFaceNormals)
            best = std::max (best, Dot (&m.normals[v], f));
        EXPECT_LT (best, std::cos (30.0 * 3.14159265358979 / 180.0))
            << "vertex " << v/3 << " should NOT match a face normal";
    }
}

// ---------------------------------------------------------------------------
// 2. The fix. Feeding per-corner normals through the welder splits the shared
//    vertices, and every emitted normal is exactly its face's normal.
// ---------------------------------------------------------------------------
TEST (VertexWeld, PerCornerNormalsSplitCubeIntoFlatFaces)
{
    const Mesh src = SharedVertexCube ();

    Mesh out;
    VertexWelder welder (out);

    // Replay what GeometryExtractor does: for each triangle corner, hand the
    // welder the source vertex plus the normal Archicad reports there (for a
    // cube, the face normal).
    for (size_t t = 0; t + 2 < src.triangles.size (); t += 3) {
        const double* fn = kFaceNormals[(t / 3) / 2];
        for (int k = 0; k < 3; ++k) {
            const uint32_t sv = src.triangles[t + k];
            out.triangles.push_back (
                welder.Add (static_cast<int32_t> (sv),
                            src.vertices[sv*3], src.vertices[sv*3+1], src.vertices[sv*3+2],
                            fn[0], fn[1], fn[2]));
        }
    }

    // 8 shared vertices x 3 faces each = 24. Not 8 (would smooth the edges)
    // and not 36 (would mean the welder deduplicates nothing).
    EXPECT_EQ (out.VertexCount (), 24u);
    EXPECT_EQ (out.triangles.size (), src.triangles.size ());
    ASSERT_EQ (out.normals.size (), out.vertices.size ());
    EXPECT_TRUE (NormalsAreUsable (out));

    // Every normal is axis-aligned and unit — a flat-shaded cube.
    for (size_t v = 0; v + 2 < out.normals.size (); v += 3) {
        double best = -2.0;
        for (const auto& f : kFaceNormals)
            best = std::max (best, Dot (&out.normals[v], f));
        EXPECT_NEAR (best, 1.0, 1e-6) << "vertex " << v/3 << " is not a face normal";
    }

    // Positions must be preserved exactly — this is a shading fix, and the
    // surface, the raycaster and the clash results must not move.
    for (size_t t = 0; t < src.triangles.size (); ++t) {
        const uint32_t s = src.triangles[t];
        const uint32_t o = out.triangles[t];
        EXPECT_DOUBLE_EQ (out.vertices[o*3],     src.vertices[s*3]);
        EXPECT_DOUBLE_EQ (out.vertices[o*3 + 1], src.vertices[s*3 + 1]);
        EXPECT_DOUBLE_EQ (out.vertices[o*3 + 2], src.vertices[s*3 + 2]);
    }
}

// ---------------------------------------------------------------------------
// 3. A curved surface must stay welded, or we would trade sphere-shaded boxes
//    for faceted cylinders. Same source vertex + same normal = one vertex.
// ---------------------------------------------------------------------------
TEST (VertexWeld, MatchingNormalsShareAVertex)
{
    Mesh m;
    VertexWelder welder (m);

    const uint32_t a = welder.Add (7, 1.0, 2.0, 3.0, 0.0, 0.0, 1.0);
    const uint32_t b = welder.Add (7, 1.0, 2.0, 3.0, 0.0, 0.0, 1.0);
    EXPECT_EQ (a, b);
    EXPECT_EQ (m.VertexCount (), 1u);

    // Below the quantum (1e-4) still counts as the same normal, so tessellation
    // noise does not shatter a smooth surface into unique vertices.
    const uint32_t c = welder.Add (7, 1.0, 2.0, 3.0, 1e-6, 0.0, 1.0);
    EXPECT_EQ (a, c);
    EXPECT_EQ (m.VertexCount (), 1u);

    // A genuinely different normal splits.
    const uint32_t d = welder.Add (7, 1.0, 2.0, 3.0, 1.0, 0.0, 0.0);
    EXPECT_NE (a, d);
    EXPECT_EQ (m.VertexCount (), 2u);

    // A different source vertex splits even at an identical normal — two
    // bodies can put distinct corners at the same place.
    const uint32_t e = welder.Add (8, 1.0, 2.0, 3.0, 0.0, 0.0, 1.0);
    EXPECT_NE (a, e);
    EXPECT_EQ (m.VertexCount (), 3u);
}

TEST (VertexWeld, ResetSeparatesBodies)
{
    Mesh m;
    VertexWelder welder (m);
    const uint32_t a = welder.Add (0, 0, 0, 0, 0, 0, 1);
    welder.Reset ();
    const uint32_t b = welder.Add (0, 9, 9, 9, 0, 0, 1);
    EXPECT_NE (a, b) << "body-local indices must not collide across bodies";
    EXPECT_EQ (m.VertexCount (), 2u);
    EXPECT_DOUBLE_EQ (m.vertices[3], 9.0);
}

// ---------------------------------------------------------------------------
// 4. Helpers used by the extractor's degenerate-corner fallback.
// ---------------------------------------------------------------------------
TEST (NormalHelpers, NormalizeRejectsZeroLength)
{
    double x = 0.0, y = 0.0, z = 0.0;
    EXPECT_FALSE (NormalizeVector (x, y, z));
    EXPECT_DOUBLE_EQ (x, 0.0);   // left untouched, not turned into NaN

    x = 0.0; y = 0.0; z = 5.0;
    EXPECT_TRUE (NormalizeVector (x, y, z));
    EXPECT_DOUBLE_EQ (z, 1.0);
}

TEST (NormalHelpers, TriangleNormalFollowsWinding)
{
    const double a[3] = {0, 0, 0};
    const double b[3] = {1, 0, 0};
    const double c[3] = {0, 1, 0};
    double nx, ny, nz;
    TriangleNormal (a, b, c, nx, ny, nz);
    ASSERT_TRUE (NormalizeVector (nx, ny, nz));
    EXPECT_NEAR (nz, 1.0, 1e-12);   // CCW seen from +Z

    TriangleNormal (a, c, b, nx, ny, nz);   // reversed winding
    ASSERT_TRUE (NormalizeVector (nx, ny, nz));
    EXPECT_NEAR (nz, -1.0, 1e-12);
}

TEST (NormalHelpers, UsableRejectsShortAndMismatchedNormals)
{
    Mesh m;
    m.vertices = {0,0,0};
    m.normals  = {0.0f, 0.0f, 0.0f};
    EXPECT_FALSE (NormalsAreUsable (m)) << "a zero normal is not usable";

    m.normals = {0.0f, 0.0f, 1.0f};
    EXPECT_TRUE (NormalsAreUsable (m));

    m.normals = {0.0f, 0.0f};
    EXPECT_FALSE (NormalsAreUsable (m)) << "count must match the vertex count";

    m.normals.clear ();
    EXPECT_FALSE (NormalsAreUsable (m));
}
