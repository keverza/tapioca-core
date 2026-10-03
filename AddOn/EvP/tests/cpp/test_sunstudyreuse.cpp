#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyWinding.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "ArchViz/ElementPacket.hpp"
#include "ArchViz/ScenePacketTrace.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>

using namespace evp::sunstudy;

namespace {
geomsrv::Mesh Floor (const std::string& guid, double x)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = { x, 0, 0, x + 12, 0, 0, x + 12, 8, 0, x, 8, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    mesh.triMaterial = { 0, 0 };
    return mesh;
}

geomsrv::Mesh Wall (double x)
{
    geomsrv::Mesh mesh;
    mesh.guid = "wall";
    mesh.vertices = { x, 0, 0, x, 8, 0, x, 8, 4, x, 0, 4 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    mesh.triMaterial = { 0, 0 };
    return mesh;
}

std::shared_ptr<geomsrv::Snapshot> Scene (double wallX = 6)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 1;
    snapshot->meshes = { Floor ("near", 0), Wall (wallX), Floor ("far", 70) };
    return snapshot;
}

SunSeries Day (size_t count = 4)
{
    std::vector<SunStep> steps;
    for (size_t i = 0; i < count; ++i) {
        SunStep step;
        step.time = { 8 + static_cast<int> (i / 60), static_cast<int> (i % 60) };
        const double angle = 0.5 + (i % 4) * 0.1;
        step.direction[0] = std::cos (angle);
        step.direction[2] = std::sin (angle);
        step.altitudeDegrees = angle * 180.0 / 3.141592653589793;
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, 1);
}

class CountTraversal : public ITraversal {
  public:
    explicit CountTraversal (std::shared_ptr<const geomsrv::Snapshot> snapshot)
        : cpu_ (std::make_shared<const geomsrv::QueryEngine> (snapshot))
    {
    }
    void OccludeDirectional (const double* origins, size_t count, const double* direction, double tmin, double tmax,
                             uint8_t* out, size_t parallel) const override
    {
        rays += count;
        cpu_.OccludeDirectional (origins, count, direction, tmin, tmax, out, parallel);
    }
    void OccludeRays (const OcclusionRay* raysIn, size_t count, uint8_t* out, size_t parallel) const override
    {
        cpu_.OccludeRays (raysIn, count, out, parallel);
    }
    uint64_t SceneVersion () const override
    {
        return cpu_.SceneVersion ();
    }
    mutable size_t rays = 0;

