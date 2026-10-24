// Differential tests: the Rust study core (the portable half of a split
// StudyRecord) against SunStudyStore's readers over a C++ record built from the
// same inputs. The store is the oracle; arrays are compared BIT FOR BIT.
//
// Each case builds the record the way the start path does (surface sampling,
// stable triangle atlas or refitted patch atlas, sync, sample meshes, reuse),
// files it in the store, and builds the Rust core from the same grid and the
// previous core. Both are then advanced slice by slice with one traversal, and
// every reader is compared after every slice, so partial results count too.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyRustStudy.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace evp::sunstudy;
namespace rs = evp::sunstudy::rust;

class Lcg {
  public:
    explicit Lcg (uint64_t seed) : state_ (seed)
    {
    }
    double Uniform (double lo, double hi)
    {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return lo + (hi - lo) * (static_cast<double> (state_ >> 11) * (1.0 / 9007199254740992.0));
    }

  private:
    uint64_t state_;
};

template <typename T> bool SameBits (const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size () == b.size () && (a.empty () || std::memcmp (a.data (), b.data (), a.size () * sizeof (T)) == 0);
}

bool SameBits (double a, double b)
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

// A quad split into a `cells` x `cells` grid of triangle pairs, so a surface
// is many triangles (the patch domain's whole point) at no extra geometry.
void AppendQuad (geomsrv::Mesh& mesh, const double origin[3], const double u[3], const double v[3], int cells)
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
            for (int axis = 0; axis < 3; ++axis)
                mesh.vertices.push_back (origin[axis] + u[axis] * i / cells + v[axis] * j / cells);
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const uint32_t a = base + j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
            mesh.triangles.insert (mesh.triangles.end (), { a, b, d, a, d, c });
        }
    }
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
}

geomsrv::Mesh Box (const std::string& guid, double x, double y, double w, double d, double h)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double top[3] = { x, y, h }, ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h };
    AppendQuad (mesh, top, ex, ey, 2);
    const double south[3] = { x, y, 0 }, north[3] = { x + w, y + d, 0 };
    const double wx[3] = { -w, 0, 0 }, dy[3] = { 0, -d, 0 };
    AppendQuad (mesh, south, ez, ex, 2);
    AppendQuad (mesh, north, ez, wx, 2);
    AppendQuad (mesh, south, ey, ez, 2);
    AppendQuad (mesh, north, dy, ez, 2);
    return mesh;
}

geomsrv::Mesh Floor (const std::string& guid, double x, double w)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double origin[3] = { x, -20, 0 }, u[3] = { w, 0, 0 }, v[3] = { 0, 40, 0 };
    AppendQuad (mesh, origin, u, v, 3);
    return mesh;
}

SunSeries Day (size_t count)
{
    std::vector<SunStep> steps;
    for (size_t i = 0; i < count; ++i) {
        SunStep step;
        step.time = { 7 + static_cast<int> (i / 4), static_cast<int> ((i % 4) * 15) };
        const double azimuth = 0.3 + 0.21 * static_cast<double> (i),
                     altitude = 0.25 + 0.05 * static_cast<double> (i % 9);
        step.direction[0] = std::cos (altitude) * std::cos (azimuth);
        step.direction[1] = std::cos (altitude) * std::sin (azimuth);
        step.direction[2] = std::sin (altitude);
        step.altitudeDegrees = altitude * 57.29;
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, 15);
}

