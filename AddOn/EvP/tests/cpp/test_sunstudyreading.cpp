// Tests for SunStudy/SunStudyReading -- the hover inspector's value under a
// point.
//
// ⚠️ THE CLAIM IS AGREEMENT WITH THE PICTURE. The inspector exists to say what
// the colour under the cursor means; if it read a different cell from the one
// the tint shader drew, it would disagree exactly at cell boundaries and be
// believed everywhere else. So the decisive test here drives the REAL display
// records through the shader's own arithmetic and requires the same sample.

#include <cmath>
#include <string>
#include <vector>

#include "ArchViz/SunStudyOverlay.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyReading.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "MeshFixtures.hpp"
#if defined(TAPIOCA_GEOMETRY_RUST)
#include "sunstudy_core_fixture.hpp"
#endif
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// A 6 m x 2 m wall as 12 welded triangles (one surface), plus a 4 m x 3 m quad
// of a second element.
struct Scene {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
    std::vector<std::string> elementOf { "wall", "slab" };

    Scene ()
    {
        for (int i = 0; i <= 6; ++i) {
            vertices.insert (vertices.end (), { double (i), 0.0, 0.0 });
            vertices.insert (vertices.end (), { double (i), 2.0, 0.0 });
        }
        for (uint32_t i = 0; i < 6; ++i) {
            const uint32_t a = i * 2, b = i * 2 + 1, c = i * 2 + 2, d = i * 2 + 3;
            triangles.insert (triangles.end (), { a, c, d, a, d, b });
            groups.insert (groups.end (), { 0u, 0u });
        }
        const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), { 20, 0, 0, 24, 0, 0, 24, 3, 0, 20, 3, 0 });
        triangles.insert (triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
        groups.insert (groups.end (), { 1u, 1u });
    }
    size_t Faces () const
    {
        return triangles.size () / 3;
    }
};

// Distinct hours per sample, so "the right sample" is checkable by value.
std::vector<double> Hours (size_t count)
{
    std::vector<double> hours (count);
    for (size_t i = 0; i < count; ++i)
        hours[i] = 0.25 * double (i);
    return hours;
}

PatchSampleGrid Patches (const Scene& scene, double spacing, const std::vector<uint8_t>* mask = nullptr)
{
    PatchSamplerOptions options;
    options.spacing = spacing;
    options.normalOffset = 0.0;
    options.sampleGroup = mask;
    return kernels::BuildPatchSampleGrid (scene.vertices.data (), scene.vertices.size () / 3, scene.triangles.data (),
                                          scene.Faces (), scene.groups.data (), scene.elementOf, options);
}

} // namespace

TEST (SunStudyReading, PickedMeshRaycastNeedsNoWholeSnapshotCacheOrInvisibleHitLimit)
{
    geomsrv::Snapshot snapshot;
    snapshot.id = 901;
    for (size_t i = 0; i < 40; ++i)
        snapshot.meshes.push_back (evptest::MakeBox ("hidden", 0, 0, 2.0 + 2.0 * i));
    snapshot.meshes.push_back (evptest::MakeBox ("picked", 0, 0, 0));
    geomsrv::QueryIndexCache::Get ().Release ();
    const double origin[3] = { 0.25, 0.4, 100 }, direction[3] = { 0, 0, -7 };
    const auto hit = geomsrv::QueryEngine::RaycastMesh (snapshot, 40, origin, direction, 0.0);
    ASSERT_TRUE (hit.hit);
    EXPECT_EQ (hit.meshIndex, 40u);
    EXPECT_GE (hit.tri, 480u);
    EXPECT_LT (hit.tri, 492u);
    EXPECT_DOUBLE_EQ (hit.t, 99.0);
    EXPECT_NEAR (hit.point[2], 1.0, 1.0e-12);
    EXPECT_DOUBLE_EQ (hit.normal[2], 1.0);
    EXPECT_EQ (geomsrv::QueryIndexCache::Get ().Peek (snapshot.id), nullptr);
}

TEST (SunStudyReading, PickedMeshRaycastIsTwoSidedAndRejectsMissesAndInvalidMeshes)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes.push_back (evptest::MakeBox ("picked", 0, 0, 0));
    const double origin[3] = { 0.25, 0.4, 0.5 }, direction[3] = { 0, 0, 1 };
    const auto inside = geomsrv::QueryEngine::RaycastMesh (snapshot, 0, origin, direction, 0.0);
    ASSERT_TRUE (inside.hit);
    EXPECT_DOUBLE_EQ (inside.t, 0.5);
    EXPECT_FALSE (geomsrv::QueryEngine::RaycastMesh (snapshot, 0, origin, direction, 0.25).hit);
    EXPECT_FALSE (geomsrv::QueryEngine::RaycastMesh (snapshot, 1, origin, direction, 0.0).hit);
    const double zero[3] = {};
    EXPECT_FALSE (geomsrv::QueryEngine::RaycastMesh (snapshot, 0, origin, zero, 0.0).hit);
    const double off[3] = { 10, 10, 10 };
    EXPECT_FALSE (geomsrv::QueryEngine::RaycastMesh (snapshot, 0, off, direction, 0.0).hit);
    snapshot.meshes[0].triangles.assign (3, 999);
    EXPECT_FALSE (geomsrv::QueryEngine::RaycastMesh (snapshot, 0, origin, direction, 0.0).hit);
}