  private:
    CpuTraversal cpu_;
};

std::unique_ptr<StudyRecord> Prepared (std::shared_ptr<const geomsrv::Snapshot> snapshot, bool patch, size_t steps = 4,
                                       const std::vector<std::string>& context = {},
                                       const std::vector<std::string>& ignored = {},
                                       const StudyRecord* previous = nullptr, bool meshSampling = false,
                                       const std::vector<std::vector<uint8_t>>& faceMasks = {})
{
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->series = Day (steps);
    record->gridSpacing = 0.75;
    const auto roles = ResolveElementRoles (*snapshot, {}, context, ignored);
    const auto subset = OccluderSnapshot (*snapshot, roles);
    record->traversal = std::make_shared<CountTraversal> (subset != nullptr ? subset : snapshot);
    for (const auto role : roles.roles)
        record->elementRoles.push_back (static_cast<uint8_t> (role));
    const auto sampleMask = roles.SampleMask ();
    if (meshSampling) {
        SurfaceSamplingOptions options;
        options.domain = patch ? SamplingDomain::SurfacePatch : SamplingDomain::TriangleLegacy;
        options.spacing = record->gridSpacing;
        auto sampling = BuildSurfaceSampling (*snapshot, sampleMask, options, previous, {}, faceMasks);
        record->domain = options.domain;
        record->samplingLayout = std::move (sampling.layout);
        record->sampleGrid = std::move (sampling.triangles);
        record->patchGrid = std::move (sampling.patches);
        record->positions = patch ? record->patchGrid.positions : record->sampleGrid.positions;
        record->normals = patch ? record->patchGrid.normals : record->sampleGrid.normals;
        SetSampleMeshes (*record);
        record->session.Sync ({ snapshot->id, record->series.Version (), 1 }, record->series, record->Samples ());
        return record;
    }
    std::vector<double> vertices;
    std::vector<uint32_t> triangles, groups;
    std::vector<std::string> elements;
    std::vector<uint8_t> faces;
    for (size_t m = 0; m < snapshot->meshes.size (); ++m) {
        const auto& mesh = snapshot->meshes[m];
        const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
        for (auto index : mesh.triangles)
            triangles.push_back (base + index);
        groups.resize (triangles.size () / 3, static_cast<uint32_t> (m));
        elements.push_back (mesh.guid);
        if (!faceMasks.empty ()) {
            if (faceMasks[m].empty ())
                faces.insert (faces.end (), mesh.TriangleCount (), 1);
            else
                faces.insert (faces.end (), faceMasks[m].begin (), faceMasks[m].end ());
        }
    }
    if (patch) {
        record->domain = SamplingDomain::SurfacePatch;
        PatchSamplerOptions options;
        options.spacing = record->gridSpacing;
        options.normalOffset = 0.01;
        options.sampleGroup = &sampleMask;
        options.sampleFace = faceMasks.empty () ? nullptr : &faces;
        record->patchGrid = BuildPatchSampleGrid (vertices.data (), vertices.size () / 3, triangles.data (),
                                                  triangles.size () / 3, groups.data (), elements, options);
        record->positions = record->patchGrid.positions;
        record->normals = record->patchGrid.normals;
    }
    else {
        SamplerOptions options;
        options.spacing = record->gridSpacing;
        options.normalOffset = 0.01;
        options.sampleGroup = &sampleMask;
        options.sampleFace = faceMasks.empty () ? nullptr : &faces;
        record->sampleGrid = BuildSampleGrid (vertices.data (), vertices.size () / 3, triangles.data (),
                                              triangles.size () / 3, groups.data (), options);
        record->positions = record->sampleGrid.positions;
        record->normals = record->sampleGrid.normals;
    }
    SetSampleMeshes (*record);
    record->session.Sync ({ snapshot->id, record->series.Version (), 1 }, record->series, record->Samples ());
    return record;
}

void Complete (StudyRecord& record)
{
    record.session.Advance (*record.traversal, 1000, 0.001, 0.0, 1);
    ASSERT_TRUE (record.session.Progress ().converged);
}

class SunStudyReuse : public testing::TestWithParam<bool> {};
} // namespace

TEST_P (SunStudyReuse, MoveAddDeleteAndReorderMatchFullRecomputationWithFewerRays)
{
    auto source = Prepared (Scene (), GetParam (), 70); // multiple bitset words
    Complete (*source);
    const std::atomic<bool> cancelled { false };
    for (int edit = 0; edit < 5; ++edit) {
        auto next = Scene (edit == 0 ? 9 : 6);
        next->id = 2;
        if (edit == 1)
            next->meshes.erase (next->meshes.begin () + 1);
        if (edit == 2) {
            auto added = Wall (3);
            added.guid = "new";
            next->meshes.push_back (added);
        }
        if (edit == 3)
            std::reverse (next->meshes.begin (), next->meshes.end ());
        if (edit == 4)
            next->meshes[0].vertices[0] += 0.00001; // exact, not quantised identity
        auto incremental = Prepared (next, GetParam (), 70);
        auto oracle = Prepared (next, GetParam (), 70);
        const size_t reused = ReuseUnaffectedSamples (*source, *incremental, cancelled);
        ASSERT_GT (reused, 0u) << edit;
        if (edit != 3)
            EXPECT_LT (reused, incremental->positions.size () / 3) << edit;
        Complete (*incremental);
        Complete (*oracle);
        EXPECT_EQ (incremental->session.Accumulator ().Bits (), oracle->session.Accumulator ().Bits ()) << edit;
        EXPECT_EQ (incremental->session.SunHours (), oracle->session.SunHours ()) << edit;
        const auto* traced = dynamic_cast<const CountTraversal*> (incremental->traversal.get ());
        const auto* full = dynamic_cast<const CountTraversal*> (oracle->traversal.get ());
        EXPECT_LT (traced->rays, full->rays) << edit;
        EXPECT_DOUBLE_EQ (incremental->gridSpacing, source->gridSpacing);
    }
}