std::shared_ptr<geomsrv::Snapshot> Scene (Lcg& lcg, int edit, const geomsrv::Snapshot* base)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    if (base == nullptr) {
        snapshot->id = 1;
        snapshot->meshes = { Floor ("{0A000000-0000-0000-0000-000000000001}", -10, 30),
                             Box ("{0A000000-0000-0000-0000-000000000002}", 2, -2, 4, 4, 9),
                             Box ("{0A000000-0000-0000-0000-000000000003}", 12, 6, 3, 2, 3),
                             Floor ("{0A000000-0000-0000-0000-000000000004}", 80, 20),
                             // Smaller than a cell: one-sample (centroid) patches.
                             Box ("{0A000000-0000-0000-0000-000000000006}", 20.3, 15.1, 0.4, 0.3, 0.35),
                             // Long and thin: no cell centre inside, so one sample on a wider lattice.
                             Box ("{0A000000-0000-0000-0000-000000000007}", -5.1, 10.2, 3.4, 0.2, 0.2) };
        return snapshot;
    }
    *snapshot = *base;
    snapshot->id = base->id + 1;
    if (edit == 0) // move the tower
        snapshot->meshes[1] = Box ("{0A000000-0000-0000-0000-000000000002}", 2 + lcg.Uniform (0.5, 3), -2, 4, 4, 9);
    else if (edit == 1) // add a box
        snapshot->meshes.push_back (
            Box ("{0A000000-0000-0000-0000-000000000005}", lcg.Uniform (-5, 15), lcg.Uniform (-10, 10), 2, 2, 5));
    else if (edit == 2) // delete the shed
        snapshot->meshes.erase (snapshot->meshes.begin () + 2);
    return snapshot; // any other edit: unchanged geometry
}

enum class Domain { Ground, Triangle, Patch };

// One study on both sides: the C++ record filed in the store, the Rust core.
struct Study {
    std::string id;
    std::shared_ptr<const ITraversal> traversal;
    std::shared_ptr<const geomsrv::Snapshot> snapshot;
    std::vector<uint8_t> roles;
    std::shared_ptr<const StudyRecord> record; // the store's, for reuse
    std::unique_ptr<rs::RustStudyCore> core;
    Domain domain = Domain::Triangle;
    double spacing = 1.5;
};

constexpr uint32_t kMaxDimension = 8192;

// The start path's construction, on both sides. `previous` is the study being
// rerun (null for a cold start).
Study Start (std::shared_ptr<const geomsrv::Snapshot> snapshot, Domain domain, size_t steps, const Study* previous,
             double spacing = 1.5)
{
    Study study;
    study.domain = domain;
    study.spacing = spacing;
    study.snapshot = snapshot;
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->series = Day (steps);
    record->gridSpacing = spacing;
    record->traversal = std::make_shared<CpuTraversal> (std::make_shared<geomsrv::QueryEngine> (snapshot));
    study.traversal = record->traversal;
    const auto roles = ResolveElementRoles (*snapshot, {}, {}, {});
    for (const auto role : roles.roles)
        record->elementRoles.push_back (static_cast<uint8_t> (role));
    study.roles = record->elementRoles;
    const StudyRecord* source = previous != nullptr ? previous->record.get () : nullptr;
    const rs::RustStudyCore* sourceCore = previous != nullptr ? previous->core.get () : nullptr;
    std::string error;
    uint64_t gridVersion = 0;

    if (domain == Domain::Ground) {
        const GroundGrid grid = MakeGroundSampleGrid ({ -12, -22, 0 }, { 22, 22, 9 }, spacing, 2.0, 0.1);
        record->positions = grid.positions;
        record->normals = grid.normals;
        gridVersion = grid.columns * 73856093ull ^ grid.rows * 19349663ull;
        study.core = rs::RustStudyCore::Ground (record->series, spacing, grid.positions, grid.normals, error);
    }
    else {
        SurfaceSamplingOptions options;
        options.domain = domain == Domain::Patch ? SamplingDomain::SurfacePatch : SamplingDomain::TriangleLegacy;
        options.spacing = spacing;
        auto sampling = BuildSurfaceSampling (*snapshot, roles.SampleMask (), options, source);
        EXPECT_TRUE (sampling.valid);
        record->samplingLayout = std::move (sampling.layout);
        if (domain == Domain::Patch) {
            auto patches = std::move (sampling.patches);
            record->domain = SamplingDomain::SurfacePatch;
            record->positions = patches.positions;
            record->normals = patches.normals;
            if (source != nullptr && source->IsPatchDomain () && source->gridSpacing == spacing &&
                source->patchAtlas.Width () <= kMaxDimension)
                record->patchAtlas = source->patchAtlas;
            record->patchAtlas.Fit (patches, 1, kMaxDimension);
            gridVersion = patches.Count () * 73856093ull ^ patches.spans.size () * 19349663ull;
            study.core = rs::RustStudyCore::Patch (record->series, spacing, patches, sourceCore, kMaxDimension, error);
            record->patchGrid = std::move (patches);
        }
        else {
            auto samples = std::move (sampling.triangles);
            record->positions = samples.positions;
            record->normals = samples.normals;
            record->atlas = BuildStableTriangleAtlas (samples, *snapshot, record->triangleAllocations,
                                                      source != nullptr && !source->IsPatchDomain () &&
                                                              source->gridSpacing == spacing
                                                          ? &source->triangleAllocations
                                                          : nullptr);
            gridVersion = samples.Count () * 73856093ull;
            rs::RustStudyCore::TriangleInputs inputs;
            inputs.grid = &samples;
            inputs.snapshot = snapshot.get ();
            study.core = rs::RustStudyCore::Triangle (record->series, spacing, inputs, sourceCore, error);
            record->sampleGrid = std::move (samples);
        }
    }
    EXPECT_NE (study.core, nullptr) << error;
    const StudyInputs inputs { snapshot->id, record->series.Version (), gridVersion };
    record->session.Sync (inputs, record->series, record->Samples ());
    EXPECT_TRUE (study.core->Sync (inputs, error)) << error;
    SetSampleMeshes (*record);
    EXPECT_TRUE (study.core->SetSampleMeshes (*snapshot, error)) << error;
    if (source != nullptr) {
        const std::atomic<bool> notCancelled { false };
        const size_t e = ReuseUnaffectedSamples (*source, *record, notCancelled);
        rs::RustStudyCore::ReuseInputs reuse;
        reuse.sourceSnapshot = previous->snapshot.get ();
        reuse.sourceRoles = &previous->roles;
        reuse.targetSnapshot = snapshot.get ();
        reuse.targetRoles = &study.roles;
        reuse.sourceDefaultRayBounds = source->defaultRayBounds;
        reuse.cancelled = &notCancelled;
        size_t reused = 0, reusedSteps = 0;
        EXPECT_TRUE (study.core->ReuseFrom (*previous->core, reuse, reused, reusedSteps, error)) << error;
        EXPECT_EQ (reused, e);
        EXPECT_EQ (reused, record->reusedSamples);
        EXPECT_EQ (reusedSteps, record->reusedSampleSteps);
    }
    study.id = SunStudyStore::Get ().Insert (std::move (record));
    EXPECT_FALSE (study.id.empty ());
    return study;
}

