#include "Geometry/GeometryRustSdf.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>

namespace lg = tapioca::geometry;

namespace {

lg::VolumetricOperation Sphere (lg::Id id, lg::Vec3 center, double radius)
{
    lg::VolumetricOperation operation;
    operation.id = id;
    operation.primitive.kind = lg::VolumetricPrimitiveKind::Sphere;
    operation.primitive.centerOrA = center;
    operation.primitive.radius = radius;
    operation.materialId = static_cast<int32_t> (id);
    return operation;
}

lg::VolumetricDetail Detail ()
{
    lg::VolumetricDetail detail;
    detail.id = 100;
    detail.voxelSize = 0.2;
    detail.revision = 7;
    detail.entries.emplace_back (Sphere (1, { 0, 0, 0 }, 1.0));
    return detail;
}

lg::SdfBuildResult Build (const lg::VolumetricDetail& detail,
                          lg::SdfSamplingStrategy strategy = lg::SdfSamplingStrategy::Sparse)
{
    lg::SdfBuildOptions options;
    options.strategy = strategy;
    lg::SdfBuildResult result;
    std::string error;
    EXPECT_TRUE (lg::BuildSdf (detail, 0.2, options, result, error)) << error;
    return result;
}

} // namespace

TEST (GeometryRustSdf, SphereFacadePublishesOwnedMeshAndMatchingHash)
{
    auto detail = Detail ();
    auto result = Build (detail);
    ASSERT_TRUE (result.mesh.valid);
    EXPECT_GT (result.mesh.VertexCount (), 0);
    EXPECT_EQ (result.mesh.positions.size (), result.mesh.normals.size ());
    EXPECT_EQ (result.mesh.triMaterial.size (), result.mesh.TriangleCount ());
    EXPECT_EQ (result.mesh.vertexSource.size (), result.mesh.VertexCount ());
    EXPECT_EQ (result.mesh.triangleSource.size (), result.mesh.TriangleCount ());
    EXPECT_EQ (result.mesh.family, lg::kVolumetricDetailMeshFamily);
    EXPECT_EQ (result.mesh.revision, detail.revision);
    EXPECT_EQ (result.mesh.definitionHash, lg::HashVolumetricDetail (detail));
    EXPECT_EQ (result.contentHash, lg::HashPublishedMesh (result.mesh));
    EXPECT_EQ (result.diagnostics.strategy, lg::SdfSamplingStrategy::Sparse);
    EXPECT_EQ (result.diagnostics.slabCells, 64);
    EXPECT_TRUE (result.mesh.bounds.Valid ());
}

TEST (GeometryRustSdf, DenseAndSparseFacadeGeometryAndHashMatch)
{
    const auto detail = Detail ();
    auto sparse = Build (detail, lg::SdfSamplingStrategy::Sparse);
    auto dense = Build (detail, lg::SdfSamplingStrategy::Dense);
    ASSERT_TRUE (sparse.mesh.valid);
    ASSERT_TRUE (dense.mesh.valid);
    EXPECT_EQ (sparse.mesh, dense.mesh);
    EXPECT_EQ (sparse.contentHash, dense.contentHash);
    EXPECT_EQ (sparse.diagnostics.totalPoints, dense.diagnostics.totalPoints);
    EXPECT_LE (sparse.diagnostics.evaluatedPoints, sparse.diagnostics.totalPoints);
    EXPECT_EQ (dense.diagnostics.evaluatedPoints, dense.diagnostics.totalPoints);
}

TEST (GeometryRustSdf, RigidFrameCopiesOriginAxesAndHalfExtents)
{
    auto detail = Detail ();
    auto& operation = std::get<lg::VolumetricOperation> (detail.entries.front ());
    operation.primitive.kind = lg::VolumetricPrimitiveKind::RoundedBox;
    operation.primitive.frame.origin = { 5.0, -4.0, 3.0 };
    operation.primitive.frame.axes = { lg::Vec3 { 0, 1, 0 }, lg::Vec3 { -1, 0, 0 }, lg::Vec3 { 0, 0, 1 } };
    operation.primitive.half = { 1.0, 2.0, 0.5 };
    operation.primitive.radius = 0.0;
    const auto result = Build (detail);
    ASSERT_TRUE (result.mesh.valid);
    EXPECT_NEAR (result.mesh.bounds.mn[0], 3.0, 1e-12);
    EXPECT_NEAR (result.mesh.bounds.mx[0], 7.0, 1e-12);
    EXPECT_NEAR (result.mesh.bounds.mn[1], -5.0, 1e-12);
    EXPECT_NEAR (result.mesh.bounds.mx[1], -3.0, 1e-12);
    EXPECT_NEAR (result.mesh.bounds.mn[2], 2.5, 1e-12);
    EXPECT_NEAR (result.mesh.bounds.mx[2], 3.5, 1e-12);
}

