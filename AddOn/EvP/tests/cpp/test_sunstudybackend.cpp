// L2 offline tests for SunStudy/SunStudyBackend (#54).
//
// The native HUD's CPU/GPU choice crosses four hops before a ray is traced:
// the HUD radio, `StartSunStudy`'s `backend`, `StudyRecord::backend`, and the
// follower's replacement, which re-issues the adopted study's value through an
// AUTOMATIC producer -- the one producer whose default is the GPU. What these
// defend is that an explicit choice survives every hop, and that what ran is
// reported beside what was asked.
//
// The command and HUD layers need the DevKit and ImGui; each hop's decision is
// the pure function here, and the record hop is the real store.

#include "SunStudy/SunStudyBackend.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace evp::sunstudy;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

std::shared_ptr<const QueryEngine> MakeRoof ()
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 54;
    Mesh mesh;
    mesh.guid = "roof";
    mesh.vertices = { -1.0, -1.0, 5.0, 1.0, -1.0, 5.0, 1.0, 1.0, 5.0, -1.0, 1.0, 5.0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snap->meshes.push_back (std::move (mesh));
    return std::make_shared<QueryEngine> (snap);
}

// A GPU traversal's three observable states, without a device.
class ReportingTraversal final : public ITraversal {
  public:
    explicit ReportingTraversal (TraversalBackendState state) : state_ (std::move (state))
    {
    }
    void OccludeDirectional (const double*, size_t count, const double*, double, double, uint8_t* out,
                             size_t) const override
    {
        std::fill (out, out + count, uint8_t (0));
    }
    void OccludeRays (const OcclusionRay*, size_t count, uint8_t* out, size_t) const override
    {
        std::fill (out, out + count, uint8_t (0));
    }
    uint64_t SceneVersion () const override
    {
        return 54;
    }
    TraversalBackendState ResolvedBackend () const override
    {
        return state_;
    }

  private:
    TraversalBackendState state_;
};

std::unique_ptr<StudyRecord> MakeRecord (const std::string& backend, std::shared_ptr<const ITraversal> traversal)
{
    auto record = std::make_unique<StudyRecord> ();
    record->backend = backend;
    record->traversal = std::move (traversal);
    record->timestepMinutes = 60;
    record->positions = { 0.0, 0.0, 0.0 };
    record->normals = { 0.0, 0.0, 1.0 };
    return record;
}

struct StoreFixture {
    StoreFixture ()
    {
        SunStudyStore::Get ().Clear ();
    }
    ~StoreFixture ()
    {
        SunStudyStore::Get ().Clear ();
    }
};

// One hop of the chain: StartSunStudy given `requested`, by a producer.
std::string Start (const std::string& requested, bool automatic)
{
    std::string backend, error;
    EXPECT_TRUE (ResolveStartBackend (&requested, automatic, backend, error)) << error;
    return backend;
}

} // namespace

// HUD radio -> StartSunStudy, for both values. The manual run is not automatic.
TEST (SunStudyBackend, HudChoiceMapsToTheStartParameterItSends)
{
    EXPECT_STREQ (SunStudyBackendForChoice (0), "cpu");
    EXPECT_STREQ (SunStudyBackendForChoice (1), "gpu");
    EXPECT_EQ (Start (SunStudyBackendForChoice (0), false), "cpu");
    EXPECT_EQ (Start (SunStudyBackendForChoice (1), false), "gpu");
}

TEST (SunStudyBackend, AbsentBackendTakesTheProducerDefault)
{
    std::string backend, error;
    ASSERT_TRUE (ResolveStartBackend (nullptr, false, backend, error));
    EXPECT_EQ (backend, "cpu");
    ASSERT_TRUE (ResolveStartBackend (nullptr, true, backend, error));
    EXPECT_EQ (backend, "gpu");
}

// ⚠️ THE CASE THE ISSUE EXISTS FOR. The follower's replacement is automatic,
// and automatic defaults to the GPU; an explicit CPU must not be upgraded.
TEST (SunStudyBackend, ExplicitChoiceIsNeverReplacedByTheAutomaticDefault)
{
    EXPECT_EQ (Start ("cpu", true), "cpu");
    EXPECT_EQ (Start ("gpu", true), "gpu");
}