void ExpectSameTiles (const std::vector<AtlasTile>& e, const std::vector<AtlasTile>& a, const std::string& what)
{
    ASSERT_EQ (a.size (), e.size ()) << what;
    for (size_t i = 0; i < e.size (); ++i)
        ASSERT_TRUE (a[i].x == e[i].x && a[i].y == e[i].y && a[i].width == e[i].width && a[i].height == e[i].height)
            << what << " tile " << i;
}

void ExpectSameLayouts (const std::vector<FaceLayout>& e, const std::vector<FaceLayout>& a, const std::string& what)
{
    ASSERT_EQ (a.size (), e.size ()) << what;
    for (size_t i = 0; i < e.size (); ++i) {
        const FaceLayout &x = e[i], &y = a[i];
        const bool same = std::memcmp (x.origin, y.origin, sizeof x.origin) == 0 &&
                          std::memcmp (x.uAxis, y.uAxis, sizeof x.uAxis) == 0 &&
                          std::memcmp (x.vAxis, y.vAxis, sizeof x.vAxis) == 0 && x.uStart == y.uStart &&
                          x.vStart == y.vStart && x.columns == y.columns && x.rows == y.rows && x.gridded == y.gridded;
        ASSERT_TRUE (same) << what << " layout " << i;
    }
}

