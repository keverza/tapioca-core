#include "SunStudy/SunPathStudyData.hpp"
#include "SunStudy/SunPathPeriodPublication.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "sunstudy_core_fixture.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::sunstudyhud;

namespace {
std::string InsertDay (bool complete)
{
    auto record = std::make_unique<evp::sunstudy::StudyRecord> ();
    record->year = 2028;
    record->month = 2;
    record->day = 29;
    record->hourFrom = 10;
    record->hourTo = 11;
    record->timestepMinutes = 15;
    record->positions = { 0.0, 0.0, 0.0 };
    record->normals = { 0.0, 0.0, 1.0 };
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 1;
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->traversal =
        std::make_shared<evp::sunstudy::CpuTraversal> (std::make_shared<geomsrv::QueryEngine> (snapshot));
    std::vector<evp::sunstudy::SunStep> steps (2);
    steps[0].time = { 10, 0 };
    steps[1].time = { 10, 15 };
    for (auto& step : steps) {
        step.direction[2] = 1.0;
        step.altitudeDegrees = 90.0;
    }
    record->series = evp::sunstudy::SunSeries::FromSteps (steps, 15);
    evp::sunstudy::StudyInputs inputs;
    inputs.geometryVersion = 1;
    inputs.sunVersion = record->series.Version ();
    inputs.gridVersion = 1;
    evp::sunstudy::fixture::AttachTestCore (*record, inputs);
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    const auto id = store.Insert (std::move (record));
    if (complete) {
        size_t advanced = 0;
        std::string error;
        EXPECT_TRUE (store.Advance (id, 2, 1, 0.001, 0.0, advanced, error)) << error;
    }
    return id;
}
} // namespace

TEST (SunPathStudyData, CompletedDayCopiesExactStepsMetadataAndReusesItsRevision)
{
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    const auto id = InsertDay (true);
    SunPathStudyDay day;
    ASSERT_TRUE (ReadSunPathStudyDay (id, day));
    EXPECT_EQ (day.studyId, id);
    EXPECT_EQ (day.revision, store.Revision (id));
    EXPECT_EQ (day.year, 2028);
    EXPECT_EQ (day.month, 2);
    EXPECT_EQ (day.day, 29);
    EXPECT_EQ (day.hourFrom, 10);
    EXPECT_EQ (day.hourTo, 11);
    EXPECT_EQ (day.timestepMinutes, 15);
    ASSERT_EQ (day.steps.size (), 2u);
    EXPECT_EQ (day.steps[1].time.minute, 15);
    EXPECT_DOUBLE_EQ (day.steps[1].direction[2], 1.0);
    const auto* original = day.steps.data ();
    ASSERT_TRUE (ReadSunPathStudyDay (id, day));
    EXPECT_EQ (day.steps.data (), original); // no per-frame deep copy
    store.Erase (id);
    EXPECT_FALSE (ReadSunPathStudyDay (id, day));
    EXPECT_FALSE (day.available);
    EXPECT_TRUE (day.steps.empty ());
}

TEST (SunPathStudyData, UnfinishedAndMissingStudiesInvalidateCachedDay)
{
    auto& store = evp::sunstudy::SunStudyStore::Get ();
    const auto finished = InsertDay (true), pending = InsertDay (false);
    SunPathStudyDay day;
    ASSERT_TRUE (ReadSunPathStudyDay (finished, day));
    EXPECT_FALSE (ReadSunPathStudyDay (pending, day));
    EXPECT_FALSE (day.available);
    EXPECT_EQ (day.revision, 0u);
    EXPECT_FALSE (ReadSunPathStudyDay ("missing", day));
    store.Erase (finished);
    store.Erase (pending);
}

TEST (SunPathStudyData, AppliedFramesShareGeometryAndOldEnvelopesRemainImmutable)
{
    SunPathPeriodPublication publication;
    auto geometry = std::make_shared<SunPathPeriodGeometry> ();
    geometry->studyId = "period-a";
    geometry->revision = 17;
    geometry->year = 2028;
    geometry->days = { { 2, 29, 1.0, 0.25, {} } };
    publication.Retain (geometry);
    const auto pending = publication.Latest ();
    EXPECT_EQ (pending->applied.frame, 0);
    ASSERT_TRUE (publication.Apply (geometry, { 1, 0, 0, 3 }));
    const auto shown = publication.Latest ();
    EXPECT_NE (shown, pending);
    EXPECT_EQ (shown->geometry, pending->geometry);
    EXPECT_EQ (shown->applied.step, 3);
    EXPECT_EQ (pending->applied.frame, 0); // requested/applied updates never mutate readers
    publication.Reset ();                  // same reset used by accepted Submit/cache release/run cancellation
    EXPECT_EQ (publication.Latest (), nullptr);
    EXPECT_FALSE (publication.Apply (geometry, { 1, 0, 0, 4 })); // retired worker cannot republish
    auto replacement = std::make_shared<SunPathPeriodGeometry> ();
    replacement->revision = 18;
    publication.Retain (replacement);
    EXPECT_FALSE (publication.Apply (geometry, { 1, 0, 0, 4 }));
    EXPECT_EQ (publication.Latest ()->geometry, replacement);
    EXPECT_EQ (shown->geometry->revision, 17u); // owning handles survive retirement safely
}

TEST (SunPathStudyData, ArchiveOffsetsAreMappedByCalendarAndClockRatherThanAssumedEqual)
{
    SunPathPeriodGeometry geometry;
    evp::sunstudy::SunStep early, late;
    early.time = { 10, 0 };
    late.time = { 11, 0 };
    geometry.days = { { 2, 28, 1.0, 1.0, {} }, { 2, 29, 1.0, 1.0, { late, early } } };
    const auto frame = SunPathAppliedFrameFor (geometry, 1, 0, 2, 29, 600);
    EXPECT_EQ (frame.day, 1);
    EXPECT_EQ (frame.step, 1);
    EXPECT_EQ (SunPathAppliedFrameFor (geometry, 1, 0, 2, 29, 630).step, -1);
    EXPECT_EQ (SunPathAppliedFrameFor (geometry, 1, 0, 3, 1, 600).day, -1);
    EXPECT_EQ (SunPathAppliedFrameFor (geometry, 0, 2, 2, 29, 600).day, -1);
}