TEST_P (SunStudyReuse, RemovingContextOrIgnoredRestoresSamplesWithoutAGeometryEdit)
{
    const auto scene = Scene ();
    const std::atomic<bool> cancelled { false };
    for (bool ignored : { false, true }) {
        const std::vector<std::string> exclude { "near" };
        auto source = Prepared (scene, GetParam (), 4, ignored ? std::vector<std::string> {} : exclude,
                                ignored ? exclude : std::vector<std::string> {});
        Complete (*source);
        auto restored = Prepared (scene, GetParam ());
        auto oracle = Prepared (scene, GetParam ());
        EXPECT_EQ (source->snapshotId, restored->snapshotId);
        ASSERT_GT (restored->positions.size (), source->positions.size ());
        ReuseUnaffectedSamples (*source, *restored, cancelled);
        size_t restoredSamples = 0;
        for (const auto mesh : restored->sampleMeshes)
            restoredSamples += mesh == 0 ? 1 : 0;
        ASSERT_GT (restoredSamples, 0u);
        EXPECT_LE (restored->reusedSamples, restored->positions.size () / 3 - restoredSamples);
        Complete (*restored);
        Complete (*oracle);
        EXPECT_EQ (restored->session.Accumulator ().Bits (), oracle->session.Accumulator ().Bits ());
        EXPECT_EQ (restored->session.SunHours (), oracle->session.SunHours ());
    }
}

TEST_P (SunStudyReuse, CachedGridsAndSelectiveResultsMatchFreshCpuForEditsAndRoles)
{
    auto source = Prepared (Scene (), GetParam (), 70, {}, {}, nullptr, true);
    Complete (*source);
    const std::atomic<bool> cancelled { false };
    for (int edit = 0; edit < 7; ++edit) {
        SCOPED_TRACE (edit);
        auto next = Scene (edit == 0 ? 9 : 6);
        next->id = 2;
        if (edit == 1)
            next->meshes.erase (next->meshes.begin () + 1);
        if (edit == 2) {
            auto added = Wall (3);
            added.guid = "new";
            next->meshes.insert (next->meshes.begin (), added);
        }
        if (edit == 3)
            std::reverse (next->meshes.begin (), next->meshes.end ());
        if (edit == 4)
            next->meshes[0].triMaterial = { 12, 13 };
        const std::vector<std::string> context =
            edit == 5 ? std::vector<std::string> { "near" } : std::vector<std::string> {};
        const std::vector<std::string> ignored =
            edit == 6 ? std::vector<std::string> { "wall" } : std::vector<std::string> {};
        auto incremental = Prepared (next, GetParam (), 70, context, ignored, source.get (), true);
        auto oracle = Prepared (next, GetParam (), 70, context, ignored);
        ASSERT_EQ (incremental->positions, oracle->positions);
        ASSERT_EQ (incremental->normals, oracle->normals);
        ReuseUnaffectedSamples (*source, *incremental, cancelled);
        Complete (*incremental);
        Complete (*oracle);
        EXPECT_EQ (incremental->session.Accumulator ().Bits (), oracle->session.Accumulator ().Bits ());
        EXPECT_EQ (incremental->session.SunHours (), oracle->session.SunHours ());
        EXPECT_GT (incremental->reusedSamples, 0u);
        if (edit == 4) {
            EXPECT_EQ (incremental->session.Accumulator ().ActiveSampleCount (), 0u);
            const auto* traced = dynamic_cast<const CountTraversal*> (incremental->traversal.get ());
            ASSERT_NE (traced, nullptr);
            EXPECT_EQ (traced->rays, 0u);
            const auto hours = incremental->session.SunHours ();
            EXPECT_GT (std::count (hours.begin (), hours.end (), 0.0), 0);
        }
    }
}

TEST_P (SunStudyReuse, SourceMustBeCompleteCompatibleUnambiguousAndNotCancelled)
{
    auto source = Prepared (Scene (), GetParam ());
    auto target = Prepared (Scene (9), GetParam ());
    std::atomic<bool> cancelled { false };
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), 0u);
    Complete (*source);
    target->gridSpacing *= 2;
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), 0u);
    target->gridSpacing = source->gridSpacing;
    cancelled = true;
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), 0u);
    cancelled = false;
    source->defaultRayBounds = false;
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), 0u);
    source->defaultRayBounds = true;
    target->series = Day (5);
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), 0u);
    target->series = source->series;
    auto ambiguous = Scene (9);
    ambiguous->meshes[2].guid = ambiguous->meshes[0].guid;
    target->snapshot = ambiguous;
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), 0u);
}