// Every reader of the store against its Rust counterpart, on the hours as they
// stand.
void ExpectSameReads (const Study& study, const std::string& what)
{
    auto& store = SunStudyStore::Get ();
    const rs::RustStudyCore& core = *study.core;
    std::string e, a;

    StudyProgress pe, pa;
    ASSERT_TRUE (store.Progress (study.id, pe, e)) << e;
    ASSERT_TRUE (core.Progress (pa, a)) << a;
    EXPECT_EQ (pa.generation, pe.generation) << what;
    EXPECT_EQ (pa.resolvedSteps, pe.resolvedSteps) << what;
    EXPECT_EQ (pa.totalSteps, pe.totalSteps) << what;
    EXPECT_EQ (pa.sampleCount, pe.sampleCount) << what;
    EXPECT_EQ (pa.converged, pe.converged) << what;
    EXPECT_EQ (pa.empty, pe.empty) << what;

    std::vector<double> hours, positions, normals, rustHours;
    std::vector<uint8_t> bits, rustBits;
    ASSERT_TRUE (store.Results (study.id, hours, positions, normals, &bits, e)) << e;
    ASSERT_TRUE (core.SunHours (rustHours, a)) << a;
    ASSERT_TRUE (core.StepBits (rustBits, a)) << a;
    EXPECT_TRUE (SameBits (hours, rustHours)) << what << " hours";
    EXPECT_TRUE (SameBits (bits, rustBits)) << what << " step bits";

    SunStudyResultSummary se, sa;
    ASSERT_TRUE (store.Summary (study.id, se, e)) << e;
    ASSERT_TRUE (core.Summary (sa, a)) << a;
    EXPECT_EQ (sa.count, se.count) << what;
    EXPECT_EQ (sa.fullyLit, se.fullyLit) << what;
    EXPECT_EQ (sa.fullyShaded, se.fullyShaded) << what;
    EXPECT_TRUE (SameBits (sa.minHours, se.minHours) && SameBits (sa.meanHours, se.meanHours) &&
                 SameBits (sa.maxHours, se.maxHours) && SameBits (sa.daylightHours, se.daylightHours))
        << what << " summary";

    uint32_t we = 0, he = 0, wa = 0, ha = 0;
    std::vector<float> ie, ia;
    rs::StudyReadStatus status = rs::StudyReadStatus::Ok;
    const bool triangleImage = store.AtlasImage (study.id, we, he, ie, e);
    ASSERT_TRUE (core.AtlasImage (wa, ha, ia, status, a)) << a;
    EXPECT_EQ (status == rs::StudyReadStatus::Ok, triangleImage) << what << ": " << e;
    if (triangleImage)
        EXPECT_TRUE (we == wa && he == ha && SameBits (ie, ia)) << what << " triangle image";

    const bool patchImage = store.PatchAtlasImage (study.id, we, he, ie, e);
    ASSERT_TRUE (core.PatchAtlasImage (wa, ha, ia, status, a)) << a;
    EXPECT_EQ (status == rs::StudyReadStatus::Ok, patchImage) << what << ": " << e;
    if (patchImage)
        EXPECT_TRUE (we == wa && he == ha && SameBits (ie, ia)) << what << " patch image";

    std::vector<AtlasTile> te, ta;
    std::vector<FaceLayout> le, la;
    double spacing = 0, daylight = 0;
    bool converged = false;
    uint64_t generation = 0;
    const bool display = store.DisplayData (study.id, te, le, we, he, spacing, ie, daylight, converged, generation, e);
    ASSERT_TRUE (core.DisplayData (ta, la, wa, ha, ia, status, a)) << a;
    EXPECT_EQ (status == rs::StudyReadStatus::Ok, display) << what << ": " << e;
    if (display) {
        EXPECT_TRUE (we == wa && he == ha && SameBits (ie, ia)) << what << " display image";
        ExpectSameTiles (te, ta, what);
        ExpectSameLayouts (le, la, what);
    }

    StepMaskAtlas me, ma;
    std::vector<uint16_t> minutesE, minutesA;
    uint32_t noonE = 0, noonA = 0;
    const bool masks = store.StepMasks (study.id, me, minutesE, noonE, e);
    ASSERT_TRUE (core.StepMasks (ma, minutesA, noonA, status, a)) << a;
    EXPECT_EQ (status == rs::StudyReadStatus::Ok, masks) << what << ": " << e;
    if (masks) {
        EXPECT_TRUE (me.width == ma.width && me.height == ma.height && me.words == ma.words && me.steps == ma.steps)
            << what << " mask shape";
        EXPECT_TRUE (SameBits (me.masks, ma.masks)) << what << " masks";
        EXPECT_EQ (minutesA, minutesE) << what;
        EXPECT_EQ (noonA, noonE) << what;
    }

    TapiocaStudyInfo info {};
    ASSERT_TRUE (core.Info (info, a)) << a;
    size_t samples = 0;
    double area = 0;
    ASSERT_TRUE (store.Footprint (study.id, samples, area, e)) << e;
    EXPECT_EQ (info.sampleCount, samples) << what;
    EXPECT_TRUE (SameBits (info.analysedArea, area)) << what << " area";
    size_t faces = 0;
    uint64_t revision = 0, snapshotId = 0;
    const bool described = store.DisplayInfo (study.id, we, he, faces, daylight, revision, snapshotId, e);
    EXPECT_EQ (info.displayStatus == 0, described) << what;
    if (described)
        EXPECT_TRUE (info.displaySize[0] == we && info.displaySize[1] == he && info.displayFaces == faces) << what;
    EXPECT_TRUE (SameBits (info.daylightHours, daylight)) << what;
}

