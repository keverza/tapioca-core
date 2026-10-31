// A study's every store reader -- and the display packet the overlay draws --
// against the C++ kernels over the record's own grids and atlases
// (sunstudy_kernel_oracle.hpp), after every slice, cold and through a reuse
// rerun; and a rerun that reused samples ends where a cold study of the same
// model ends. These replaced the C++-against-Rust flip comparison when the
// C++ study core was deleted (P7a); the atlas-mismatch refusal is pinned too.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "sunstudy_kernel_oracle.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace evp::sunstudy;
using evp::sunstudy::fixture::ExpectStoreMatchesKernels;
using evp::sunstudy::fixture::SameBytes;

void AppendQuad (geomsrv::Mesh& mesh, const double o[3], const double u[3], const double v[3], int cells)
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
            for (int axis = 0; axis < 3; ++axis)
                mesh.vertices.push_back (o[axis] + u[axis] * i / cells + v[axis] * j / cells);
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const uint32_t a = base + j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
            mesh.triangles.insert (mesh.triangles.end (), { a, b, d, a, d, c });
        }
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
}

geomsrv::Mesh Box (const std::string& guid, double x, double y, double w, double d, double h)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double top[3] = { x, y, h }, ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h };
    const double south[3] = { x, y, 0 }, north[3] = { x + w, y + d, 0 }, wx[3] = { -w, 0, 0 }, dy[3] = { 0, -d, 0 };
    AppendQuad (mesh, top, ex, ey, 2);
    AppendQuad (mesh, south, ez, ex, 2);
    AppendQuad (mesh, north, ez, wx, 2);
    AppendQuad (mesh, south, ey, ez, 2);
    AppendQuad (mesh, north, dy, ez, 2);
    return mesh;
}

geomsrv::Mesh Floor (const std::string& guid)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double o[3] = { -10, -20, 0 }, u[3] = { 30, 0, 0 }, v[3] = { 0, 40, 0 };
    AppendQuad (mesh, o, u, v, 3);
    return mesh;
}

std::shared_ptr<geomsrv::Snapshot> Scene (uint64_t id, double towerX)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = id;
    snapshot->meshes = { Floor ("{0C000000-0000-0000-0000-000000000001}"),
                         Box ("{0C000000-0000-0000-0000-000000000002}", towerX, -2, 4, 4, 9),
                         Box ("{0C000000-0000-0000-0000-000000000003}", 12, 6, 3, 2, 3) };
    return snapshot;
}

SunSeries Day (size_t count)
{
    std::vector<SunStep> steps;
    for (size_t i = 0; i < count; ++i) {
        SunStep step;
        step.time = { 7 + static_cast<int> (i / 4), static_cast<int> ((i % 4) * 15) };
        const double azimuth = 0.3 + 0.21 * double (i), altitude = 0.25 + 0.05 * double (i % 9);
        step.direction[0] = std::cos (altitude) * std::cos (azimuth);
        step.direction[1] = std::cos (altitude) * std::sin (azimuth);
        step.direction[2] = std::sin (altitude);
        step.altitudeDegrees = altitude * 57.29;
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, 15);
}

enum class Domain { Ground, Triangle, Patch };

