// #139 M0 stage 3: the Clean pipeline. These tests pin the deterministic
// step order (weld -> degenerate -> interior -> duplicate -> loose), attribute
// preservation (materials follow surviving triangles, normals follow
// surviving representatives), stable remaps back to input indices,
// determinism, idempotence and the fails-closed validity gate.
#include "Edit/EditMeshClean.hpp"
#include "Edit/EditPublishedMesh.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace lg = tapioca::geometry;

namespace {

lg::PublishedMesh Publish (const geomsrv::Mesh& mesh)
{
    lg::PublishedMesh published;
    std::string error;
    EXPECT_TRUE (lg::FromGeomsrvMesh (mesh, published, error)) << error;
    return published;
}

lg::PublishedMesh Clean (const lg::PublishedMesh& input, lg::MeshCleanReport& report)
{
    lg::PublishedMesh output;
    std::string error;
    EXPECT_TRUE (lg::CleanMesh (input, {}, output, report, error)) << error;
    return output;
}

// Box plus every pathology in one mesh: a near-duplicate of vertex 0, a
// duplicated triangle (different material), an interior pair off to the side,
// a collinear triangle with three exclusive vertices, and one loose vertex.
lg::PublishedMesh Messy ()
{
    geomsrv::Mesh mesh = evptest::MakeBox ("messy", 0, 0, 0);
    for (size_t t = 0; t < mesh.triMaterial.size (); ++t)
        mesh.triMaterial[t] = static_cast<int32_t> (t % 3);
    const uint32_t base = static_cast<uint32_t> (mesh.VertexCount ());

    // Near-duplicate of vertex 0 (within the 1e-4 weld tolerance).
    mesh.vertices.insert (mesh.vertices.end (), { mesh.vertices[0] + 3e-5, mesh.vertices[1], mesh.vertices[2] });

    // Exact duplicate of triangle 0, but with a different material.
    mesh.triangles.insert (mesh.triangles.end (), { mesh.triangles[0], mesh.triangles[1], mesh.triangles[2] });
    mesh.triMaterial.push_back (99);

    // Interior pair: same triangle, opposite orientation, exclusive vertices.
    const uint32_t side = base + 1;
    mesh.vertices.insert (mesh.vertices.end (), { 5, 0, 0, 6, 0, 0, 5, 1, 0, 9, 9, 9, 2, 0, 0, 4, 0, 0 });
    mesh.triangles.insert (mesh.triangles.end (), { side, side + 1, side + 2, side, side + 2, side + 1, // interior pair
                                                    0, side + 4, side + 5 }); // collinear (degenerate)
    mesh.triMaterial.insert (mesh.triMaterial.end (), { 0, 0, 0 });

    evptest::RecomputeBounds (mesh);
    return Publish (mesh);
}

} // namespace

TEST (EditMeshClean, CleanBoxIsNoOp)
{
    const auto input = Publish (evptest::MakeBox ("box", 0, 0, 0));
    lg::MeshCleanReport report;
    const auto output = Clean (input, report);

    EXPECT_TRUE (report.IsNoOp ());
    EXPECT_EQ (output.positions, input.positions);
    EXPECT_EQ (output.triangles, input.triangles);
    EXPECT_EQ (output.triMaterial, input.triMaterial);
    EXPECT_EQ (output.vertexSource, input.vertexSource);
    EXPECT_EQ (output.triangleSource, input.triangleSource);
    EXPECT_TRUE (output.valid);
    EXPECT_EQ (output.definitionHash, lg::HashPublishedMesh (input));
    EXPECT_EQ (output.bounds.mn[0], input.bounds.mn[0]);
}