// The hover reading at face centroids and jittered points, every face.
void ExpectSameReadings (const Study& study, Lcg& lcg, const std::string& what)
{
    std::vector<double> corners; // per global face: three xyz corners
    for (const geomsrv::Mesh& mesh : study.snapshot->meshes)
        for (const uint32_t index : mesh.triangles)
            corners.insert (corners.end (), mesh.vertices.begin () + index * 3, mesh.vertices.begin () + index * 3 + 3);
    const size_t faceCount = corners.size () / 9;
    size_t measured = 0;
    for (size_t face = 0; face < faceCount + 2; ++face) {
        for (int k = 0; k < 4; ++k) {
            double w0 = k == 0 ? 1.0 / 3 : lcg.Uniform (0, 1), w1 = k == 0 ? 1.0 / 3 : lcg.Uniform (0, 1 - w0);
            const double w2 = 1 - w0 - w1;
            double point[3] = { lcg.Uniform (-30, 30), lcg.Uniform (-30, 30), lcg.Uniform (0, 9) };
            if (face < faceCount)
                for (int axis = 0; axis < 3; ++axis)
                    point[axis] = w0 * corners[face * 9 + axis] + w1 * corners[face * 9 + 3 + axis] +
                                  w2 * corners[face * 9 + 6 + axis];
            SunStudyReading e, a;
            uint8_t role = 0;
            double daylight = 0;
            std::string error;
            ASSERT_TRUE (
                SunStudyStore::Get ().ReadAt (study.id, study.snapshot->id, face, 0, point, e, role, daylight, error))
                << error;
            ASSERT_TRUE (study.core->ReadAt (face, point, a, error)) << error;
            ASSERT_TRUE (a.measured == e.measured && SameBits (a.hours, e.hours) && a.sample == e.sample &&
                         a.column == e.column && a.row == e.row && a.nearest == e.nearest)
                << what << " face " << face << " point " << k;
            measured += e.measured ? 1 : 0;
        }
    }
    if (study.domain != Domain::Ground)
        EXPECT_GT (measured, faceCount) << what << ": the corpus must read real samples";

    // Points exactly on cell boundaries of horizontal faces: where the shader's
    // float-rounded lattice and an exact one pick different cells.
    for (size_t face = 0; face < faceCount; ++face) {
        const double* c = &corners[face * 9];
        if (c[2] != c[5] || c[2] != c[8])
            continue;
        const double minX = std::min ({ c[0], c[3], c[6] }), maxX = std::max ({ c[0], c[3], c[6] });
        const double minY = std::min ({ c[1], c[4], c[7] }), maxY = std::max ({ c[1], c[4], c[7] });
        for (double x = std::ceil (minX / study.spacing) * study.spacing; x <= maxX; x += study.spacing) {
            for (double y = std::ceil (minY / study.spacing) * study.spacing; y <= maxY; y += study.spacing) {
                const double point[3] = { x, y, c[2] };
                SunStudyReading e, a;
                uint8_t role = 0;
                double daylight = 0;
                std::string error;
                ASSERT_TRUE (SunStudyStore::Get ().ReadAt (study.id, study.snapshot->id, face, 0, point, e, role,
                                                           daylight, error))
                    << error;
                ASSERT_TRUE (study.core->ReadAt (face, point, a, error)) << error;
                ASSERT_TRUE (a.measured == e.measured && SameBits (a.hours, e.hours) && a.sample == e.sample &&
                             a.column == e.column && a.row == e.row && a.nearest == e.nearest)
                    << what << " face " << face << " boundary " << x << ", " << y;
            }
        }
    }
}

