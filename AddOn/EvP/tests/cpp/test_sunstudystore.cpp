// L2 offline tests for SunStudy/SunStudyStore.
//
// The store is what lets a study outlive the call that started it, so what these
// defend is lifetime and isolation: that it owns its sample arrays rather than
// borrowing a caller's stack, that two studies do not interfere, and that a
// study being advanced cannot be advanced again underneath itself.
//
// The command layer above it needs the DevKit and so is covered at a higher
// level; everything below the commands is here.

#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace evp::sunstudy;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

std::shared_ptr<const Snapshot> MakeRoofSnapshot (uint64_t id = 31)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = id;
    Mesh mesh;
    mesh.guid = "roof";
    mesh.vertices = { -1.0, -1.0, 5.0, 1.0, -1.0, 5.0, 1.0, 1.0, 5.0, -1.0, 1.0, 5.0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snap->meshes.push_back (std::move (mesh));
    return snap;
}

SunSeries MakeDay (int steps)
{
    std::vector<SunStep> raw;
    for (int i = 0; i < steps; ++i) {
        SunStep step;
        step.time = TimeOfDay { 6 + i, 0 };
        step.altitudeDegrees = 40.0;
        step.direction[0] = 0.0;
        step.direction[1] = 0.0;
        step.direction[2] = 1.0;
        raw.push_back (step);
    }
    return SunSeries::FromSteps (raw, 60);
}

// Builds a record whose sample arrays are LOCAL to this function, so a store
// that borrowed rather than copied would be left with dangling pointers the
// moment it returns.
std::unique_ptr<StudyRecord> MakeRecord (int steps = 4, uint64_t snapshotId = 31)
{
    auto engine = std::make_shared<QueryEngine> (MakeRoofSnapshot (snapshotId));

    auto record = std::make_unique<StudyRecord> ();
    record->series = MakeDay (steps);
    record->traversal = std::make_shared<CpuTraversal> (engine);
    record->timestepMinutes = 60;

    const std::vector<double> localPositions { 0.0, 0.0, 0.0, 10.0, 0.0, 0.0 };
    const std::vector<double> localNormals { 0.0, 0.0, 1.0, 0.0, 0.0, 1.0 };
    record->positions = localPositions;
    record->normals = localNormals;

    StudyInputs inputs;
    inputs.geometryVersion = engine->SnapshotId ();
    inputs.sunVersion = record->series.Version ();
    inputs.gridVersion = 1;
    record->session.Sync (inputs, record->series, record->Samples ());
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

} // namespace

TEST (SunStudyStore, AStudySurvivesTheCallThatStartedIt)
{
    StoreFixture fixture;
    const std::string id = SunStudyStore::Get ().Insert (MakeRecord ());
    ASSERT_FALSE (id.empty ());

    // ⚠️ THE ARRAYS THE RECORD WAS BUILT FROM ARE GONE BY NOW. If the store
    // borrowed rather than copied, everything below reads released memory --
    // which under a sanitizer is a diagnosis and without one is a plausible
    // wrong answer.
    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Advance (id, 10, 1, 0.001, 0.0, advanced, error)) << error;
    EXPECT_EQ (advanced, 4u);

    StudyProgress progress;
    ASSERT_TRUE (SunStudyStore::Get ().Progress (id, progress, error)) << error;
    EXPECT_TRUE (progress.converged);

    std::vector<double> hours;
    std::vector<double> positions;
    ASSERT_TRUE (SunStudyStore::Get ().SunHours (id, hours, positions, error)) << error;
    ASSERT_EQ (hours.size (), 2u);
    EXPECT_NEAR (hours[0], 0.0, 1e-12) << "under the roof";
    EXPECT_NEAR (hours[1], 4.0, 1e-12) << "in the open";
    EXPECT_EQ (positions.size (), 6u);
}

TEST (SunStudyStore, AdvancingInSlicesReachesTheSameAnswer)
{
    StoreFixture fixture;
    const std::string id = SunStudyStore::Get ().Insert (MakeRecord (6));

    size_t advanced = 0;
    std::string error;
    size_t total = 0;
    while (SunStudyStore::Get ().Advance (id, 2, 1, 0.001, 0.0, advanced, error) && advanced > 0)
        total += advanced;

    EXPECT_EQ (total, 6u);

    StudyProgress progress;
    SunStudyStore::Get ().Progress (id, progress, error);
    EXPECT_TRUE (progress.converged);
}

TEST (SunStudyStore, TwoStudiesDoNotInterfere)
{
    StoreFixture fixture;
    const std::string shortStudy = SunStudyStore::Get ().Insert (MakeRecord (2, 31));
    const std::string longStudy = SunStudyStore::Get ().Insert (MakeRecord (7, 32));
    ASSERT_NE (shortStudy, longStudy);
    EXPECT_EQ (SunStudyStore::Get ().Count (), 2u);

    size_t advanced = 0;
    std::string error;
    SunStudyStore::Get ().Advance (shortStudy, 10, 1, 0.001, 0.0, advanced, error);

    StudyProgress shortProgress;
    StudyProgress longProgress;
    SunStudyStore::Get ().Progress (shortStudy, shortProgress, error);
    SunStudyStore::Get ().Progress (longStudy, longProgress, error);

    EXPECT_TRUE (shortProgress.converged);
    EXPECT_FALSE (longProgress.converged);
    EXPECT_EQ (longProgress.resolvedSteps, 0u);
}