TEST (EditMeshClean, WeldMergesWithinToleranceAndCountsNormalConflicts)
{
    // A quad split into two triangles; the second uses a near-duplicate of the
    // quad's last corner instead of the corner itself.
    geomsrv::Mesh mesh;
    mesh.guid = "weld";
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0 + 5e-5, 1, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 4 };
    mesh.triMaterial = { 0, 0 };
    evptest::RecomputeBounds (mesh);

    lg::PublishedMesh withConflict;
    std::string error;
    ASSERT_TRUE (lg::FromGeomsrvMesh (mesh, withConflict, error)) << error;
    withConflict.normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0 }; // corner 4 disagrees
    lg::MeshCleanReport report;
    const auto output = Clean (withConflict, report);

    EXPECT_EQ (report.weldedVertices, 1);
    EXPECT_EQ (report.normalConflicts, 1) << "Position wins; the shading clash is counted, not hidden.";
    EXPECT_EQ (output.VertexCount (), 4);
    EXPECT_EQ (output.TriangleCount (), 2);
    EXPECT_EQ (output.triangles[3], 0u);
    EXPECT_EQ (output.triangles[4], 2u);
    EXPECT_EQ (output.triangles[5], 3u) << "Triangle 2 now closes onto the representative corner.";

    // Same mesh without the normal clash: welded, no conflict.
    auto calm = withConflict;
    calm.normals[12] = 0.0f;
    calm.normals[13] = 0.0f;
    calm.normals[14] = 1.0f;
    lg::MeshCleanReport calmReport;
    Clean (calm, calmReport);
    EXPECT_EQ (calmReport.weldedVertices, 1);
    EXPECT_EQ (calmReport.normalConflicts, 0);
}

TEST (EditMeshClean, ZeroToleranceDisablesWelding)
{
    const auto input = Messy ();
    lg::MeshCleanReport report;
    lg::PublishedMesh output;
    std::string error;
    lg::MeshCleanOptions options;
    options.weldTolerance = 0.0;
    ASSERT_TRUE (lg::CleanMesh (input, options, output, report, error)) << error;
    EXPECT_EQ (report.weldedVertices, 0);
    EXPECT_EQ (output.VertexCount (), 8) << "Interior-pair, degenerate and loose vertices all drop.";
    EXPECT_EQ (output.TriangleCount (), 12);
}

TEST (EditMeshClean, InteriorPairDissolvesCompletely)
{
    geomsrv::Mesh mesh = evptest::MakeSingleTriangle ("wall");
    mesh.triangles.insert (mesh.triangles.end (), { 0, 2, 1 });
    mesh.triMaterial.push_back (0);
    lg::MeshCleanReport report;
    const auto output = Clean (Publish (mesh), report);
    EXPECT_EQ (report.removedInterior, 2);
    EXPECT_EQ (output.TriangleCount (), 0);
    EXPECT_EQ (output.VertexCount (), 0);
}

TEST (EditMeshClean, DuplicateKeepsLowestIndexMemberAndItsAttributes)
{
    geomsrv::Mesh mesh = evptest::MakeSingleTriangle ("dup");
    mesh.triangles.insert (mesh.triangles.end (), { 1, 2, 0 }); // same cyclic class
    mesh.triMaterial = { 5, 9 };
    lg::MeshCleanReport report;
    const auto output = Clean (Publish (mesh), report);

    EXPECT_EQ (report.removedDuplicate, 1);
    ASSERT_EQ (output.TriangleCount (), 1);
    EXPECT_EQ (output.triMaterial[0], 5) << "The first member survives, with its material.";
    EXPECT_EQ (output.triangleSource[0], 0u);
    ASSERT_EQ (output.VertexCount (), 3);
    EXPECT_EQ (output.vertexSource, (std::vector<uint32_t> { 0, 1, 2 }));
}