TEST_P (SunStudyReuse, GlassFaceMasksMatchFreshCpuAndUnmeasuredFacesStillCastShadows)
{
    std::vector<std::vector<uint8_t>> masks { { 1, 0 }, {}, {} };
    auto source = Prepared (Scene (), GetParam (), 70, {}, {}, nullptr, true, masks);
    Complete (*source);
    const std::atomic<bool> cancelled { false };
    for (int edit = 0; edit < 3; ++edit) {
        auto next = Scene (edit == 0 ? 9 : 6);
        next->id = 2;
        auto changed = masks;
        if (edit == 1)
            changed[0] = { 0, 1 }; // material/picked receiver change, same geometry
        if (edit == 2)
            changed[0].clear (); // manual early-model switch restores both faces
        auto incremental = Prepared (next, GetParam (), 70, {}, {}, source.get (), true, changed);
        auto oracle = Prepared (next, GetParam (), 70, {}, {}, nullptr, false, changed);
        ASSERT_EQ (incremental->positions, oracle->positions);
        ASSERT_EQ (incremental->normals, oracle->normals);
        ReuseUnaffectedSamples (*source, *incremental, cancelled);
        Complete (*incremental);
        Complete (*oracle);
        EXPECT_EQ (incremental->session.Accumulator ().Bits (), oracle->session.Accumulator ().Bits ());
        EXPECT_EQ (incremental->session.SunHours (), oracle->session.SunHours ());
        EXPECT_GT (incremental->reusedSamples, 0u);
    }
}

TEST_P (SunStudyReuse, OppositeSunsDoNotDirtyTheWholeSiteThroughTheUnionEnvelope)
{
    auto steps = Day ().Steps ();
    steps[1].direction[0] = -steps[1].direction[0];
    steps[3].direction[0] = -steps[3].direction[0];
    const auto sun = SunSeries::FromSteps (steps, 1);
    auto source = Prepared (Scene (), GetParam ());
    source->series = sun;
    source->session.Sync ({ 1, sun.Version (), 1 }, sun, source->Samples ());
    Complete (*source);
    auto target = Prepared (Scene (9), GetParam ());
    auto oracle = Prepared (Scene (9), GetParam ());
    for (auto* record : { target.get (), oracle.get () }) {
        record->series = sun;
        record->session.Sync ({ 2, sun.Version (), 1 }, sun, record->Samples ());
    }
    const std::atomic<bool> cancelled { false };
    const size_t reused = ReuseUnaffectedSamples (*source, *target, cancelled);
    const auto farSamples = std::count (target->sampleMeshes.begin (), target->sampleMeshes.end (), 2u);
    EXPECT_GE (reused, static_cast<size_t> (farSamples));
    Complete (*target);
    Complete (*oracle);
    EXPECT_EQ (target->session.Accumulator ().Bits (), oracle->session.Accumulator ().Bits ());
}

TEST_P (SunStudyReuse, MaterialOnlyEditNeedsNoRaysAndSeedPreservesNeverLitSamples)
{
    auto source = Prepared (Scene (), GetParam ());
    Complete (*source);
    const auto hours = source->session.SunHours ();
    ASSERT_GT (std::count (hours.begin (), hours.end (), 0.0), 0);
    auto next = Scene ();
    next->meshes[0].triMaterial = { 12, 13 };
    auto target = Prepared (next, GetParam ());
    const std::atomic<bool> cancelled { false };
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), target->positions.size () / 3);
    EXPECT_TRUE (target->session.Progress ().converged);
    EXPECT_EQ (target->session.Accumulator ().Bits (), source->session.Accumulator ().Bits ());
    EXPECT_EQ (target->session.Accumulator ().ActiveSampleCount (), 0u);
    Complete (*target);
    EXPECT_EQ (target->session.SunHours (), hours);
    const auto* traced = dynamic_cast<const CountTraversal*> (target->traversal.get ());
    ASSERT_NE (traced, nullptr);
    EXPECT_EQ (traced->rays, 0u);
}

TEST_P (SunStudyReuse, ChangedLastNormalKeepsOnlyThatSampleDirty)
{
    auto source = Prepared (Scene (), GetParam ());
    Complete (*source);
    auto target = Prepared (Scene (), GetParam ());
    auto oracle = Prepared (Scene (), GetParam ());
    const size_t count = target->positions.size () / 3;
    ASSERT_GT (count, 1u);
    ASSERT_GT (source->session.Accumulator ().LitStepCount (count - 1), 0u);
    // The last xyz normal's end is one-past the vector, never a valid subscript.
    for (auto* record : { target.get (), oracle.get () }) {
        for (size_t axis = 0; axis < 3; ++axis)
            record->normals[(count - 1) * 3 + axis] *= -1.0;
    }
    const std::atomic<bool> cancelled { false };
    EXPECT_EQ (ReuseUnaffectedSamples (*source, *target, cancelled), count - 1);
    EXPECT_EQ (target->session.Accumulator ().ActiveSampleCount (), 1u);
    EXPECT_FALSE (target->session.Progress ().converged);
    Complete (*target);
    Complete (*oracle);
    EXPECT_EQ (target->session.Accumulator ().Bits (), oracle->session.Accumulator ().Bits ());
    EXPECT_EQ (target->session.SunHours (), oracle->session.SunHours ());
    EXPECT_EQ (target->session.Accumulator ().LitStepCount (count - 1), 0u);
}

