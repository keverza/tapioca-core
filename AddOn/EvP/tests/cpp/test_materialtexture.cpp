// #252: Archicad material textures in the viewer -- the image fit and key (ArchViz/MaterialTexture)
// and the texture coordinates through the vertex weld (Geometry/VertexWeld, MeshExtraction).
//
// Each wrong answer here renders a plausible picture: a weld that ignores the coordinate smears one
// side of a seam across the other, a coordinate array one vertex short shifts every texture after
// it, and a fit that drops a row skews the image. So the shapes are pinned.

#include "ArchViz/MaterialTexture.hpp"
#include "Geometry/MeshExtraction.hpp"
#include "Geometry/VertexWeld.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace geomsrv;
using namespace geomsrv::archviz;

namespace {

TexturePixels Solid (uint32_t w, uint32_t h, uint8_t value)
{
    TexturePixels p;
    p.width = w;
    p.height = h;
    p.rgba.assign (size_t (w) * h * 4, value);
    return p;
}

} // namespace

TEST (MaterialTexture, FitHalvesUntilBothSidesFitAndAverages)
{
    TexturePixels p = Solid (4, 2, 0);
    // Left half white, right half black: the 2x1 result is one white and one black pixel.
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x)
            for (int c = 0; c < 4; ++c)
                p.rgba[(size_t (y) * 4 + x) * 4 + c] = 255;
    FitTexturePixels (p, 2);
    ASSERT_TRUE (p.Valid ());
    EXPECT_EQ (p.width, 2u);
    EXPECT_EQ (p.height, 1u);
    EXPECT_EQ (p.rgba[0], 255);
    EXPECT_EQ (p.rgba[4], 0);
}

TEST (MaterialTexture, FitLeavesASmallImageAndHandlesOddSides)
{
    TexturePixels small = Solid (8, 8, 7);
    FitTexturePixels (small, 2048);
    EXPECT_EQ (small.width, 8u);

    TexturePixels odd = Solid (5, 3, 100);
    FitTexturePixels (odd, 2);
    ASSERT_TRUE (odd.Valid ());
    EXPECT_EQ (odd.width, 2u); // 5x3 -> 2x1: both sides fit after one halving
    EXPECT_EQ (odd.height, 1u);
    EXPECT_EQ (odd.rgba[0], 100);
}

TEST (MaterialTexture, KeyIsNeverZeroAndTellsImagesApart)
{
    EXPECT_NE (TextureKeyOf (""), 0u);
    EXPECT_NE (TextureKeyOf ("brick"), TextureKeyOf ("concrete"));
    EXPECT_EQ (TextureKeyOf ("brick"), TextureKeyOf ("brick"));
}

TEST (MaterialTexture, WeldSplitsOnTheTextureCoordinateAndKeepsTwoPerVertex)
{
    Mesh mesh;
    VertexWelder welder (mesh);
    // Untextured first: no coordinates yet.
    const uint32_t plain = welder.Add (0, 0, 0, 0, 0, 0, 1);
    EXPECT_TRUE (mesh.uvs.empty ());
    // The same corner with a coordinate is a different vertex, and the array is back-filled.
    const double a[2] = { 1.0, 2.0 };
    const uint32_t textured = welder.Add (0, 0, 0, 0, 0, 0, 1, a);
    EXPECT_NE (plain, textured);
    ASSERT_TRUE (mesh.HasUvs ());
    EXPECT_FLOAT_EQ (mesh.uvs[0], 0.0f);
    EXPECT_FLOAT_EQ (mesh.uvs[2], 1.0f);
    EXPECT_FLOAT_EQ (mesh.uvs[3], 2.0f);
    // The same coordinate welds; another one (a seam) splits.
    EXPECT_EQ (welder.Add (0, 0, 0, 0, 0, 0, 1, a), textured);
    const double b[2] = { 1.5, 2.0 };
    EXPECT_NE (welder.Add (0, 0, 0, 0, 0, 0, 1, b), textured);
    // An untextured vertex after the first textured one still gets its pair.
    welder.Add (1, 1, 0, 0, 0, 0, 1);
    EXPECT_TRUE (mesh.HasUvs ());
    EXPECT_EQ (mesh.uvs.size (), mesh.VertexCount () * 2);
}