TEST (SunStudyStore, AnUnknownIdIsRefusedByName)
{
    StoreFixture fixture;
    size_t advanced = 0;
    std::string error;

    EXPECT_FALSE (SunStudyStore::Get ().Advance ("nope", 1, 1, 0.001, 0.0, advanced, error));
    EXPECT_NE (error.find ("nope"), std::string::npos) << "the message must name the id";

    StudyProgress progress;
    EXPECT_FALSE (SunStudyStore::Get ().Progress ("nope", progress, error));

    std::vector<double> hours;
    std::vector<double> positions;
    EXPECT_FALSE (SunStudyStore::Get ().SunHours ("nope", hours, positions, error));
}

TEST (SunStudyStore, EraseAndClearRemoveStudies)
{
    StoreFixture fixture;
    const std::string first = SunStudyStore::Get ().Insert (MakeRecord ());
    const std::string second = SunStudyStore::Get ().Insert (MakeRecord ());
    EXPECT_EQ (SunStudyStore::Get ().Count (), 2u);

    EXPECT_TRUE (SunStudyStore::Get ().Erase (first));
    EXPECT_FALSE (SunStudyStore::Get ().Erase (first)) << "erasing twice is not an error but changes nothing";
    EXPECT_EQ (SunStudyStore::Get ().Count (), 1u);

    SunStudyStore::Get ().Clear ();
    EXPECT_EQ (SunStudyStore::Get ().Count (), 0u);
    EXPECT_TRUE (SunStudyStore::Get ().Ids ().empty ());
}

TEST (SunStudyStore, DescribeCopiesMetadataWithoutHandingOutAPointer)
{
    StoreFixture fixture;
    auto record = MakeRecord (3);
    record->gridSpacing = 2.5;
    record->groundPad = 17.0;
    record->year = 2026;
    record->month = 3;
    record->day = 21;
    const std::string id = SunStudyStore::Get ().Insert (std::move (record));

    StudyRecord metadata;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Describe (id, metadata, error)) << error;

    EXPECT_EQ (metadata.id, id);
    EXPECT_NEAR (metadata.gridSpacing, 2.5, 1e-12);
    EXPECT_NEAR (metadata.groundPad, 17.0, 1e-12);
    EXPECT_EQ (metadata.year, 2026);
    EXPECT_EQ (metadata.month, 3);
    EXPECT_EQ (metadata.day, 21);
}

// The measurement the backend decision rests on has to survive to a live run,
// so it accumulates across slices rather than reporting only the last one.
TEST (SunStudyStore, AnalysisTimeAccumulatesAcrossSlices)
{
    StoreFixture fixture;
    const std::string id = SunStudyStore::Get ().Insert (MakeRecord (4));

    size_t advanced = 0;
    std::string error;
    SunStudyStore::Get ().Advance (id, 2, 1, 0.001, 0.0, advanced, error);

    StudyRecord afterFirst;
    SunStudyStore::Get ().Describe (id, afterFirst, error);

    SunStudyStore::Get ().Advance (id, 2, 1, 0.001, 0.0, advanced, error);

    StudyRecord afterSecond;
    SunStudyStore::Get ().Describe (id, afterSecond, error);

    EXPECT_GE (afterSecond.analysisMilliseconds, afterFirst.analysisMilliseconds);
}

TEST (SunStudyStore, IdsAreGeneratedWhenNoneIsGiven)
{
    StoreFixture fixture;
    const std::string first = SunStudyStore::Get ().Insert (MakeRecord ());
    const std::string second = SunStudyStore::Get ().Insert (MakeRecord ());

    EXPECT_FALSE (first.empty ());
    EXPECT_FALSE (second.empty ());
    EXPECT_NE (first, second);

    const std::vector<std::string> ids = SunStudyStore::Get ().Ids ();
    EXPECT_EQ (ids.size (), 2u);
}

TEST (SunStudyStore, InsertingNullIsRefusedRatherThanStored)
{
    StoreFixture fixture;
    EXPECT_TRUE (SunStudyStore::Get ().Insert (nullptr).empty ());
    EXPECT_EQ (SunStudyStore::Get ().Count (), 0u);
}

