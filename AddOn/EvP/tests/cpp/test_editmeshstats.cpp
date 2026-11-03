// #139 M0 stage 2: topology view and Stats report. These tests pin the
// classification vocabulary against analytic solids (a closed box is genus 0
// with chi = 2, an open box has one four-edge rim) and against constructed
// pathologies (non-manifold edge, duplicate/interior/degenerate triangles,
// unreferenced vertices), plus the fails-closed input gate.
#include "Edit/EditMeshStats.hpp"
#include "Edit/EditMeshTopology.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace lg = tapioca::geometry;

namespace {

lg::MeshStats Stats (const geomsrv::Mesh& mesh)
{
    lg::MeshStats stats;
    std::string error;
    EXPECT_TRUE (lg::ComputeMeshStats (mesh.vertices, mesh.triangles, stats, error)) << error;
    return stats;
}

// A closed box with one face pair removed: open rim, still genus 0.
geomsrv::Mesh OpenBox ()
{
    geomsrv::Mesh box = evptest::MakeBox ("open", 0, 0, 0);
    // Drop the two bottom (-Z) triangles; the bottom diagonal vanishes with them.
    box.triangles.erase (box.triangles.begin (), box.triangles.begin () + 6);
    box.triMaterial.erase (box.triMaterial.begin (), box.triMaterial.begin () + 2);
    return box;
}

} // namespace

TEST (EditMeshTopology, ClosedBoxIsManifoldClosedGenusZero)
{
    const auto stats = Stats (evptest::MakeBox ("box", 0, 0, 0));
    EXPECT_TRUE (stats.IsManifold ());
    EXPECT_TRUE (stats.IsClosed ());
    EXPECT_EQ (stats.vertexCount, 8);
    EXPECT_EQ (stats.triangleCount, 12);
    EXPECT_EQ (stats.edgeCount, 18);
    EXPECT_EQ (stats.boundaryEdgeCount, 0);
    EXPECT_EQ (stats.boundaryLoopCount, 0);
    EXPECT_EQ (stats.nonManifoldEdgeCount, 0);
    EXPECT_EQ (stats.eulerCharacteristic, 2);
    EXPECT_EQ (stats.genus, 0);
    EXPECT_EQ (stats.degenerateTriangleCount, 0);
    EXPECT_EQ (stats.duplicateTriangleCount, 0);
    EXPECT_EQ (stats.interiorTriangleCount, 0);
    EXPECT_EQ (stats.unreferencedVertexCount, 0);
}

TEST (EditMeshTopology, OpenBoxHasOneFourEdgeRim)
{
    const geomsrv::Mesh mesh = OpenBox ();
    const auto stats = Stats (mesh);
    EXPECT_TRUE (stats.IsManifold ());
    EXPECT_FALSE (stats.IsClosed ());
    EXPECT_EQ (stats.triangleCount, 10);
    EXPECT_EQ (stats.boundaryEdgeCount, 4);
    EXPECT_EQ (stats.boundaryLoopCount, 1);
    EXPECT_EQ (stats.eulerCharacteristic, 1);
    EXPECT_EQ (stats.genus, 0);

    lg::MeshTopology topo;
    std::string error;
    ASSERT_TRUE (lg::BuildMeshTopology (mesh.vertices, mesh.triangles, topo, error)) << error;
    ASSERT_EQ (topo.boundaryLoops.size (), 1);
    EXPECT_EQ (topo.boundaryLoops[0].size (), 4) << "The rim is the bottom square, no diagonal.";
    EXPECT_EQ (topo.untracedBoundaryHalfEdges, 0);
}

TEST (EditMeshTopology, SingleTriangleIsOneBoundaryLoop)
{
    lg::MeshTopology topo;
    std::string error;
    const geomsrv::Mesh tri = evptest::MakeSingleTriangle ();
    ASSERT_TRUE (lg::BuildMeshTopology (tri.vertices, tri.triangles, topo, error)) << error;
    EXPECT_TRUE (topo.IsManifold ());
    EXPECT_EQ (topo.boundaryEdgeCount, 3);
    ASSERT_EQ (topo.boundaryLoops.size (), 1);
    EXPECT_EQ (topo.boundaryLoops[0].size (), 3);
    EXPECT_EQ (topo.eulerCharacteristic, 1);
    EXPECT_EQ (topo.genus, 0);
}

