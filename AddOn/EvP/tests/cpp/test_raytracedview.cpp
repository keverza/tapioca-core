// ArchViz/RayTracedView -- the ray-traced style's pure half (issue #231): the scene flattened for
// one acceleration structure, the inverse its rays are generated from, and when the accumulation
// restarts, shows, and stops.

#include "ArchViz/RayTracedView.hpp"

#include "ArchViz/MatrixMath.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

std::shared_ptr<SectionMesh> Quad (float x)
{
    auto mesh = std::make_shared<SectionMesh> ();
    mesh->positions = { x, 0, 0, x + 1, 0, 0, x + 1, 1, 0, x, 1, 0 };
    mesh->indices = { 0, 1, 2, 0, 2, 3 };
    return mesh;
}

} // namespace

TEST (RayTracedView, FlattensElementsWithRebasedIndicesAndMaterialSlots)
{
    std::vector<RayTracedSource> sources;
    sources.push_back ({ Quad (0.0f), { { 7, 0, 3 }, { 9, 3, 3 } } });
    sources.push_back ({ Quad (5.0f), { { 9, 0, 6 } } });
    const RayTracedGeometry g = BuildRayTracedGeometry (sources);
    ASSERT_EQ (g.TriangleCount (), 4u);
    EXPECT_EQ (g.positions.size (), 24u);
    // The second element's indices are rebased past the first's four vertices.
    EXPECT_EQ (g.indices[6], 4u);
    EXPECT_EQ (g.indices[11], 7u);
    // Distinct materials, first seen first; the default (-1) is registered for each element.
    ASSERT_EQ (g.triangleMaterial.size (), 4u);
    EXPECT_EQ (g.materials[g.triangleMaterial[0]], 7);
    EXPECT_EQ (g.materials[g.triangleMaterial[1]], 9);
    EXPECT_EQ (g.materials[g.triangleMaterial[2]], 9);
    EXPECT_EQ (g.triangleMaterial[1], g.triangleMaterial[3]);
}

TEST (RayTracedView, UncoveredTrianglesGetTheDefaultAndBadIndicesAreDropped)
{
    auto mesh = Quad (0.0f);
    mesh->indices.insert (mesh->indices.end (), { 0, 1, 99 }); // out of range
    std::vector<RayTracedSource> sources { { mesh, { { 4, 0, 3 } } } };
    const RayTracedGeometry g = BuildRayTracedGeometry (sources);
    ASSERT_EQ (g.TriangleCount (), 2u);
    EXPECT_EQ (g.materials[g.triangleMaterial[0]], 4);
    EXPECT_EQ (g.materials[g.triangleMaterial[1]], -1);
}

TEST (RayTracedView, InverseUndoesAProjection)
{
    const float m[16] = { 1.2f, 0.1f, 0, 0, 0, 2.0f, 0.3f, 0, 0.2f, 0, 1.0f, 1.0f, 3, -4, 0.5f, 2 };
    float inv[16], product[16];
    ASSERT_TRUE (InvertMatrix4 (m, inv));
    Multiply (product, m, inv);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            EXPECT_NEAR (product[r * 4 + c], r == c ? 1.0f : 0.0f, 1e-5f) << r << "," << c;
    const float singular[16] = {};
    EXPECT_FALSE (InvertMatrix4 (singular, inv));
}

TEST (RayTracedView, MovingShowsRasterThenStillBurstsThenConverges)
{
    RayTracedAccumulator acc;
    RayTracedStep s = acc.Advance (1, 6);
    EXPECT_FALSE (s.show); // the first frame of a view is "moving"
    s = acc.Advance (1, 6);
    EXPECT_TRUE (s.show);
    EXPECT_EQ (s.firstSample, 0u);
    EXPECT_EQ (s.trace, RayTracedAccumulator::kFirstBurst);
    s = acc.Advance (1, 6);
    EXPECT_EQ (s.firstSample, 4u);
    EXPECT_EQ (s.trace, 1u);
    acc.Advance (1, 6);
    s = acc.Advance (1, 6);
    EXPECT_TRUE (s.show);
    EXPECT_EQ (s.trace, 0u); // converged at 6: re-shown, not traced
    EXPECT_EQ (acc.Samples (), 6u);
    s = acc.Advance (2, 6); // the camera moved
    EXPECT_FALSE (s.show);
    EXPECT_EQ (acc.Samples (), 0u);
}

