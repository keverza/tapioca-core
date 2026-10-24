// The P5 flip: a study started with the Rust core attached must answer every
// SunStudyStore reader -- and the display packet -- exactly as the same study
// on the C++ core. Two otherwise identical records are filed side by side,
// advanced through the store's own Advance, and compared bit for bit after
// every slice, cold and through reuse reruns. The selection flag, the
// atlas-mismatch fallback and the mixed-core reuse rule are pinned too.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyCoreSelection.hpp"
#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace evp::sunstudy;

template <typename T> bool SameBits (const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size () == b.size () && (a.empty () || std::memcmp (a.data (), b.data (), a.size () * sizeof (T)) == 0);
}

bool SameBits (double a, double b)
{
    return std::memcmp (&a, &b, sizeof a) == 0;
}

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
// functions it calls. `rust` selects the core; `previous` is the reuse source.
std::string Start (std::shared_ptr<geomsrv::Snapshot> snapshot, Domain domain, bool rust,
                   std::shared_ptr<const StudyRecord> previous, bool& attached)
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
    attached = rust && AttachRustStudyCore (*record, *snapshot, previous.get (), 8192, error);
    EXPECT_EQ (attached, rust) << error;
    SyncStudy (*record, { snapshot->id, record->series.Version (), gridVersion });
    record->snapshot = snapshot;
    SetSampleMeshes (*record);
    if (previous != nullptr) {
        const std::atomic<bool> notCancelled { false };
        ReuseUnaffectedSamples (*previous, *record, notCancelled);
    }
    return SunStudyStore::Get ().Insert (std::move (record));
}

// The store names the study in its refusals; compare them with the ids aligned.
std::string Named (std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find ("'" + from + "'");
    if (at != std::string::npos)
        text.replace (at + 1, from.size (), to);
    return text;
}