TEST (EditMeshTopology, ThreeFacesOnOneEdgeIsNonManifoldWithUndefinedGenus)
{
    geomsrv::Mesh mesh;
    mesh.guid = "nonmanifold";
    mesh.vertices = { 0,0,0,  1,0,0,  0,1,0,  1,1,0,  0.5,-1,0 };
    mesh.triangles = { 0, 1, 2,  1, 0, 3,  0, 1, 4 }; // edge (0,1) carries all three
    mesh.triMaterial.assign (3, 0);
    evptest::RecomputeBounds (mesh);

    const auto stats = Stats (mesh);
    EXPECT_EQ (stats.nonManifoldEdgeCount, 1);
    EXPECT_FALSE (stats.IsManifold ());
    EXPECT_EQ (stats.genus, -1) << "No genus is invented for a non-manifold complex.";
}

TEST (EditMeshStats, DegenerateTrianglesAreCountedAndExcludedFromTopology)
{
    geomsrv::Mesh mesh = evptest::MakeSingleTriangle ();
    mesh.guid = "degenerate";
    // Repeated-index triangle and a collinear (roundoff-zero) triangle on the
    // x axis: (0,0,0), (2,0,0), (4,0,0).
    mesh.vertices.insert (mesh.vertices.end (), { 5,5,5,  5,5,5,  2,0,0,  4,0,0 });
    const uint32_t base = 3;
    mesh.triangles.insert (mesh.triangles.end (),
                           { base, base, base + 1, 0, base + 2, base + 3 });
    mesh.triMaterial.insert (mesh.triMaterial.end (), { 0, 0 });
    evptest::RecomputeBounds (mesh);

    const auto stats = Stats (mesh);
    EXPECT_EQ (stats.degenerateTriangleCount, 2);
    EXPECT_EQ (stats.triangleCount, 1) << "Degenerate triangles carry no edge topology.";
    EXPECT_EQ (stats.edgeCount, 3);
}

TEST (EditMeshStats, DuplicateAndInteriorCoincidenceCounts)
{
    // A single triangle, then a same-orientation copy (duplicate), then a
    // reversed copy (interior pair member).
    geomsrv::Mesh mesh = evptest::MakeSingleTriangle ("coincident");
    mesh.triangles.insert (mesh.triangles.end (), { 0, 1, 2,  0, 2, 1 });
    mesh.triMaterial.insert (mesh.triMaterial.end (), { 0, 0 });
    const auto stats = Stats (mesh);
    EXPECT_EQ (stats.duplicateTriangleCount, 1);
    EXPECT_EQ (stats.interiorTriangleCount, 3)
        << "The reversed twin makes its whole class interior, copies included.";

    // Same-orientation copies never become interior.
    geomsrv::Mesh dupOnly = evptest::MakeSingleTriangle ("duponly");
    dupOnly.triangles.insert (dupOnly.triangles.end (), { 1, 2, 0,  2, 0, 1 }); // same cyclic class
    dupOnly.triMaterial.insert (dupOnly.triMaterial.end (), { 0, 0 });
    const auto dupStats = Stats (dupOnly);
    EXPECT_EQ (dupStats.duplicateTriangleCount, 2);
    EXPECT_EQ (dupStats.interiorTriangleCount, 0);
}

TEST (EditMeshStats, UnreferencedVerticesAreCounted)
{
    geomsrv::Mesh mesh = evptest::MakeSingleTriangle ("loose");
    mesh.vertices.insert (mesh.vertices.end (), { 9, 9, 9,  8, 8, 8 });
    evptest::RecomputeBounds (mesh);
    EXPECT_EQ (Stats (mesh).unreferencedVertexCount, 2);
}

TEST (EditMeshStats, DeterministicAcrossRuns)
{
    const auto first = Stats (evptest::MakeDegenerateSnapshot ().meshes[2]);
    const auto second = Stats (evptest::MakeDegenerateSnapshot ().meshes[2]);
    EXPECT_EQ (first.vertexCount, second.vertexCount);
    EXPECT_EQ (first.degenerateTriangleCount, second.degenerateTriangleCount);
    EXPECT_EQ (first.duplicateTriangleCount, second.duplicateTriangleCount);
    EXPECT_EQ (first.interiorTriangleCount, second.interiorTriangleCount);
    EXPECT_EQ (first.genus, second.genus);
}

TEST (EditMeshStats, FailsClosedAndLeavesReportUntouched)
{
    geomsrv::Mesh bad = evptest::MakeOutOfRangeIndices ();
    lg::MeshStats stats;
    stats.degenerateTriangleCount = 999; // sentinel
    std::string error;
    EXPECT_FALSE (lg::ComputeMeshStats (bad.vertices, bad.triangles, stats, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (stats.degenerateTriangleCount, 999) << "A refused input must not publish partial stats.";

    std::vector<double> positions = { 0, 0, 0, 1, 0, 0 };
    std::vector<uint32_t> broken = { 0, 1 };
    EXPECT_FALSE (lg::ComputeMeshStats (positions, broken, stats, error));
}