TEST (RayTracedView, BurstNeverExceedsTheCap)
{
    RayTracedAccumulator acc;
    acc.Advance (5, 2);
    const RayTracedStep s = acc.Advance (5, 2);
    EXPECT_EQ (s.trace, 2u);
    EXPECT_EQ (acc.Advance (5, 2).trace, 0u);
}

// #231: the traced styles' textures read Archicad's coordinates per vertex. An element without
// them contributes zeros, so the flattened array stays parallel to the positions.
TEST (RayTracedView, FlattensTextureCoordinatesParallelToPositions)
{
    auto textured = Quad (0.0f);
    textured->uvs = { 0, 0, 2, 0, 2, 3, 0, 3 };
    std::vector<RayTracedSource> sources;
    sources.push_back ({ Quad (5.0f), { { 1, 0, 6 } } }); // no coordinates
    sources.push_back ({ textured, { { 2, 0, 6 } } });
    const RayTracedGeometry g = BuildRayTracedGeometry (sources);
    ASSERT_EQ (g.uvs.size (), g.positions.size () / 3 * 2);
    for (size_t k = 0; k < 8; ++k)
        EXPECT_EQ (g.uvs[k], 0.0f);
    EXPECT_EQ (g.uvs[8 + 4], 2.0f); // the textured quad's third vertex, after the first four
    EXPECT_EQ (g.uvs[8 + 5], 3.0f);
}

// #231: one slot per distinct image, shared by the materials that name it; untextured materials
// get none, and a full array is counted, never wrapped onto another image.
TEST (RayTracedView, TextureSlotsAreSharedPerImageAndOverflowIsCounted)
{
    const RayTracedTextureSlots s = AssignTextureSlots ({ 0, 11, 22, 11, 33, 0, 44 }, 3);
    ASSERT_EQ (s.slotOf.size (), 7u);
    EXPECT_EQ (s.slotOf[0], -1);
    EXPECT_EQ (s.slotOf[1], 0);
    EXPECT_EQ (s.slotOf[2], 1);
    EXPECT_EQ (s.slotOf[3], 0);
    EXPECT_EQ (s.slotOf[4], 2);
    EXPECT_EQ (s.slotOf[5], -1);
    EXPECT_EQ (s.slotOf[6], -1); // the fourth image: no room
    EXPECT_EQ (s.overflow, 1u);
    EXPECT_EQ (s.keys, (std::vector<uint64_t> { 11, 22, 33 }));
}

// #231: the traced styles shade with Archicad's per-corner normals, flattened parallel to the
// positions; an element without them contributes zeros, which the shader reads as "shade flat".
TEST (RayTracedView, FlattensNormalsParallelToPositions)
{
    auto smooth = Quad (0.0f);
    smooth->normals = { 0, 0, 1, 0, 0.6f, 0.8f, 0, 0, 1, 0, -0.6f, 0.8f };
    std::vector<RayTracedSource> sources;
    sources.push_back ({ smooth, { { 1, 0, 6 } } });
    sources.push_back ({ Quad (5.0f), { { 2, 0, 6 } } }); // no normals
    const RayTracedGeometry g = BuildRayTracedGeometry (sources);
    ASSERT_EQ (g.normals.size (), g.positions.size ());
    EXPECT_FLOAT_EQ (g.normals[4], 0.6f); // the first quad's second vertex
    EXPECT_FLOAT_EQ (g.normals[10], -0.6f);
    for (size_t k = 12; k < 24; ++k)
        EXPECT_EQ (g.normals[k], 0.0f);
}
