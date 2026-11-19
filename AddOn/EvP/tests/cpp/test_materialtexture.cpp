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