void ExpectSameStore (const std::string& cpp, const std::string& rust, const std::string& what)
{
    auto& store = SunStudyStore::Get ();
    std::string e, a;
    StudyProgress pe, pa;
    ASSERT_TRUE (store.Progress (cpp, pe, e) && store.Progress (rust, pa, a)) << e << a;
    EXPECT_TRUE (pa.resolvedSteps == pe.resolvedSteps && pa.totalSteps == pe.totalSteps &&
                 pa.sampleCount == pe.sampleCount && pa.converged == pe.converged && pa.empty == pe.empty)
        << what << " progress";
    std::vector<double> he, ha, xe, xa, ne, na;
    std::vector<uint8_t> be, ba;
    ASSERT_TRUE (store.Results (cpp, he, xe, ne, &be, e) && store.Results (rust, ha, xa, na, &ba, a)) << e << a;
    EXPECT_TRUE (SameBits (he, ha) && SameBits (be, ba) && SameBits (xe, xa) && SameBits (ne, na)) << what;
    SunStudyResultSummary se, sa;
    ASSERT_TRUE (store.Summary (cpp, se, e) && store.Summary (rust, sa, a)) << e << a;
    EXPECT_TRUE (sa.count == se.count && sa.fullyLit == se.fullyLit && sa.fullyShaded == se.fullyShaded &&
                 SameBits (sa.meanHours, se.meanHours) && SameBits (sa.maxHours, se.maxHours))
        << what << " summary";

    uint32_t w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    std::vector<float> i1, i2;
    std::string e1, e2;
    EXPECT_EQ (store.AtlasImage (rust, w2, h2, i2, e2), store.AtlasImage (cpp, w1, h1, i1, e1)) << what;
    EXPECT_EQ (Named (e2, rust, cpp), e1) << what;
    EXPECT_TRUE (w1 == w2 && h1 == h2 && SameBits (i1, i2)) << what << " atlas image";
    e1.clear ();
    e2.clear ();
    EXPECT_EQ (store.PatchAtlasImage (rust, w2, h2, i2, e2), store.PatchAtlasImage (cpp, w1, h1, i1, e1)) << what;
    EXPECT_EQ (Named (e2, rust, cpp), e1) << what;
    EXPECT_TRUE (SameBits (i1, i2)) << what << " patch image";

    std::vector<AtlasTile> t1, t2;
    std::vector<FaceLayout> l1, l2;
    double s1 = 0, s2 = 0, d1 = 0, d2 = 0;
    bool c1 = false, c2 = false;
    uint64_t g1 = 0, g2 = 0;
    e1.clear ();
    e2.clear ();
    const bool display = store.DisplayData (cpp, t1, l1, w1, h1, s1, i1, d1, c1, g1, e1);
    EXPECT_EQ (store.DisplayData (rust, t2, l2, w2, h2, s2, i2, d2, c2, g2, e2), display) << what;
    EXPECT_EQ (Named (e2, rust, cpp), e1) << what;
    if (display) {
        EXPECT_TRUE (SameBits (i1, i2) && t1.size () == t2.size () && l1.size () == l2.size () && c1 == c2)
            << what << " display";
        for (size_t f = 0; f < t1.size () && f < t2.size (); ++f)
            ASSERT_TRUE (t1[f].x == t2[f].x && t1[f].y == t2[f].y && t1[f].width == t2[f].width &&
                         std::memcmp (l1[f].origin, l2[f].origin, sizeof l1[f].origin) == 0)
                << what << " face " << f;
    }

    StepMaskAtlas m1, m2;
    std::vector<uint16_t> n1, n2;
    uint32_t q1 = 0, q2 = 0;
    e1.clear ();
    e2.clear ();
    EXPECT_EQ (store.StepMasks (rust, m2, n2, q2, e2), store.StepMasks (cpp, m1, n1, q1, e1)) << what;
    EXPECT_EQ (Named (e2, rust, cpp), e1) << what;
    EXPECT_TRUE (SameBits (m1.masks, m2.masks) && n1 == n2 && q1 == q2) << what << " masks";

    // The display packet the overlay draws.
    std::shared_ptr<const StudyRecord> r1, r2;
    store.ReadDisplayRecord (
        cpp, store.Revision (cpp),
        [&] (const StudyRecord& r) {
            std::string error;
            const auto p1 = PrepareStudyDisplayData (r, error);
            store.ReadDisplayRecord (
                rust, store.Revision (rust),
                [&] (const StudyRecord& q) {
                    const auto p2 = PrepareStudyDisplayData (q, error);
                    ASSERT_EQ (p1 == nullptr, p2 == nullptr) << what << ": " << error;
                    if (p1 != nullptr) {
                        EXPECT_TRUE (p1->width == p2->width && p1->height == p2->height &&
                                     SameBits (*p1->image, *p2->image) && SameBits (*p1->stepMasks, *p2->stepMasks) &&
                                     p1->stepMinutes == p2->stepMinutes && p1->noonStep == p2->noonStep &&
                                     SameBits (p1->area, p2->area) && p1->patchTiles.size () == p2->patchTiles.size ())
                            << what << " display packet";
                    }
                },
                error);
        },
        e);
}

void AdvanceBoth (const std::string& cpp, const std::string& rust, const std::string& what)
{
    for (int slice = 0; slice < 20; ++slice) {
        size_t a = 0, b = 0;
        std::string e;
        ASSERT_TRUE (SunStudyStore::Get ().Advance (cpp, 4, 1, 0.001, 0.0, a, e)) << e;
        ASSERT_TRUE (SunStudyStore::Get ().Advance (rust, 4, 1, 0.001, 0.0, b, e)) << e;
        ASSERT_EQ (a, b) << what;
        ExpectSameStore (cpp, rust, what + " slice " + std::to_string (slice));
        if (a == 0 || ::testing::Test::HasFatalFailure ())
            return;
    }
}

struct Cleanup {
    Cleanup ()
    {
        SunStudyStore::Get ().Clear ();
    }
    ~Cleanup ()
    {
        SunStudyStore::Get ().Clear ();
        OverrideSunStudyCore (std::nullopt);
    }
};

} // namespace