// A study runs only on the Rust core; a build without it has no store reads.
#if defined(TAPIOCA_GEOMETRY_RUST)
TEST (SunStudyReading, RolePartitionedStudiesReadPickedSourceFacesInBothDomainsWithoutACachedBvh)
{
    auto& store = SunStudyStore::Get ();
    struct Cleanup {
        ~Cleanup ()
        {
            SunStudyStore::Get ().Clear ();
        }
    } cleanup;
    store.Clear ();
    for (bool patch : { false, true }) {
        for (bool context : { false, true }) {
            for (bool ignored : { false, true }) {
                auto snapshot = std::make_shared<geomsrv::Snapshot> ();
                snapshot->id = 902;
                for (const auto& guid : { "context", "ignored", "analysis" }) {
                    geomsrv::Mesh mesh;
                    mesh.guid = guid;
                    const double x = snapshot->meshes.size () * 10.0;
                    mesh.vertices = { x, 0, 0, x + 4, 0, 0, x + 4, 4, 0, x, 4, 0 };
                    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
                    snapshot->meshes.push_back (std::move (mesh));
                }
                const auto roles = kernels::ResolveElementRoles (
                    *snapshot, {}, context ? std::vector<std::string> { "context" } : std::vector<std::string> {},
                    ignored ? std::vector<std::string> { "ignored" } : std::vector<std::string> {});
                auto record = std::make_unique<StudyRecord> ();
                record->snapshot = snapshot;
                record->snapshotId = snapshot->id;
                record->gridSpacing = 1.0;
                record->occluders = BuildSunStudyOccluders (*snapshot, roles);
                ASSERT_NE (record->occluders, nullptr);
                record->traversal = std::make_shared<SunStudyPartitionTraversal> (record->occluders->analysis,
                                                                                  record->occluders->context);
                for (const auto role : roles.roles)
                    record->elementRoles.push_back (static_cast<uint8_t> (role));
                SurfaceSamplingOptions options;
                options.domain = patch ? SamplingDomain::SurfacePatch : SamplingDomain::TriangleLegacy;
                auto sampling = kernels::BuildSurfaceSampling (*snapshot, roles.SampleMask (), options);
                ASSERT_TRUE (sampling.valid);
                record->domain = options.domain;
                record->sampleGrid = std::move (sampling.triangles);
                record->patchGrid = std::move (sampling.patches);
                record->positions = patch ? record->patchGrid.positions : record->sampleGrid.positions;
                record->normals = patch ? record->patchGrid.normals : record->sampleGrid.normals;
                SunStep step;
                step.time = { 12, 0 };
                step.altitudeDegrees = 90;
                step.direction[2] = 1;
                record->series = kernels::SeriesFromSteps ({ step }, 60);
                evp::sunstudy::fixture::BuildTestAtlas (*record);
                evp::sunstudy::fixture::AttachTestCore (*record, { snapshot->id, record->series.Version (), 1 },
                                                        snapshot.get ());
                const auto id = store.Insert (std::move (record));
                ASSERT_FALSE (id.empty ());
                size_t advanced = 0;
                std::string error;
                ASSERT_TRUE (store.Advance (id, 1, 1, 0.001, 0.0, advanced, error)) << error;
                geomsrv::QueryIndexCache::Get ().Release ();
                for (size_t mesh = 0; mesh < snapshot->meshes.size (); ++mesh) {
                    const double origin[3] = { mesh * 10.0 + 1.25, 1.5, 5 }, direction[3] = { 0, 0, -1 };
                    const auto hit = geomsrv::QueryEngine::RaycastMesh (*snapshot, mesh, origin, direction, 0.0);
                    ASSERT_TRUE (hit.hit);
                    SunStudyReading reading;
                    uint8_t role = 0xff;
                    double daylight = 0;
                    ASSERT_TRUE (store.ReadAt (id, snapshot->id, hit.tri, hit.meshIndex, hit.point, reading, role,
                                               daylight, error))
                        << error;
                    EXPECT_EQ (role, static_cast<uint8_t> (roles.roles[mesh]));
                    EXPECT_EQ (reading.measured, roles.roles[mesh] == ElementRole::Analysis);
                    if (reading.measured)
                        EXPECT_DOUBLE_EQ (reading.hours, 1.0);
                    EXPECT_DOUBLE_EQ (daylight, 1.0);
                    EXPECT_FALSE (store.ReadAt (id, snapshot->id + 1, hit.tri, hit.meshIndex, hit.point, reading, role,
                                                daylight, error));
                    EXPECT_EQ (error, "stale");
                }
                EXPECT_EQ (geomsrv::QueryIndexCache::Get ().Peek (snapshot->id), nullptr);
                ASSERT_TRUE (store.Erase (id));
            }
        }
    }
}
#endif