// The start path in the order SunStudyCommands runs it, through the public
// functions it calls. `previous` is the reuse source.
std::string Start (std::shared_ptr<geomsrv::Snapshot> snapshot, Domain domain,
                   std::shared_ptr<const StudyRecord> previous)
{
    auto record = std::make_unique<StudyRecord> ();
    record->snapshotId = snapshot->id;
    record->series = Day (13);
    record->gridSpacing = 1.5;
    record->traversal = std::make_shared<CpuTraversal> (std::make_shared<geomsrv::QueryEngine> (snapshot));
    const auto roles = ResolveElementRoles (*snapshot, {}, {}, {});
    for (const auto role : roles.roles)
        record->elementRoles.push_back (static_cast<uint8_t> (role));
    uint64_t gridVersion = 1;
    if (domain == Domain::Ground) {
        const GroundGrid grid = MakeGroundSampleGrid ({ -12, -22, 0 }, { 22, 22, 9 }, 1.5, 2.0, 0.1);
        record->positions = grid.positions;
        record->normals = grid.normals;
    }
    else {
        SurfaceSamplingOptions options;
        options.domain = domain == Domain::Patch ? SamplingDomain::SurfacePatch : SamplingDomain::TriangleLegacy;
        options.spacing = 1.5;
        auto sampling = BuildSurfaceSampling (*snapshot, roles.SampleMask (), options, previous.get ());
        record->samplingLayout = std::move (sampling.layout);
        if (domain == Domain::Patch) {
            record->domain = SamplingDomain::SurfacePatch;
            record->positions = sampling.patches.positions;
            record->normals = sampling.patches.normals;
            if (previous != nullptr && previous->IsPatchDomain ())
                record->patchAtlas = previous->patchAtlas;
            record->patchAtlas.Fit (sampling.patches, 1, 8192);
            record->patchGrid = std::move (sampling.patches);
        }
        else {
            record->positions = sampling.triangles.positions;
            record->normals = sampling.triangles.normals;
            record->atlas = BuildStableTriangleAtlas (sampling.triangles, *snapshot, record->triangleAllocations,
                                                      previous != nullptr ? &previous->triangleAllocations : nullptr);
            record->sampleGrid = std::move (sampling.triangles);
        }
        gridVersion = record->positions.size ();
    }
    std::string error;
    EXPECT_TRUE (AttachRustStudyCore (*record, *snapshot, previous.get (), 8192, error)) << error;
    SyncStudy (*record, { snapshot->id, record->series.Version (), gridVersion });
    record->snapshot = snapshot;
    SetSampleMeshes (*record);
    if (previous != nullptr) {
        const std::atomic<bool> notCancelled { false };
        ReuseUnaffectedSamples (*previous, *record, notCancelled);
    }
    return SunStudyStore::Get ().Insert (std::move (record));
}

// The display packet against the store's own readers: the overlay draws what
// the store answers.
void ExpectPacketMatchesStore (const std::string& id, const std::string& what)
{
    auto& store = SunStudyStore::Get ();
    std::string error;
    StepMaskAtlas masks;
    std::vector<uint16_t> minutes;
    uint32_t noon = 0;
    const bool hasMasks = store.StepMasks (id, masks, minutes, noon, error);
    std::vector<AtlasTile> tiles;
    std::vector<FaceLayout> layouts;
    std::vector<float> image;
    uint32_t w = 0, h = 0;
    double spacing = 0, daylight = 0;
    bool converged = false;
    uint64_t generation = 0;
    const bool display =
        store.DisplayData (id, tiles, layouts, w, h, spacing, image, daylight, converged, generation, error);
    store.ReadDisplayRecord (
        id, store.Revision (id),
        [&] (const StudyRecord& record) {
            std::string reason;
            const auto packet = PrepareStudyDisplayData (record, reason);
            ASSERT_EQ (packet != nullptr, display && hasMasks) << what << ": " << reason;
            if (packet == nullptr)
                return;
            const size_t patchTiles = record.IsPatchDomain () ? tiles.size () : 0u;
            EXPECT_TRUE (packet->width == w && packet->height == h && SameBytes (*packet->image, image) &&
                         SameBytes (*packet->stepMasks, masks.masks) && packet->stepMinutes == minutes &&
                         packet->noonStep == noon && packet->patchTiles.size () == patchTiles)
                << what << " display packet";
        },
        error);
}

void AdvanceChecked (const std::string& id, const std::string& what)
{
    for (int slice = 0; slice < 20; ++slice) {
        size_t advanced = 0;
        std::string e;
        ASSERT_TRUE (SunStudyStore::Get ().Advance (id, 4, 1, 0.001, 0.0, advanced, e)) << e;
        ExpectStoreMatchesKernels (id, what + " slice " + std::to_string (slice));
        ExpectPacketMatchesStore (id, what + " slice " + std::to_string (slice));
        if (advanced == 0 || ::testing::Test::HasFatalFailure ())
            return;
    }
}

std::vector<uint8_t> FinalBits (const std::string& id)
{
    std::vector<double> hours, positions, normals;
    std::vector<uint8_t> bits;
    std::string error;
    size_t advanced = 0;
    EXPECT_TRUE (SunStudyStore::Get ().Advance (id, 1000, 1, 0.001, 0.0, advanced, error)) << error;
    EXPECT_TRUE (SunStudyStore::Get ().Results (id, hours, positions, normals, &bits, error)) << error;
    return bits;
}

