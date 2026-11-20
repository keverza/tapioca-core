// The solar cut over a study prepared through the kernel facade (Rust in a Rust
// build, P7b) against the same study prepared by the C++ kernels: what the cut
// reads from the record -- series, cells, normals, patch grid, roles,
// occluders -- and everything the cut then draws must be the same. A live run
// after the facade switch reported lit regions where no sun should reach; this
// pins whether the switch can be the cause.

#include "ArchViz/SunStudyObstructionController.hpp"

#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

using namespace geomsrv::archviz;
using namespace evp::sunstudy;

namespace {

void AddQuad (geomsrv::Mesh& mesh, const double o[3], const double u[3], const double v[3])
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int corner = 0; corner < 4; ++corner) {
        const double a = corner == 1 || corner == 2 ? 1.0 : 0.0, b = corner >= 2 ? 1.0 : 0.0;
        for (int axis = 0; axis < 3; ++axis)
            mesh.vertices.push_back (o[axis] + a * u[axis] + b * v[axis]);
    }
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

// A closed box, outward faces.
void AddBox (geomsrv::Mesh& mesh, double x, double y, double z, double w, double d, double h)
{
    const double ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h };
    const double nx[3] = { -w, 0, 0 }, ny[3] = { 0, -d, 0 }, nz[3] = { 0, 0, -h };
    const double low[3] = { x, y, z }, top[3] = { x, y, z + h }, far[3] = { x + w, y + d, z };
    AddQuad (mesh, top, ex, ey); // top
    const double under[3] = { x, y + d, z };
    AddQuad (mesh, under, ex, ny); // bottom
    AddQuad (mesh, low, ex, ez);   // south
    AddQuad (mesh, far, nx, ez);   // north
    AddQuad (mesh, low, ez, ey);   // west
    AddQuad (mesh, far, ez, ny);   // east
    (void) nz;
}

geomsrv::Mesh Element (const std::string& guid)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    return mesh;
}

void Finish (geomsrv::Mesh& mesh)
{
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
    for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3)
        mesh.bounds.Expand (mesh.vertices[v], mesh.vertices[v + 1], mesh.vertices[v + 2]);
}

// The reported kind of scene: ground, five stacked slabs (the third with a
// cut-out made of four slab pieces), a tower beside them.
std::shared_ptr<geomsrv::Snapshot> Scene ()
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 911;
    snapshot->captureStamp = 912;
    char guid[64];
    int next = 1;
    const auto add = [&] (const std::function<void (geomsrv::Mesh&)>& build) {
        std::snprintf (guid, sizeof guid, "{0D000000-0000-0000-0000-%012d}", next++);
        geomsrv::Mesh mesh = Element (guid);
        build (mesh);
        Finish (mesh);
        snapshot->meshes.push_back (std::move (mesh));
    };
    add ([] (geomsrv::Mesh& m) { AddBox (m, -20, -20, -0.5, 50, 45, 0.5); });
    for (int level = 0; level < 5; ++level) {
        const double z = 3.0 + 3.2 * level;
        if (level == 2) {
            add ([z] (geomsrv::Mesh& m) {
                AddBox (m, 0, 0, z, 12, 3, 0.3);
                AddBox (m, 0, 7, z, 12, 3, 0.3);
                AddBox (m, 0, 3, z, 4, 4, 0.3);
                AddBox (m, 8, 3, z, 4, 4, 0.3);
            });
        }
        else
            add ([z] (geomsrv::Mesh& m) { AddBox (m, 0, 0, z, 12, 10, 0.3); });
    }
    add ([] (geomsrv::Mesh& m) { AddBox (m, 16, -6, 0, 5, 5, 22); });
    return snapshot;
}

SunSeries Day ()
{
    std::vector<SunStep> steps;
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    for (int minute = 6 * 60; minute <= 18 * 60; minute += 15) {
        const double hour = (minute - 12 * 60) / 60.0;
        const double altitude = (52.0 - 4.0 * hour * hour) * kDeg;
        const double azimuth = (180.0 + 15.0 * hour) * kDeg;
        SunStep step;
        step.time = { minute / 60, minute % 60 };
        step.altitudeDegrees = altitude / kDeg;
        step.direction[0] = std::cos (altitude) * std::sin (azimuth);
        step.direction[1] = std::cos (altitude) * std::cos (azimuth);
        step.direction[2] = std::sin (altitude);
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, 15);
}