TEST (SunStudyBackend, UnknownBackendIsRefusedNotDefaulted)
{
    for (const std::string requested : { "", "GPU", "optix" }) {
        std::string backend, error;
        EXPECT_FALSE (ResolveStartBackend (&requested, true, backend, error)) << requested;
        EXPECT_EQ (error, "backend must be cpu or gpu");
    }
}

// Record -> ShowSunStudy's adopted config -> the replacement after an edit ->
// its record, and again: every hop the follower takes, for both values.
TEST (SunStudyBackend, FollowerReplacementKeepsTheAdoptedBackendAcrossEdits)
{
    StoreFixture fixture;
    auto& store = SunStudyStore::Get ();
    const auto roof = MakeRoof ();
    for (const std::string chosen : { "cpu", "gpu" }) {
        std::string id = store.Insert (MakeRecord (Start (chosen, false), std::make_shared<CpuTraversal> (roof)));
        for (int edit = 0; edit < 3; ++edit) {
            StudyRecord adopted;
            std::string error;
            ASSERT_TRUE (store.Describe (id, adopted, error)) << error;
            ASSERT_EQ (adopted.backend, chosen) << "edit " << edit;
            // ActiveSunStudyConfig::backend = metadata.backend; the driver sends it.
            const std::string replacement = Start (adopted.backend, true);
            id = store.Insert (MakeRecord (replacement, std::make_shared<CpuTraversal> (roof)));
        }
        StudyRecord last;
        std::string error;
        ASSERT_TRUE (store.Describe (id, last, error)) << error;
        EXPECT_EQ (last.backend, chosen);
    }
}

TEST (SunStudyBackend, CpuStudyReportsTheCpuTracer)
{
    StoreFixture fixture;
    const std::string id =
        SunStudyStore::Get ().Insert (MakeRecord ("cpu", std::make_shared<CpuTraversal> (MakeRoof ())));
    SunStudyBackendReport report;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Backend (id, report, error)) << error;
    EXPECT_EQ (report.requested, "cpu");
    EXPECT_EQ (report.resolved, "cpu");
    EXPECT_TRUE (report.fallbackReason.empty ());
    EXPECT_EQ (DescribeSunStudyBackend (report), "CPU");
}

// A GPU that disabled keeps the record's `gpu` -- what was asked -- and
// reports the `sun-gpu-fallback` reason beside it.
TEST (SunStudyBackend, GpuFallbackIsReportedNotRewritten)
{
    StoreFixture fixture;
    TraversalBackendState fellBack;
    fellBack.fallbackReason = "hardware FP64 D3D11 compute unavailable; no precision downgrade";
    const std::string id =
        SunStudyStore::Get ().Insert (MakeRecord ("gpu", std::make_shared<ReportingTraversal> (fellBack)));
    SunStudyBackendReport report;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Backend (id, report, error)) << error;
    EXPECT_EQ (report.requested, "gpu");
    EXPECT_EQ (report.resolved, "cpu");
    EXPECT_EQ (report.fallbackReason, fellBack.fallbackReason);
    EXPECT_EQ (DescribeSunStudyBackend (report),
               "GPU unavailable, ran on CPU: hardware FP64 D3D11 compute unavailable; no precision downgrade");

    StudyRecord metadata;
    ASSERT_TRUE (SunStudyStore::Get ().Describe (id, metadata, error)) << error;
    EXPECT_EQ (metadata.backend, "gpu");
}

TEST (SunStudyBackend, GpuStatesDescribeWhatRan)
{
    TraversalBackendState engaged;
    engaged.resolved = "gpu";
    TraversalBackendState untried;
    untried.resolved.clear ();
    const ReportingTraversal engagedGpu (engaged), untriedGpu (untried);
    EXPECT_EQ (DescribeSunStudyBackend (ReportSunStudyBackend ("gpu", &engagedGpu)), "GPU");
    EXPECT_EQ (DescribeSunStudyBackend (ReportSunStudyBackend ("gpu", &untriedGpu)), "GPU requested, not engaged yet");
    EXPECT_EQ (ReportSunStudyBackend ("cpu", nullptr).resolved, "cpu");
}

TEST (SunStudyBackend, UnknownStudyIsAnError)
{
    StoreFixture fixture;
    SunStudyBackendReport report;
    std::string error;
    EXPECT_FALSE (SunStudyStore::Get ().Backend ("missing", report, error));
    EXPECT_NE (error.find ("missing"), std::string::npos);
}