INSTANTIATE_TEST_SUITE_P (Domains, SunStudyReuse, testing::Values (false, true));

TEST (SunStudyReuseAccumulator, RejectsIncompleteBadIndexAndResetsSelectiveState)
{
    OcclusionAccumulator source (2, 1), target (2, 1);
    EXPECT_FALSE (target.SeedReusable (source, { 0, 1 }));
    auto record = Prepared (Scene (), false);
    Complete (*record);
    EXPECT_FALSE (target.SeedReusable (record->session.Accumulator (), { 0, 1 }));
    auto single = Prepared (Scene (), false, 1);
    Complete (*single);
    EXPECT_FALSE (target.SeedReusable (single->session.Accumulator (), { 0, 9999999 }));
    EXPECT_TRUE (target.SeedReusable (single->session.Accumulator (), { 0, OcclusionAccumulator::kNoReuse }));
    EXPECT_EQ (target.ActiveSampleCount (), 1u);
    target.Reset ();
    EXPECT_EQ (target.ActiveSampleCount (), 2u);
    EXPECT_EQ (target.ResolvedStepCount (), 0u);
}

TEST (SunStudyReuseStore, CompletedHandleSurvivesEraseAndFurtherAdvanceIsImmutable)
{
    auto& store = SunStudyStore::Get ();
    store.Clear ();
    auto record = Prepared (Scene (), false);
    const auto id = store.Insert (std::move (record));
    EXPECT_EQ (store.CompletedRecord (id), nullptr);
    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (store.Advance (id, 100, 1, 0.001, 0.0, advanced, error));
    const auto completed = store.CompletedRecord (id);
    ASSERT_NE (completed, nullptr);
    StudyRecord metadata;
    ASSERT_TRUE (store.Describe (id, metadata, error));
    EXPECT_EQ (metadata.snapshotId, completed->snapshotId);
    const auto bits = completed->session.Accumulator ().Bits ();
    ASSERT_TRUE (store.Advance (id, 100, 1, 0.001, 0.0, advanced, error));
    EXPECT_EQ (advanced, 0u);
    ASSERT_TRUE (store.Erase (id));
    EXPECT_EQ (completed->session.Accumulator ().Bits (), bits);
}

TEST (ScenePacketTelemetry, PayloadBytesExcludeUnusedCapacityAndQueueKeepsStamp)
{
    using namespace geomsrv::archviz;
    CapturedMeshPacket packet;
    packet.mesh = Floor ("packet", 0);
    packet.capturedAt = std::chrono::steady_clock::now ();
    auto upload = MakeElementPacket (packet);
    ASSERT_NE (upload, nullptr);
    EXPECT_GT (upload->packetId, 0u);
    const size_t payload = upload->PayloadBytes ();
    upload->vertices.reserve (1000);
    EXPECT_EQ (upload->PayloadBytes (), payload);
    EXPECT_GT (upload->Bytes (), payload);
    auto& queue = SceneCmdQueue::Get ();
    queue.Clear ();
    queue.SetConsumer (true);
    const auto before = std::chrono::steady_clock::now ();
    queue.PushUpsert (std::move (upload));
    auto commands = queue.Take (1);
    ASSERT_EQ (commands.size (), 1u);
    EXPECT_GE (commands[0].queuedAt, before);
    EXPECT_LE (commands[0].queuedAt, std::chrono::steady_clock::now ());
    EXPECT_EQ (commands[0].upload->capturedAt, packet.capturedAt);
    SunStudyAtlasUpload study;
    study.texels = std::make_shared<const std::vector<float>> (8, 0.0f);
    study.stepMasks = std::make_shared<const std::vector<uint32_t>> (16, 0u);
    EXPECT_EQ (SunStudyPayloadBytes (study), 8 * sizeof (float) + 16 * sizeof (uint32_t));
    queue.SetConsumer (false);
}