// One study as StartSunStudy builds it in the patch domain, through the C++
// kernels or through the facade.
std::string Study (const std::shared_ptr<geomsrv::Snapshot>& snapshot, bool facade)
{
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    const SunSeries day = Day ();
    record->series = facade ? kernels::SeriesFromSteps (day.Steps (), 15) : day;
    record->timestepMinutes = 15;
    record->hourFrom = 0;
    record->hourTo = 24;
    record->gridSpacing = 1.0;
    const auto roles =
        facade ? kernels::ResolveElementRoles (*snapshot, {}, {}, {}) : ResolveElementRoles (*snapshot, {}, {}, {});
    for (const auto role : roles.roles)
        record->elementRoles.push_back (uint8_t (role));
    SurfaceSamplingOptions options;
    options.domain = SamplingDomain::SurfacePatch;
    options.spacing = 1.0;
    options.normalOffset = 0.10;
    auto sampling = facade ? kernels::BuildSurfaceSampling (*snapshot, roles.SampleMask (), options)
                           : BuildSurfaceSampling (*snapshot, roles.SampleMask (), options);
    if (!sampling.valid)
        return {};
    record->samplingLayout = std::move (sampling.layout);
    record->domain = SamplingDomain::SurfacePatch;
    record->positions = sampling.patches.positions;
    record->normals = sampling.patches.normals;
    if (facade)
        kernels::FitPatchAtlas (record->patchAtlas, sampling.patches, 1, 8192);
    else
        record->patchAtlas.Fit (sampling.patches, 1, 8192);
    record->patchGrid = std::move (sampling.patches);
    record->occluders = BuildSunStudyOccluders (*snapshot, roles);
    record->traversal =
        std::make_shared<SunStudyPartitionTraversal> (record->occluders->analysis, record->occluders->context);
    const StudyInputs inputs { snapshot->id, record->series.Version (),
                               record->patchGrid.Count () * 73856093ull ^ record->patchGrid.spans.size () };
    std::string error;
    if (!AttachRustStudyCore (*record, *snapshot, nullptr, 8192, error))
        return {};
    SyncStudy (*record, inputs);
    SetSampleMeshes (*record);
    auto& store = SunStudyStore::Get ();
    const std::string id = store.Insert (std::move (record));
    size_t advanced = 0;
    if (id.empty () || !store.Advance (id, 100000, 4, 0.001, 0.0, advanced, error))
        return {};
    return id;
}

std::shared_ptr<const SunObstructionFrame> Cut (SunObstructionController& controller, const std::string& id,
                                                size_t cell)
{
    SunObstructionRequest request;
    request.studyId = id;
    request.samples = { cell };
    request.mode = SunObstructionMode::SolarCut;
    request.fromMinute = 690;
    request.toMinute = 810;
    const auto started = std::chrono::steady_clock::now ();
    while (std::chrono::steady_clock::now () - started < std::chrono::seconds (60)) {
        controller.Request (request);
        const auto frame = controller.Latest ();
        if (frame != nullptr && frame->cutStage == SunSolarCutStage::Done && !controller.Busy ())
            return frame;
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return nullptr;
}

struct StoreReset {
    ~StoreReset ()
    {
        SunStudyStore::Get ().Clear ();
    }
};

} // namespace