// Advance both sides in slices of `slice` steps, comparing after each.
void AdvanceInLockstep (const Study& study, size_t slice, const std::string& what)
{
    for (int round = 0; round < 200; ++round) {
        StudyProgress progress;
        std::string e, a;
        ASSERT_TRUE (SunStudyStore::Get ().Progress (study.id, progress, e)) << e;
        if (progress.converged || progress.empty)
            return;
        size_t advancedE = 0, advancedA = 0;
        ASSERT_TRUE (SunStudyStore::Get ().Advance (study.id, slice, 1, 0.001, 0.0, advancedE, e)) << e;
        ASSERT_TRUE (study.core->Advance (*study.traversal, slice, 0.001, 0.0, 1, {}, {}, advancedA, a)) << a;
        ASSERT_EQ (advancedA, advancedE) << what;
        ExpectSameReads (study, what + " slice " + std::to_string (round));
        if (::testing::Test::HasFatalFailure () || advancedE == 0)
            return;
    }
}

struct StoreCleanup {
    StoreCleanup ()
    {
        SunStudyStore::Get ().Clear ();
    }
    ~StoreCleanup ()
    {
        SunStudyStore::Get ().Clear ();
    }
};

std::string Name (Domain domain)
{
    return domain == Domain::Ground ? "ground" : domain == Domain::Triangle ? "triangle" : "patch";
}

} // namespace

TEST (SunStudyRustStudyParity, ColdStudiesMatchEveryReaderAtEverySlice)
{
    StoreCleanup cleanup;
    Lcg lcg (0x5e7d1ull);
    for (const Domain domain : { Domain::Ground, Domain::Triangle, Domain::Patch }) {
        for (const size_t steps : { size_t { 1 }, size_t { 9 }, size_t { 37 } }) {
            const std::string what = Name (domain) + " steps=" + std::to_string (steps);
            const double spacing = steps == 9 ? 0.7 : 1.5;
            const Study study = Start (Scene (lcg, -1, nullptr), domain, steps, nullptr, spacing);
            ASSERT_NE (study.core, nullptr);
            ExpectSameReads (study, what + " fresh");
            AdvanceInLockstep (study, steps > 5 ? 4 : 1, what);
            ExpectSameReadings (study, lcg, what);
            if (::testing::Test::HasFatalFailure ())
                return;
        }
    }
}

TEST (SunStudyRustStudyParity, RerunsKeepTheSameAtlasesAndReuseTheSameSamples)
{
    StoreCleanup cleanup;
    Lcg lcg (0x5e7d2ull);
    size_t reused = 0;
    for (const Domain domain : { Domain::Triangle, Domain::Patch }) {
        for (int edit = 0; edit < 4; ++edit) {
            const std::string what = Name (domain) + " edit=" + std::to_string (edit);
            const auto base = Scene (lcg, -1, nullptr);
            Study source = Start (base, domain, 13, nullptr);
            AdvanceInLockstep (source, 13, what + " source");
            source.record = SunStudyStore::Get ().CompletedRecord (source.id);
            ASSERT_NE (source.record, nullptr) << what;
            const Study rerun = Start (Scene (lcg, edit, base.get ()), domain, 13, &source);
            ASSERT_NE (rerun.core, nullptr);
            TapiocaStudyInfo info {};
            std::string error;
            ASSERT_TRUE (rerun.core->Info (info, error)) << error;
            reused += info.reuse[0];
            ExpectSameReads (rerun, what + " seeded");
            AdvanceInLockstep (rerun, 5, what);
            ExpectSameReadings (rerun, lcg, what);
            if (::testing::Test::HasFatalFailure ())
                return;
        }
    }
    EXPECT_GT (reused, 100u) << "the corpus must actually reuse something";
}

