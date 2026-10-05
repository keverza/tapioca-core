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
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyReading.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "MeshFixtures.hpp"
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
    return BuildPatchSampleGrid (scene.vertices.data (), scene.vertices.size () / 3, scene.triangles.data (),
                                 scene.Faces (), scene.groups.data (), scene.elementOf, options);
}

} // namespace

TEST (SunStudyReading, EveryPatchSampleReadsItselfFromEveryTriangleOfItsSurface)
{
    const Scene scene;
    const PatchSampleGrid grid = Patches (scene, 0.7); // does not divide either rectangle
    ASSERT_TRUE (grid.valid);
    const std::vector<double> hours = Hours (grid.Count ());

    for (size_t sample = 0; sample < grid.Count (); ++sample) {
        const double point[3] = { grid.positions[sample * 3], grid.positions[sample * 3 + 1],
                                  grid.positions[sample * 3 + 2] };
        for (size_t face = 0; face < scene.Faces (); ++face) {
            if (grid.patchOfTriangle[face] != grid.spanOf[sample])
                continue;
            const SunStudyReading reading = ReadPatchStudyAt (grid, 0.7, hours, face, point);
            ASSERT_TRUE (reading.measured);
            EXPECT_EQ (reading.sample, sample) << "through face " << face;
            EXPECT_FALSE (reading.nearest);
            EXPECT_DOUBLE_EQ (reading.hours, hours[sample]);
        }
    }
}

TEST (SunStudyReading, EveryTriangleSampleReadsItself)
{
    const Scene scene;
    SamplerOptions options;
    options.spacing = 0.7;
    options.normalOffset = 0.0;
    options.wantLayouts = true;
    const SampleGrid grid = BuildSampleGrid (scene.vertices.data (), scene.vertices.size () / 3,
                                             scene.triangles.data (), scene.Faces (), scene.groups.data (), options);
    ASSERT_TRUE (grid.valid);
    const std::vector<double> hours = Hours (grid.Count ());

    for (size_t sample = 0; sample < grid.Count (); ++sample) {
        const double point[3] = { grid.positions[sample * 3], grid.positions[sample * 3 + 1],
                                  grid.positions[sample * 3 + 2] };
        const SunStudyReading reading = ReadTriangleStudyAt (grid, 0.7, hours, grid.faces[sample], point);
        ASSERT_TRUE (reading.measured);
        EXPECT_EQ (reading.sample, sample);
        EXPECT_DOUBLE_EQ (reading.hours, hours[sample]);
    }
}

TEST (SunStudyReading, TheReaderAndTheShaderReadTheSameCellAcrossTheSurface)
{
    // ⚠️ THE DECISIVE ONE. A dense walk of points over the wall, including cell
    // boundaries: wherever the reader found the cell's own sample, the shader's
    // arithmetic on the real display record lands on that sample's texel.
    const Scene scene;
    const PatchSampleGrid grid = Patches (scene, 0.7);
    SunStudyPatchAtlas atlas;
    atlas.Fit (grid);
    std::vector<AtlasTile> tiles;
    std::vector<FaceLayout> layouts;
    ASSERT_TRUE (PatchFaceArrays (grid, atlas, tiles, layouts));
    geomsrv::archviz::SunStudyElementMap map;
    const std::vector<int32_t> material (scene.Faces (), 0); // identity draw order
    ASSERT_TRUE (geomsrv::archviz::BuildSunStudyElementMap (tiles, layouts, 0.7, scene.triangles, material, 0, map));
    const std::vector<double> hours = Hours (grid.Count ());

    size_t checked = 0;
    for (double x = 0.01; x < 6.0; x += 0.13) {
        for (double y = 0.01; y < 2.0; y += 0.11) {
            const double point[3] = { x, y, 0.0 };
            const size_t face = 0; // any triangle of the wall: one record for all
            const SunStudyReading reading = ReadPatchStudyAt (grid, 0.7, hours, face, point);
            ASSERT_TRUE (reading.measured);
            if (reading.nearest)
                continue;
            const int64_t shaderTexel =
                geomsrv::archviz::SunStudyTexelAt (map.faces[face], point, atlas.Width (), atlas.Height ());
            EXPECT_EQ (shaderTexel, atlas.TexelOf (grid, reading.sample)) << "at " << x << ", " << y;
            ++checked;
        }
    }
    EXPECT_GT (checked, 500u);
}

TEST (SunStudyReading, AContextSurfaceHasNoMeasurement)
{
    const Scene scene;
    const std::vector<uint8_t> analysed { 1u, 0u }; // the slab is context
    const PatchSampleGrid grid = Patches (scene, 1.0, &analysed);
    const double point[3] = { 21.0, 1.0, 0.0 };
    const SunStudyReading reading = ReadPatchStudyAt (grid, 1.0, Hours (grid.Count ()), 12, point);
    EXPECT_FALSE (reading.measured) << "a context surface reported some other surface's hours";
}

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
                const auto roles = ResolveElementRoles (
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
                auto sampling = BuildSurfaceSampling (*snapshot, roles.SampleMask (), options);
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
                record->series = SunSeries::FromSteps ({ step }, 60);
                record->session.Sync ({ snapshot->id, record->series.Version (), 1 }, record->series,
                                      record->Samples ());
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