TEST (SunStudySolarCutFacade, TheCutOverAFacadeStudyIsTheCutOverACppStudy)
{
    StoreReset reset;
    const auto snapshot = Scene ();
    const std::string cppId = Study (snapshot, false), facadeId = Study (snapshot, true);
    ASSERT_FALSE (cppId.empty ());
    ASSERT_FALSE (facadeId.empty ());

    // What the cut reads from the two records.
    auto& store = SunStudyStore::Get ();
    const auto e = store.CompletedRecord (cppId), a = store.CompletedRecord (facadeId);
    ASSERT_NE (e, nullptr);
    ASSERT_NE (a, nullptr);
    EXPECT_EQ (a->series.Version (), e->series.Version ());
    EXPECT_EQ (a->elementRoles, e->elementRoles);
    EXPECT_EQ (a->positions, e->positions);
    EXPECT_EQ (a->normals, e->normals);
    EXPECT_EQ (a->sampleMeshes, e->sampleMeshes);
    EXPECT_EQ (a->patchGrid.spanOf, e->patchGrid.spanOf);
    EXPECT_EQ (a->patchGrid.cellColumns, e->patchGrid.cellColumns);
    EXPECT_EQ (a->patchGrid.cellRows, e->patchGrid.cellRows);
    ASSERT_EQ (a->patchGrid.spans.size (), e->patchGrid.spans.size ());
    for (size_t s = 0; s < e->patchGrid.spans.size (); ++s) {
        EXPECT_EQ (std::memcmp (a->patchGrid.spans[s].origin, e->patchGrid.spans[s].origin, sizeof (double) * 3), 0);
        EXPECT_EQ (std::memcmp (a->patchGrid.spans[s].uAxis, e->patchGrid.spans[s].uAxis, sizeof (double) * 3), 0);
        EXPECT_EQ (std::memcmp (a->patchGrid.spans[s].vAxis, e->patchGrid.spans[s].vAxis, sizeof (double) * 3), 0);
    }
    const size_t samples = e->positions.size () / 3;
    ASSERT_GT (samples, 100u);

    // The cut itself, cell by cell.
    SunObstructionController controller;
    size_t won = 0;
    // Cells in midday shadow: the ground north of the slabs and of the tower,
    // and the slab under the cut-out; a few spread cells besides.
    std::vector<size_t> cells;
    for (size_t s = 0; s < samples && cells.size () < 24; s += 3) {
        const double x = e->positions[3 * s], y = e->positions[3 * s + 1], z = e->positions[3 * s + 2];
        const bool ground =
            z < 0.5 && ((x > 1 && x < 11 && y > 10.5 && y < 18) || (x > 16 && x < 21 && y > 0 && y < 12));
        const bool underHole = z > 9 && z < 10 && x > 4 && x < 8 && y > 3 && y < 7;
        if (ground || underHole)
            cells.push_back (s);
    }
    for (size_t i = 0; i < 6; ++i)
        cells.push_back ((i * 7919 + 13) % samples);
    for (const size_t cell : cells) {
        const auto ef = Cut (controller, cppId, cell);
        const auto af = Cut (controller, facadeId, cell);
        ASSERT_NE (ef, nullptr) << "cell " << cell;
        ASSERT_NE (af, nullptr) << "cell " << cell;
        const std::string what = "cell " + std::to_string (cell);
        EXPECT_EQ (af->error, ef->error) << what;
        EXPECT_EQ (af->removedVolume, ef->removedVolume) << what;
        EXPECT_EQ (af->wonCells, ef->wonCells) << what;
        EXPECT_EQ (af->cutObjects.size (), ef->cutObjects.size ()) << what;
        EXPECT_EQ (af->cutCells.samples, ef->cutCells.samples) << what;
        EXPECT_EQ (af->cutCells.before, ef->cutCells.before) << what;
        EXPECT_EQ (af->cutCells.after, ef->cutCells.after) << what;
        EXPECT_EQ (af->cutCells.outline, ef->cutCells.outline) << what;
        ASSERT_EQ (af->exposure.triangles.size (), ef->exposure.triangles.size ()) << what;
        for (size_t t = 0; t < ef->exposure.triangles.size (); ++t) {
            EXPECT_EQ (af->exposure.triangles[t].mesh, ef->exposure.triangles[t].mesh) << what;
            EXPECT_EQ (std::memcmp (af->exposure.triangles[t].v, ef->exposure.triangles[t].v, sizeof (double) * 9), 0)
                << what << " lit triangle " << t;
        }
        EXPECT_EQ (af->exposure.note, ef->exposure.note) << what;
        won += ef->wonCells > 0 || !ef->exposure.triangles.empty () ? 1 : 0;
    }
    controller.Request ({});
    EXPECT_GT (won, 8u) << "the scene must give the cut something to win";
}