TEST (EditMeshClean, DegenerateTrianglesAndLooseVerticesDrop)
{
    const auto input = Messy ();
    lg::MeshCleanReport report;
    const auto output = Clean (input, report);

    EXPECT_EQ (report.weldedVertices, 1);
    EXPECT_EQ (report.removedDuplicate, 1);
    EXPECT_EQ (report.removedInterior, 2);
    EXPECT_EQ (report.removedDegenerate, 1);
    EXPECT_EQ (report.removedUnreferenced, 6)
        << "Loose vertex + two collinear exclusives + three interior-pair exclusives.";
    EXPECT_EQ (output.VertexCount (), 8);
    EXPECT_EQ (output.TriangleCount (), 12);

    // The surviving box is byte-identical to a clean box: weld, duplicate and
    // degenerate surgery never touched its geometry, and its per-triangle
    // materials came through unchanged (the messy t%3 pattern, in order).
    const auto box = Publish (evptest::MakeBox ("box", 0, 0, 0));
    lg::MeshCleanReport boxReport;
    const auto cleanBox = Clean (box, boxReport);
    EXPECT_EQ (output.positions, cleanBox.positions);
    EXPECT_EQ (output.triangles, cleanBox.triangles);
    EXPECT_EQ (output.triMaterial, (std::vector<int32_t> { 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2 }));
}

TEST (EditMeshClean, RemapTracksInputIdentity)
{
    // Weld case: the surviving corner 3 is representative; vertex 4 is gone.
    geomsrv::Mesh mesh;
    mesh.guid = "remap";
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 5e-5, 1, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 4 };
    mesh.triMaterial = { 7, 7 };
    lg::MeshCleanReport report;
    const auto output = Clean (Publish (mesh), report);

    ASSERT_EQ (output.VertexCount (), 4);
    EXPECT_EQ (output.vertexSource, (std::vector<uint32_t> { 0, 1, 2, 3 }));
    ASSERT_EQ (output.TriangleCount (), 2);
    EXPECT_EQ (output.triangleSource, (std::vector<uint32_t> { 0, 1 }));
    EXPECT_EQ (output.triMaterial[0], 7);
    EXPECT_EQ (output.bounds.mn[0], 0.0);
}

TEST (EditMeshClean, DeterministicAcrossRuns)
{
    const auto input = Messy ();
    lg::MeshCleanReport firstReport, secondReport;
    const auto first = Clean (input, firstReport);
    const auto second = Clean (input, secondReport);
    EXPECT_EQ (first, second);
    EXPECT_EQ (firstReport.weldedVertices, secondReport.weldedVertices);
    EXPECT_EQ (firstReport.removedInterior, secondReport.removedInterior);
    EXPECT_EQ (firstReport.removedUnreferenced, secondReport.removedUnreferenced);
}

TEST (EditMeshClean, SecondRunIsNoOp)
{
    const auto input = Messy ();
    lg::MeshCleanReport firstReport;
    const auto once = Clean (input, firstReport);
    lg::MeshCleanReport secondReport;
    const auto twice = Clean (once, secondReport);

    EXPECT_TRUE (secondReport.IsNoOp ()) << "Clean is idempotent: nothing left to repair.";
    EXPECT_EQ (twice.positions, once.positions);
    EXPECT_EQ (twice.triangles, once.triangles);
    EXPECT_EQ (twice.triMaterial, once.triMaterial);
    EXPECT_EQ (twice.normals, once.normals);
}

TEST (EditMeshClean, FailsClosedOnInvalidInput)
{
    auto input = Publish (evptest::MakeBox ("box", 0, 0, 0));
    input.valid = false;
    lg::PublishedMesh output;
    output.positions = { 1, 2, 3 }; // sentinel
    lg::MeshCleanReport report;
    report.weldedVertices = 77; // sentinel
    std::string error;
    EXPECT_FALSE (lg::CleanMesh (input, {}, output, report, error));
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (output.positions, (std::vector<double> { 1, 2, 3 }));
    EXPECT_EQ (report.weldedVertices, 77u);
}

TEST (EditMeshClean, NonFinitePositionsRefuse)
{
    auto input = Publish (evptest::MakeSingleTriangle ("tri"));
    input.positions[0] = std::numeric_limits<double>::infinity ();
    lg::PublishedMesh output;
    lg::MeshCleanReport report;
    std::string error;
    EXPECT_FALSE (lg::CleanMesh (input, {}, output, report, error));
}