TEST (SunStudyRustStudyParity, ADifferentSpacingStartsAfreshOnBothSides)
{
    StoreCleanup cleanup;
    Lcg lcg (0x5e7d3ull);
    for (const Domain domain : { Domain::Triangle, Domain::Patch }) {
        const auto base = Scene (lcg, -1, nullptr);
        Study source = Start (base, domain, 3, nullptr, 1.5);
        AdvanceInLockstep (source, 3, Name (domain) + " source");
        source.record = SunStudyStore::Get ().CompletedRecord (source.id);
        ASSERT_NE (source.record, nullptr);
        const Study finer = Start (base, domain, 3, &source, 1.0);
        ASSERT_NE (finer.core, nullptr);
        ExpectSameReads (finer, Name (domain) + " finer");
    }
}

TEST (SunStudyRustStudyParity, RefusalsMatchTheStore)
{
    std::string error;
    const SunSeries day = Day (3);
    EXPECT_EQ (rs::RustStudyCore::Ground (day, 1.0, { 0, 0 }, {}, error), nullptr);
    EXPECT_FALSE (error.empty ());
    EXPECT_EQ (rs::RustStudyCore::Ground (day, 1.0, { 0, 0, 0 }, { 0, 0 }, error), nullptr);

    // An atlas that cannot be packed is refused, as the start path refuses it.
    Lcg lcg (0x5e7d4ull);
    const auto scene = Scene (lcg, -1, nullptr);
    SurfaceSamplingOptions options;
    options.domain = SamplingDomain::SurfacePatch;
    options.spacing = 0.25;
    const auto roles = ResolveElementRoles (*scene, {}, {}, {});
    const auto sampling = BuildSurfaceSampling (*scene, roles.SampleMask (), options);
    SunStudyPatchAtlas atlas;
    atlas.Fit (sampling.patches, 1, 32);
    EXPECT_EQ (atlas.Width (), 0u);
    error.clear ();
    EXPECT_EQ (rs::RustStudyCore::Patch (day, 0.25, sampling.patches, nullptr, 32, error), nullptr);
    EXPECT_NE (error.find ("patch atlas"), std::string::npos) << error;
}

TEST (SunStudyRustStudyParity, ConcurrentAdvancesOnOneCoreNeverDoubleClaimAStep)
{
    // Once shared, a core may be advanced and read from several threads: an
    // advance that loses the race resolves nothing, reads wait for the slice,
    // and the converged result is the store's to the bit.
    StoreCleanup cleanup;
    Lcg lcg (0x5e7d5ull);
    const Study study = Start (Scene (lcg, -1, nullptr), Domain::Patch, 24, nullptr);
    ASSERT_NE (study.core, nullptr);
    std::atomic<size_t> resolved { 0 };
    std::atomic<bool> failed { false };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back ([&] {
            for (int round = 0; round < 64; ++round) {
                size_t advanced = 0;
                std::string error;
                if (!study.core->Advance (*study.traversal, 1, 0.001, 0.0, 1, {}, {}, advanced, error))
                    failed = true;
                resolved += advanced;
                std::vector<double> hours;
                if (!study.core->SunHours (hours, error))
                    failed = true;
            }
        });
    }
    for (auto& thread : threads)
        thread.join ();
    EXPECT_FALSE (failed.load ());
    StudyProgress progress;
    std::string error;
    ASSERT_TRUE (study.core->Progress (progress, error)) << error;
    if (!progress.converged) { // every thread may have lost some races
        size_t advanced = 0;
        ASSERT_TRUE (study.core->Advance (*study.traversal, 64, 0.001, 0.0, 1, {}, {}, advanced, error)) << error;
        resolved += advanced;
    }
    EXPECT_EQ (resolved.load (), 24u);
    size_t advanced = 0;
    ASSERT_TRUE (SunStudyStore::Get ().Advance (study.id, 64, 1, 0.001, 0.0, advanced, error)) << error;
    ExpectSameReads (study, "concurrent");
}