TEST (SunStudyRustFlip, SelectionDefaultsToRustAndCanBeOverridden)
{
    Cleanup cleanup;
    const SunStudyCoreKind available = RustSunStudyCoreAvailable () ? SunStudyCoreKind::Rust : SunStudyCoreKind::Cpp;
    const char* value = std::getenv ("TAPIOCA_SUNSTUDY_CORE");
    if (value == nullptr)
        EXPECT_EQ (SelectedSunStudyCore (), available) << "the default is the Rust core (P6)";
    else if (std::string (value) == "cpp")
        EXPECT_EQ (SelectedSunStudyCore (), SunStudyCoreKind::Cpp) << "the escape hatch";
    OverrideSunStudyCore (SunStudyCoreKind::Rust);
    EXPECT_EQ (SelectedSunStudyCore (), available);
    OverrideSunStudyCore (SunStudyCoreKind::Cpp);
    EXPECT_EQ (SelectedSunStudyCore (), SunStudyCoreKind::Cpp);
    OverrideSunStudyCore (std::nullopt);
}

TEST (SunStudyRustFlip, EveryStoreReaderAndTheDisplayPacketMatchTheCppCore)
{
    Cleanup cleanup;
    for (const Domain domain : { Domain::Ground, Domain::Triangle, Domain::Patch }) {
        const std::string what = domain == Domain::Ground     ? "ground"
                                 : domain == Domain::Triangle ? "triangle"
                                                              : "patch";
        bool attached = false;
        const auto scene = Scene (1, 2);
        const std::string cpp = Start (scene, domain, false, nullptr, attached);
        const std::string rust = Start (scene, domain, true, nullptr, attached);
        ASSERT_TRUE (attached);
        ExpectSameStore (cpp, rust, what + " fresh");
        AdvanceBoth (cpp, rust, what);
        if (::testing::Test::HasFatalFailure ())
            return;

        // A rerun after an edit reuses the unaffected samples on both cores.
        const auto e = SunStudyStore::Get ().CompletedRecord (cpp), a = SunStudyStore::Get ().CompletedRecord (rust);
        ASSERT_TRUE (e != nullptr && a != nullptr) << what;
        const auto edited = Scene (2, 3.5);
        const std::string cpp2 = Start (edited, domain, false, e, attached);
        const std::string rust2 = Start (edited, domain, true, a, attached);
        StudyRecord me, ma;
        std::string error;
        ASSERT_TRUE (SunStudyStore::Get ().Describe (cpp2, me, error) &&
                     SunStudyStore::Get ().Describe (rust2, ma, error));
        EXPECT_EQ (ma.reusedSamples, me.reusedSamples) << what;
        EXPECT_EQ (ma.reusedSampleSteps, me.reusedSampleSteps) << what;
        if (domain != Domain::Ground)
            EXPECT_GT (me.reusedSamples, 0u) << what << ": the rerun must reuse something";
        ExpectSameStore (cpp2, rust2, what + " seeded");
        AdvanceBoth (cpp2, rust2, what + " rerun");
    }
}

TEST (SunStudyRustFlip, ARustStudyNeverSeedsFromACppSourceAndAMismatchStaysOnCpp)
{
    Cleanup cleanup;
    bool attached = false;
    const auto scene = Scene (1, 2);
    const std::string cpp = Start (scene, Domain::Patch, false, nullptr, attached);
    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Advance (cpp, 64, 1, 0.001, 0.0, advanced, error)) << error;
    const auto source = SunStudyStore::Get ().CompletedRecord (cpp);
    ASSERT_NE (source, nullptr);

    // Rust target, C++ source: traced afresh, never seeded from bits it cannot read.
    const std::string rust = Start (Scene (2, 2), Domain::Patch, true, source, attached);
    StudyRecord metadata;
    ASSERT_TRUE (SunStudyStore::Get ().Describe (rust, metadata, error));
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
}