TEST (GeometryRustSdf, CaveAndExtrusionUseTheSameOwnedFacade)
{
    auto cave = Detail ();
    cave.entries.clear ();
    lg::VolumetricOperation body;
    body.id = 1;
    body.materialId = 11;
    body.primitive.kind = lg::VolumetricPrimitiveKind::RoundedBox;
    body.primitive.half = { 2.0, 1.5, 1.0 };
    body.primitive.radius = 0.1;
    cave.entries.emplace_back (body);
    lg::VolumetricOperation cutter;
    cutter.id = 2;
    cutter.primitive.kind = lg::VolumetricPrimitiveKind::Capsule;
    cutter.primitive.centerOrA = { -3, 0, 0 };
    cutter.primitive.b = { 3, 0, 0 };
    cutter.primitive.radius = 0.6;
    cutter.operation = lg::VolumetricBoolean::Subtract;
    cutter.materialId = 22;
    cave.entries.emplace_back (cutter);
    auto caveResult = Build (cave);
    ASSERT_TRUE (caveResult.mesh.valid);
    std::set<int32_t> materials (caveResult.mesh.triMaterial.begin (), caveResult.mesh.triMaterial.end ());
    EXPECT_EQ (materials, (std::set<int32_t> { 11, 22 }));

    auto extrusion = Detail ();
    auto& operation = std::get<lg::VolumetricOperation> (extrusion.entries.front ());
    operation.primitive.kind = lg::VolumetricPrimitiveKind::Extrude;
    operation.primitive.radius = 0.0;
    operation.primitive.halfHeight = 0.75;
    operation.primitive.profile = { { -1.0, -1.0 }, { 1.0, -1.0 }, { 1.0, 1.0 }, { -1.0, 1.0 } };
    auto extruded = Build (extrusion);
    ASSERT_TRUE (extruded.mesh.valid);
    EXPECT_GT (extruded.mesh.TriangleCount (), 0);
}

TEST (GeometryRustSdf, RefusalCancellationAndBudgetPreserveThePriorResult)
{
    auto valid = Detail ();
    auto sentinel = Build (valid);
    ASSERT_TRUE (sentinel.mesh.valid);

    auto invalid = valid;
    std::get<lg::VolumetricOperation> (invalid.entries.front ()).operation = lg::VolumetricBoolean::Subtract;
    auto result = sentinel;
    std::string error;
    EXPECT_FALSE (lg::BuildSdf (invalid, 0.2, {}, result, error));
    EXPECT_NE (error.find ("FirstOperationNotUnion"), std::string::npos);
    EXPECT_EQ (result, sentinel);

    lg::SdfBuildOptions options;
    std::atomic_bool cancel { true };
    options.cancel = &cancel;
    EXPECT_FALSE (lg::BuildSdf (valid, 0.2, options, result, error));
    EXPECT_NE (error.find ("Cancelled"), std::string::npos);
    EXPECT_EQ (result, sentinel);

    options.cancel = nullptr;
    options.maxTriangles = 1;
    EXPECT_FALSE (lg::BuildSdf (valid, 0.2, options, result, error));
    EXPECT_NE (error.find ("TriangleBudget"), std::string::npos);
    EXPECT_EQ (result, sentinel);
}

TEST (GeometryRustSdf, SoloUsesTheTransientCompilerCutoff)
{
    auto detail = Detail ();
    auto second = Sphere (2, { 0.5, 0, 0 }, 1.0);
    second.operation = lg::VolumetricBoolean::Subtract;
    detail.entries.emplace_back (second);
    lg::SdfBuildOptions options;
    options.soloOperation = 1;
    lg::SdfBuildResult solo;
    std::string error;
    ASSERT_TRUE (lg::BuildSdf (detail, 0.2, options, solo, error)) << error;
    EXPECT_TRUE (solo.mesh.triMaterial.size () > 0);
    EXPECT_TRUE (std::all_of (solo.mesh.triMaterial.begin (), solo.mesh.triMaterial.end (),
                              [] (int32_t material) { return material == 1; }));
}