struct Cleanup {
    Cleanup ()
    {
        SunStudyStore::Get ().Clear ();
    }
    ~Cleanup ()
    {
        SunStudyStore::Get ().Clear ();
    }
};

} // namespace

TEST (SunStudyRustFlip, EveryStoreReaderAndTheDisplayPacketMatchTheKernels)
{
    Cleanup cleanup;
    for (const Domain domain : { Domain::Ground, Domain::Triangle, Domain::Patch }) {
        const std::string what = domain == Domain::Ground     ? "ground"
                                 : domain == Domain::Triangle ? "triangle"
                                                              : "patch";
        const auto scene = Scene (1, 2);
        const std::string id = Start (scene, domain, nullptr);
        ASSERT_FALSE (id.empty ());
        ExpectStoreMatchesKernels (id, what + " fresh");
        AdvanceChecked (id, what);
        if (::testing::Test::HasFatalFailure ())
            return;

        // A rerun after an edit reuses the unaffected samples, and ends where a
        // cold study of the edited model ends.
        const auto source = SunStudyStore::Get ().CompletedRecord (id);
        ASSERT_NE (source, nullptr) << what;
        const auto edited = Scene (2, 3.5);
        const std::string rerun = Start (edited, domain, source);
        const std::string cold = Start (edited, domain, nullptr);
        StudyRecord metadata;
        std::string error;
        ASSERT_TRUE (SunStudyStore::Get ().Describe (rerun, metadata, error)) << error;
        if (domain != Domain::Ground)
            EXPECT_GT (metadata.reusedSamples, 0u) << what << ": the rerun must reuse something";
        ExpectStoreMatchesKernels (rerun, what + " seeded");
        AdvanceChecked (rerun, what + " rerun");
        EXPECT_EQ (FinalBits (rerun), FinalBits (cold)) << what << ": reuse is exact";
    }
}

TEST (SunStudyRustFlip, ACorelessSourceSeedsNothingAndAMismatchIsRefused)
{
    Cleanup cleanup;
    const auto scene = Scene (1, 2);
    const std::string id = Start (scene, Domain::Patch, nullptr);
    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Advance (id, 64, 1, 0.001, 0.0, advanced, error)) << error;
    const auto source = SunStudyStore::Get ().CompletedRecord (id);
    ASSERT_NE (source, nullptr);

    // A source without a core has no bits to give: the target is traced afresh.
    auto coreless = std::make_shared<StudyRecord> ();
    coreless->snapshot = source->snapshot;
    coreless->snapshotId = source->snapshotId;
    coreless->series = source->series;
    coreless->gridSpacing = source->gridSpacing;
    coreless->domain = source->domain;
    coreless->positions = source->positions;
    coreless->normals = source->normals;
    coreless->sampleMeshes = source->sampleMeshes;
    coreless->elementRoles = source->elementRoles;
    const std::string target = Start (Scene (2, 2), Domain::Patch, coreless);
    StudyRecord metadata;
    ASSERT_TRUE (SunStudyStore::Get ().Describe (target, metadata, error));
    EXPECT_EQ (metadata.reusedSamples, 0u);

    // A record whose atlas disagrees with the core it would get is refused.
    auto record = std::make_unique<StudyRecord> ();
    record->series = Day (3);
    record->gridSpacing = 1.5;
    SurfaceSamplingOptions options;
    options.domain = SamplingDomain::SurfacePatch;
    options.spacing = 1.5;
    const auto roles = ResolveElementRoles (*scene, {}, {}, {});
    auto sampling = BuildSurfaceSampling (*scene, roles.SampleMask (), options);
    record->domain = SamplingDomain::SurfacePatch;
    record->positions = sampling.patches.positions;
    record->normals = sampling.patches.normals;
    record->patchGrid = std::move (sampling.patches);
    // No fit: the record has no atlas, the core would.
    EXPECT_FALSE (AttachRustStudyCore (*record, *scene, nullptr, 8192, error));
    EXPECT_EQ (record->rustCore, nullptr);
    EXPECT_FALSE (error.empty ());

    // And a record without a core is refused by every reader rather than read.
    const std::string orphan = SunStudyStore::Get ().Insert (std::move (record));
    std::vector<double> hours, positions, normals;
    EXPECT_FALSE (SunStudyStore::Get ().Results (orphan, hours, positions, normals, nullptr, error));
    EXPECT_EQ (error, NoStudyCoreError ());
}