TEST (MaterialTexture, ExtractedConvexCarriesItsCornersCoordinates)
{
    Mesh mesh;
    VertexWelder welder (mesh);
    const std::vector<double> positions { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
    std::vector<ExtractionCorner> corners { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 2, 0, 0, 1 }, { 3, 0, 0, 1 } };
    const double uv[4][2] = { { 0, 0 }, { 3, 0 }, { 3, 2 }, { 0, 2 } };
    for (size_t k = 0; k < corners.size (); ++k) {
        corners[k].hasUv = true;
        corners[k].u = uv[k][0];
        corners[k].v = uv[k][1];
    }
    const std::vector<WireEdgeKey> edges;
    ExtractionCounts counts;
    AppendExtractedConvex (mesh, welder, positions, corners, { 1, 1, 5, 0, edges }, counts);
    ASSERT_EQ (mesh.TriangleCount (), 2u);
    ASSERT_TRUE (mesh.HasUvs ());
    for (size_t k = 0; k < 4; ++k) {
        EXPECT_FLOAT_EQ (mesh.uvs[k * 2], float (uv[k][0]));
        EXPECT_FLOAT_EQ (mesh.uvs[k * 2 + 1], float (uv[k][1]));
    }
}

TEST (MaterialTexture, TriangleStretchTellsEvenFromSqueezed)
{
    const float p0[3] = { 0, 0, 0 }, p1[3] = { 4, 0, 0 }, p2[3] = { 0, 4, 0 };
    // Archicad's top projection: one coordinate unit per metre both ways.
    const float a0[2] = { 0, 0 }, a1[2] = { 4, 0 }, a2[2] = { 0, 4 };
    UvStretch even = TriangleUvStretch (p0, p1, p2, a0, a1, a2);
    EXPECT_NEAR (even.maxPerUnit, 1.0, 1e-9);
    EXPECT_NEAR (even.minPerUnit, 1.0, 1e-9);
    // A corner paired with another vertex's coordinate: many repeats squeezed one way.
    const float b2[2] = { 0, 40 };
    UvStretch squeezed = TriangleUvStretch (p0, p1, p2, a0, a1, b2);
    EXPECT_GT (squeezed.maxPerUnit / squeezed.minPerUnit, 5.0);
    // Degenerate: zero, not a division by zero.
    UvStretch flat = TriangleUvStretch (p0, p1, p1, a0, a1, a2);
    EXPECT_EQ (flat.maxPerUnit, 0.0);
}

// Live (SimpleArchicadScene's terrain): when the FIRST vertex is the textured one, its own pair was
// skipped and the next vertex back-filled it with (0, 0) -- three triangles drew squeezed 12-24x.
TEST (MaterialTexture, WeldKeepsTheFirstVertexsOwnCoordinate)
{
    Mesh mesh;
    VertexWelder welder (mesh);
    const double a[2] = { 116.5, -228.25 };
    const double b[2] = { 88.25, -228.25 };
    welder.Add (0, 0, 0, 0, 0, 0, 1, a);
    welder.Add (1, 1, 0, 0, 0, 0, 1, b);
    welder.Add (2, 2, 0, 0, 0, 0, 1); // an untextured vertex after them still gets its pair
    ASSERT_TRUE (mesh.HasUvs ());
    EXPECT_FLOAT_EQ (mesh.uvs[0], 116.5f);
    EXPECT_FLOAT_EQ (mesh.uvs[1], -228.25f);
    EXPECT_FLOAT_EQ (mesh.uvs[2], 88.25f);
    EXPECT_FLOAT_EQ (mesh.uvs[4], 0.0f);
}

// Material::GetTextureRotationAngle: an unrotated material must stay bit-identical, and a turn
// follows Graphisoft's example renderer, (u cos - v sin, u sin + v cos), before the repeat scale.
TEST (MaterialTexture, RotationZeroAndWholeTurnsAreExactlyTheIdentity)
{
    for (double angle : { 0.0, 6.283185307179586, -6.283185307179586, 1e-9, std::nan ("") }) {
        const TextureRotation r = TextureRotationOf (angle);
        EXPECT_EQ (r.cosA, 1.0f) << angle;
        EXPECT_EQ (r.sinA, 0.0f) << angle;
    }
}

TEST (MaterialTexture, RotationTurnsTheCoordinateCounterClockwise)
{
    const TextureRotation quarter = TextureRotationOf (1.5707963267948966);
    double u = 2.0, v = 0.0;
    RotateTextureCoordinate (quarter, u, v);
    EXPECT_NEAR (u, 0.0, 1e-6);
    EXPECT_NEAR (v, 2.0, 1e-6);

    const TextureRotation negative = TextureRotationOf (-0.5235987755982988); // -30 deg == 330 deg
    const TextureRotation wrapped = TextureRotationOf (5.759586531581287);
    EXPECT_NEAR (negative.cosA, wrapped.cosA, 1e-6);
    EXPECT_NEAR (negative.sinA, wrapped.sinA, 1e-6);
    u = 1.0;
    v = 1.0;
    RotateTextureCoordinate (negative, u, v);
    EXPECT_NEAR (u, 0.8660254 + 0.5, 1e-6); // u cos - v sin, sin = -0.5
    EXPECT_NEAR (v, -0.5 + 0.8660254, 1e-6);
}
