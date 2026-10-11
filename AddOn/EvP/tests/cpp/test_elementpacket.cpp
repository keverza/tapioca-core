// ArchViz/ElementPacket — the render-triangle -> source-triangle permutation.
//
// ⚠️ WHY OFFLINE: MakeElementPacket material-groups (reorders) the index buffer,
// so after it runs a rendered triangle is no longer a source triangle. The
// permutation that maps one back to the other is what a picker, a side buffer or
// a takeoff reads. Getting it wrong does not fail -- every rendered triangle is
// attributed to a neighbour's provenance, which looks like a result. It is pure,
// so it is checked here where a wrong permutation is an assertion.

#include "ArchViz/ElementPacket.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <vector>

using geomsrv::Mesh;
using geomsrv::archviz::CapturedMeshPacket;
using geomsrv::archviz::ElementUpload;
using geomsrv::archviz::MakeElementPacket;
using geomsrv::archviz::MaterialRange;

namespace {

// Four independent triangles, each its own three vertices, so a triangle's
// identity is readable from its first index: triangle i starts at 3*i.
Mesh FourTriangles (const std::vector<int32_t>& materials)
{
    Mesh mesh;
    mesh.guid = "mesh-guid";
    mesh.vertices.assign (12 * 3, 0.0);
    for (size_t i = 0; i < 12 * 3; ++i)
        mesh.vertices[i] = double (i);
    mesh.triangles = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    mesh.triMaterial = materials;
    mesh.triWireEdges.assign (materials.size (), 0x7);
    mesh.triSourceBody.assign (materials.size (), 1);
    mesh.triSourcePgon = { 1, 2, 3, 4 };
    return mesh;
}

std::unique_ptr<ElementUpload> BuildPacket (const Mesh& mesh)
{
    CapturedMeshPacket packet;
    packet.mesh = mesh;
    packet.capturedAt = std::chrono::steady_clock::now ();
    packet.captureMilliseconds = 0.0;
    return MakeElementPacket (packet);
}

bool IsPermutationOfZeroToN (const std::vector<uint32_t>& order)
{
    std::vector<uint32_t> sorted = order;
    std::sort (sorted.begin (), sorted.end ());
    for (uint32_t i = 0; i < sorted.size (); ++i)
        if (sorted[i] != i)
            return false;
    return true;
}

} // namespace

// The permutation is a permutation, and it is the one the reordered index buffer
// actually used -- so a shader reading SV_PrimitiveID and a CPU side buffer walk
// the same order.
TEST (ElementPacket, RenderToSourceTriangleFollowsTheReorderedBuffer)
{
    const Mesh mesh = FourTriangles ({ 5, 0, 5, 0 });
    const std::unique_ptr<ElementUpload> up = BuildPacket (mesh);
    ASSERT_NE (up, nullptr);

    ASSERT_EQ (up->renderToSourceTriangle.size (), 4u);
    EXPECT_TRUE (IsPermutationOfZeroToN (up->renderToSourceTriangle));

    // material 0 stabilises triangles 1 and 3 first, then material 5: 0 and 2.
    EXPECT_EQ (up->renderToSourceTriangle, (std::vector<uint32_t> { 1, 3, 0, 2 }));

    // The index buffer is the source triangles in exactly that order...
    for (size_t render = 0; render < up->renderToSourceTriangle.size (); ++render) {
        const uint32_t source = up->renderToSourceTriangle[render];
        EXPECT_EQ (up->indices[render * 3 + 0], mesh.triangles[source * 3 + 0]);
        EXPECT_EQ (up->indices[render * 3 + 1], mesh.triangles[source * 3 + 1]);
        EXPECT_EQ (up->indices[render * 3 + 2], mesh.triangles[source * 3 + 2]);
    }

    // ...so each render triangle's range material equals its source triangle's.
    for (const MaterialRange& range : up->ranges) {
        for (uint32_t i = range.firstIndex; i < range.firstIndex + range.indexCount; i += 3) {
            const uint32_t render = i / 3;
            EXPECT_EQ (mesh.triMaterial[up->renderToSourceTriangle[render]], range.material);
        }
    }
}

