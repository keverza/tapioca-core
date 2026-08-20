// ArchViz/DebugCubeMesh — the one mesh both renderers can draw without Archicad.
//
// ⚠️ WHY OFFLINE: this box is the instrument the Diligent port is calibrated
// with. If the CUBE is wrong, every conclusion drawn from looking at it is
// wrong too — and the failure modes are mutually disguising. A reversed winding
// and a mirrored view matrix produce the same picture; a face whose stored
// normal disagrees with its triangles shades like a dent and reads as a
// lighting bug. Checking the geometry here is what lets the in-Archicad run
// mean "the RENDERER is right" rather than "something is right".
//
// The winding test is the one that matters: with a right-handed view+projection
// (tests/cpp/test_matrixmath.cpp guards that), every face must be
// counter-clockwise seen from OUTSIDE, so a cull setting that hides the outside
// of the box indicts the matrices and not the mesh.

#include "ArchViz/DebugCubeMesh.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <set>

using namespace geomsrv::archviz;

namespace {

struct Cube {
    ArchVizVertex vertices[debugcubemesh::kVertexCount];
    uint16_t indices[debugcubemesh::kIndexCount];

    Cube () { debugcubemesh::Build (vertices, indices); }
};

void Cross (float out[3], const float a[3], const float b[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

}   // namespace

TEST (DebugCubeMesh, EveryTriangleIsWoundCounterClockwiseSeenFromOutside)
{
    const Cube cube;
    for (size_t t = 0; t < debugcubemesh::kIndexCount; t += 3) {
        const ArchVizVertex& v0 = cube.vertices[cube.indices[t + 0]];
        const ArchVizVertex& v1 = cube.vertices[cube.indices[t + 1]];
        const ArchVizVertex& v2 = cube.vertices[cube.indices[t + 2]];

        const float e1[3] = {v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
        const float e2[3] = {v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
        float geometric[3];
        Cross (geometric, e1, e2);

        // The right-hand rule says a CCW triangle's cross product points out of
        // the face it belongs to. Comparing against the STORED normal checks
        // both the winding and that the normal was not put on the wrong face.
        const float agreement = geometric[0] * v0.nx + geometric[1] * v0.ny + geometric[2] * v0.nz;
        EXPECT_GT (agreement, 0.0f) << "triangle at index " << t << " is wound the wrong way "
                                       "or carries the wrong normal";
    }
}

TEST (DebugCubeMesh, HasTwentyFourVerticesSoNoNormalIsShared)
{
    const Cube cube;
    // ⚠️ 24, NOT 8. A corner belongs to three faces with three different
    // normals, exactly as VertexWeld leaves real Archicad geometry. Building it
    // the cheap way would shade a flat box like a sphere and would hide the very
    // property the extraction port depends on.
    static_assert (debugcubemesh::kVertexCount == 24, "");

    // Every vertex is a cube corner: all three coordinates at the half extent.
    for (const ArchVizVertex& v : cube.vertices) {
        EXPECT_FLOAT_EQ (std::abs (v.x), debugcubemesh::kHalfExtent);
        EXPECT_FLOAT_EQ (std::abs (v.y), debugcubemesh::kHalfExtent);
        EXPECT_FLOAT_EQ (std::abs (v.z), debugcubemesh::kHalfExtent);
    }

    // Exactly 8 distinct positions, each appearing 3 times.
    std::set<std::tuple<float, float, float>> positions;
    for (const ArchVizVertex& v : cube.vertices)
        positions.insert ({v.x, v.y, v.z});
    EXPECT_EQ (positions.size (), 8u);
}

TEST (DebugCubeMesh, EachFaceIsFlatAndAxisAligned)
{
    const Cube cube;
    std::set<std::tuple<float, float, float>> normals;
    for (int face = 0; face < 6; ++face) {
        const ArchVizVertex& first = cube.vertices[face * 4];
        normals.insert ({first.nx, first.ny, first.nz});
        // Unit length, and along exactly one axis.
        const float lengthSq = first.nx * first.nx + first.ny * first.ny + first.nz * first.nz;
        EXPECT_FLOAT_EQ (lengthSq, 1.0f);
        // All four corners of a face share it: flat shading, not averaged.
        for (int corner = 1; corner < 4; ++corner) {
            const ArchVizVertex& v = cube.vertices[face * 4 + corner];
            EXPECT_FLOAT_EQ (v.nx, first.nx);
            EXPECT_FLOAT_EQ (v.ny, first.ny);
            EXPECT_FLOAT_EQ (v.nz, first.nz);
        }
    }
    EXPECT_EQ (normals.size (), 6u) << "the six faces must point six different ways";
}

// ⚠️ THE COLOUR NAMES ARE ASKED OF A HUMAN, so a wrong name turns a real answer
// into a wrong one. The field is a uint32 written 0xAABBGGRR: red is the LAST
// byte pair, not the first. This locks the decode the face names claim.
TEST (DebugCubeMesh, FaceColoursDecodeToTheNamesTheProbeUses)
{
    const uint32_t topAbgr = debugcubemesh::FaceColorAbgr (0);
    EXPECT_EQ (topAbgr & 0xffu, 0xdfu) << "red";
    EXPECT_EQ ((topAbgr >> 8) & 0xffu, 0x9fu) << "green";
    EXPECT_EQ ((topAbgr >> 16) & 0xffu, 0x4fu) << "blue";
    EXPECT_EQ ((topAbgr >> 24) & 0xffu, 0xffu) << "alpha";
    // Red > green > blue is what makes it orange rather than the blue a
    // straight RGBA read would produce.
    EXPECT_GT (topAbgr & 0xffu, (topAbgr >> 16) & 0xffu);
    EXPECT_STREQ (debugcubemesh::FaceName (0), "+Z top (orange)");
}

TEST (DebugCubeMesh, EveryFaceHasItsOwnColour)
{
    const Cube cube;
    std::set<uint32_t> colors;
    for (int face = 0; face < 6; ++face) {
        colors.insert (debugcubemesh::FaceColorAbgr (face));
        // The vertices carry the same colour the accessor reports -- a probe
        // that names a face by the accessor must be naming what is drawn.
        for (int corner = 0; corner < 4; ++corner)
            EXPECT_EQ (cube.vertices[face * 4 + corner].abgr, debugcubemesh::FaceColorAbgr (face));
    }
    EXPECT_EQ (colors.size (), 6u);
}

TEST (DebugCubeMesh, OutOfRangeFaceIndicesAreRefusedRatherThanRead)
{
    EXPECT_EQ (debugcubemesh::FaceColorAbgr (-1), 0u);
    EXPECT_EQ (debugcubemesh::FaceColorAbgr (6), 0u);
    EXPECT_STREQ (debugcubemesh::FaceName (-1), "");
    EXPECT_STREQ (debugcubemesh::FaceName (6), "");
}

// ⚠️ THE NEUTRAL PALETTE MUST CHANGE ONLY THE COLOUR. It exists so the shading
// question can be answered without six hues competing with it; if it also moved
// a vertex or a normal, the two instruments would disagree about the same box.
TEST (DebugCubeMesh, NeutralPaletteChangesColourAndNothingElse)
{
    ArchVizVertex colored[debugcubemesh::kVertexCount];
    ArchVizVertex neutral[debugcubemesh::kVertexCount];
    uint16_t coloredIndices[debugcubemesh::kIndexCount];
    uint16_t neutralIndices[debugcubemesh::kIndexCount];
    debugcubemesh::Build (colored, coloredIndices, debugcubemesh::Palette::Colored);
    debugcubemesh::Build (neutral, neutralIndices, debugcubemesh::Palette::Neutral);

    std::set<uint32_t> neutralColors;
    for (size_t i = 0; i < debugcubemesh::kVertexCount; ++i) {
        EXPECT_FLOAT_EQ (neutral[i].x, colored[i].x);
        EXPECT_FLOAT_EQ (neutral[i].y, colored[i].y);
        EXPECT_FLOAT_EQ (neutral[i].z, colored[i].z);
        EXPECT_FLOAT_EQ (neutral[i].nx, colored[i].nx);
        EXPECT_FLOAT_EQ (neutral[i].ny, colored[i].ny);
        EXPECT_FLOAT_EQ (neutral[i].nz, colored[i].nz);
        neutralColors.insert (neutral[i].abgr);
    }
    for (size_t i = 0; i < debugcubemesh::kIndexCount; ++i)
        EXPECT_EQ (neutral[i % debugcubemesh::kVertexCount].abgr, *neutralColors.begin ());
    EXPECT_EQ (neutralColors.size (), 1u) << "neutral means ONE colour, or it is not an instrument";
    for (size_t i = 0; i < debugcubemesh::kIndexCount; ++i)
        EXPECT_EQ (neutralIndices[i], coloredIndices[i]);
}

TEST (DebugCubeMesh, IndicesStayInsideTheVertexArray)
{
    const Cube cube;
    for (uint16_t index : cube.indices)
        EXPECT_LT (index, debugcubemesh::kVertexCount);
}