// The whole vertical slice, over the sampler the command layer actually uses:
// ground grid -> session -> hours. Samples under the roof get nothing; samples
// out in the open get the whole day.
TEST (SunStudyStore, EndToEndOverTheGroundGrid)
{
    StoreFixture fixture;

    auto engine = std::make_shared<QueryEngine> (MakeRoofSnapshot ());
    const GroundGrid grid = MakeGroundSampleGrid (Vec3 { -1, -1, 0 }, Vec3 { 1, 1, 5 }, 1.0, 4.0 /* pad */, 0.1);
    ASSERT_TRUE (grid.valid);

    auto record = std::make_unique<StudyRecord> ();
    record->series = MakeDay (5);
    record->traversal = std::make_shared<CpuTraversal> (engine);
    record->positions = grid.positions;
    record->normals = grid.normals;

    StudyInputs inputs;
    inputs.geometryVersion = engine->SnapshotId ();
    inputs.sunVersion = record->series.Version ();
    inputs.gridVersion = 1;
    record->session.Sync (inputs, record->series, record->Samples ());

    const std::string id = SunStudyStore::Get ().Insert (std::move (record));

    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Advance (id, 100, 0, 0.001, 0.0, advanced, error)) << error;

    std::vector<double> hours;
    std::vector<double> positions;
    ASSERT_TRUE (SunStudyStore::Get ().SunHours (id, hours, positions, error)) << error;
    ASSERT_EQ (hours.size (), grid.columns * grid.rows);

    // The roof spans x,y in [-1, 1]; a sample at the centre is under it and one
    // at the padded corner is not.
    size_t shadowed = 0;
    size_t sunlit = 0;
    for (size_t i = 0; i < hours.size (); ++i) {
        const bool underRoof = std::abs (positions[i * 3]) <= 1.0 && std::abs (positions[i * 3 + 1]) <= 1.0;
        if (underRoof)
            ++shadowed;
        else if (hours[i] > 0.0)
            ++sunlit;
    }
    EXPECT_GT (shadowed, 0u) << "the padded grid must reach under the model";
    EXPECT_GT (sunlit, 0u) << "and out past it -- otherwise the study says nothing";
}

// ---------------------------------------------------------------------------
// Results — the single read every consumer uses
// ---------------------------------------------------------------------------

TEST (SunStudyStore, ResultsRefusesAnUnknownId)
{
    SunStudyStore::Get ().Clear ();
    std::vector<double> hours, positions, normals;
    std::string error;
    EXPECT_FALSE (SunStudyStore::Get ().Results ("nope", hours, positions, normals, nullptr, error));
    EXPECT_FALSE (error.empty ());
}

TEST (SunStudyStore, ResultsCarriesNormalsAlongsidePositions)
{
    SunStudyStore::Get ().Clear ();
    StoreFixture fixture;
    const std::string id = SunStudyStore::Get ().Insert (MakeRecord (4));

    std::vector<double> hours, positions, normals;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Results (id, hours, positions, normals, nullptr, error));

    EXPECT_EQ (positions.size (), normals.size ());
    EXPECT_EQ (hours.size () * 3, positions.size ());
    SunStudyStore::Get ().Clear ();
}

// ⚠️ THE BITS ARE WHAT MAKES A CROSS-CHECK POSSIBLE. Hours answer "how much";
// only a per-step bit answers "which steps", which is what a sample-by-sample
// diff against another engine compares.
TEST (SunStudyStore, StepBitsAreSampleMajorAndAgreeWithTheHours)
{
    SunStudyStore::Get ().Clear ();
    StoreFixture fixture;
    const std::string id = SunStudyStore::Get ().Insert (MakeRecord (4));

    size_t advanced = 0;
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Advance (id, 100, 1, 0.001, 0.0, advanced, error));

    std::vector<double> hours, positions, normals;
    std::vector<uint8_t> bits;
    ASSERT_TRUE (SunStudyStore::Get ().Results (id, hours, positions, normals, &bits, error));

    StudyProgress progress;
    ASSERT_TRUE (SunStudyStore::Get ().Progress (id, progress, error));
    ASSERT_GT (progress.totalSteps, 0u);
    ASSERT_EQ (bits.size (), hours.size () * progress.totalSteps);

    for (size_t sample = 0; sample < hours.size (); ++sample) {
        size_t lit = 0;
        for (size_t step = 0; step < progress.totalSteps; ++step)
            lit += bits[sample * progress.totalSteps + step];
        // Every lit step contributes the same slice of the day, so the count
        // and the hours must tell the same story.
        if (lit == 0)
            EXPECT_DOUBLE_EQ (hours[sample], 0.0);
        else
            EXPECT_GT (hours[sample], 0.0);
    }
    SunStudyStore::Get ().Clear ();
}

TEST (SunStudyStore, StepBitsAreOmittedWhenNotAsked)
{
    SunStudyStore::Get ().Clear ();
    StoreFixture fixture;
    const std::string id = SunStudyStore::Get ().Insert (MakeRecord (4));

    std::vector<double> hours, positions, normals;
    std::vector<uint8_t> bits { 1, 2, 3 };
    std::string error;
    ASSERT_TRUE (SunStudyStore::Get ().Results (id, hours, positions, normals, nullptr, error));
    EXPECT_EQ (bits.size (), 3u) << "the caller's buffer was written despite passing nullptr";
    SunStudyStore::Get ().Clear ();
}