// A single material does not reorder, so the permutation is the identity. This
// is the case where a wrong map would still look right -- until a second
// material arrived.
TEST (ElementPacket, SingleMaterialIsIdentity)
{
    const std::unique_ptr<ElementUpload> up = BuildPacket (FourTriangles ({ 4, 4, 4, 4 }));
    ASSERT_NE (up, nullptr);
    ASSERT_EQ (up->renderToSourceTriangle.size (), 4u);
    for (uint32_t i = 0; i < 4; ++i)
        EXPECT_EQ (up->renderToSourceTriangle[i], i);
}

// Provenance is recoverable only while holding the exact producing mesh; no
// renderer retention or GPU picking is implemented by this packet contract.
TEST (ElementPacket, RenderTriangleReachesSourceProvenance)
{
    const Mesh mesh = FourTriangles ({ 5, 0, 5, 0 });
    const std::unique_ptr<ElementUpload> up = BuildPacket (mesh);
    ASSERT_NE (up, nullptr);
    ASSERT_EQ (up->renderToSourceTriangle.size (), up->indices.size () / 3);

    for (uint32_t render = 0; render < up->renderToSourceTriangle.size (); ++render) {
        const uint32_t source = up->renderToSourceTriangle[render];
        ASSERT_LT (source, mesh.triSourcePgon.size ());
        EXPECT_GE (mesh.triSourceBody[source], 1);
        EXPECT_GE (mesh.triSourcePgon[source], 1);
    }
}

// The permutation is part of what the upload queue retains, so it is part of the byte
// accounting -- an under-count here is a throttle that lets the queue grow past
// its budget.
TEST (ElementPacket, PermutationIsCountedInByteAccounting)
{
    const Mesh mesh = FourTriangles ({ 5, 0, 5, 0 });
    const std::unique_ptr<ElementUpload> up = BuildPacket (mesh);
    ASSERT_NE (up, nullptr);

    const size_t expected = up->guid.size () + up->vertices.size () * sizeof (float) +
                            up->normals.size () * sizeof (float) + up->indices.size () * sizeof (uint32_t) +
                            up->ranges.size () * sizeof (MaterialRange) + up->wireEdges.size () * sizeof (uint32_t) +
                            up->renderToSourceTriangle.size () * sizeof (uint32_t);
    EXPECT_EQ (up->PayloadBytes (), expected);
    EXPECT_GE (up->Bytes (), up->renderToSourceTriangle.size () * sizeof (uint32_t));
}

TEST (ElementPacket, EmptyMeshProducesNoPacket)
{
    Mesh empty;
    empty.guid = "empty";
    EXPECT_EQ (BuildPacket (empty), nullptr);
}

TEST (ElementPacket, MaterialFallbackStillRetainsCompletePermutation)
{
    const Mesh mesh = FourTriangles ({ 5 });
    const auto upload = BuildPacket (mesh);
    ASSERT_NE (upload, nullptr);
    EXPECT_EQ (upload->renderToSourceTriangle, (std::vector<uint32_t> { 1, 2, 3, 0 }));
    for (size_t i = 0; i < upload->renderToSourceTriangle.size (); ++i)
        EXPECT_EQ (upload->indices[i * 3], mesh.triangles[upload->renderToSourceTriangle[i] * 3]);
}

TEST (ElementPacket, PermutationDoesNotImplySourceProvenanceAvailability)
{
    Mesh mesh = FourTriangles ({ 5, 0, 5, 0 });
    mesh.triSourceBody.clear ();
    mesh.triSourcePgon.clear ();
    const auto upload = BuildPacket (mesh);
    ASSERT_NE (upload, nullptr);
    EXPECT_EQ (upload->renderToSourceTriangle, (std::vector<uint32_t> { 1, 3, 0, 2 }));
}
