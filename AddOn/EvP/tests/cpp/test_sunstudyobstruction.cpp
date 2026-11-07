#include "SunStudy/SunStudyObstruction.hpp"

#include "MeshFixtures.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

using namespace evp::sunstudy;

namespace {

SunStep Noon ()
{
    SunStep sun;
    sun.direction[2] = 1.0;
    sun.altitudeDegrees = 90.0;
    return sun;
}

TEST (SunStudyObstruction, CellsDistinguishExactBlockersClearAndBackFacing)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 41;
    snapshot->meshes = { evptest::MakeBox ("analysis", 4, 0, 5), evptest::MakeBox ("context", 0, 0, 5),
                         evptest::MakeBox ("ignored", 8, 0, 5) };
    StudyRecord record;
    record.snapshot = snapshot;
    record.snapshotId = snapshot->id;
    record.occluders =
        BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, { "context" }, { "ignored" }));
    record.positions = { 0.25, 0.25, -1.0, 4.25, 0.25, -1.0, 8.25, 0.25, -1.0, 100.0, 100.0, 0.0, 0.25, 0.25, -1.0 };
    record.normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, -1 };

    SunObstructionResult result;
    ASSERT_TRUE (AttributeSunObstruction (record, { 0, 1, 2, 3, 4 }, Noon (), result)) << result.error;
    ASSERT_EQ (result.cells.size (), 5u);
    EXPECT_EQ (result.cells[0].state, SunObstructionState::Blocked);
    EXPECT_EQ (result.cells[0].blockerGuid, "context");
    EXPECT_TRUE (result.cells[0].context);
    EXPECT_EQ (result.cells[0].sourceTriangle, 1u); // bottom face, not a global/mesh index guess
    EXPECT_NEAR (result.cells[0].distance, 6.0, 1.0e-9);
    EXPECT_EQ (result.cells[1].state, SunObstructionState::Blocked);
    EXPECT_EQ (result.cells[1].blockerGuid, "analysis");
    EXPECT_FALSE (result.cells[1].context);
    EXPECT_EQ (result.cells[2].state, SunObstructionState::Lit); // ignored is absent from both engines
    EXPECT_EQ (result.cells[3].state, SunObstructionState::Lit);
    EXPECT_EQ (result.cells[4].state, SunObstructionState::BackFacing);
    EXPECT_TRUE (result.cells[4].blockerGuid.empty ());
}

TEST (SunStudyObstruction, NearerContextWinsAndMeshPrefixBecomesALocalTriangle)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 42;
    snapshot->meshes = { evptest::MakeBox ("first-analysis", 8, 0, 5), evptest::MakeBox ("second-analysis", 4, 0, 5),
                         evptest::MakeBox ("near-context", 4, 0, 2) };
    StudyRecord record;
    record.occluders = BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, { "near-context" }, {}));
    record.positions = { 4.25, 0.25, 0.0 };
    record.normals = { 0, 0, 1 };
    SunObstructionResult result;
    ASSERT_TRUE (AttributeSunObstruction (record, { 0 }, Noon (), result)) << result.error;
    ASSERT_EQ (result.cells.size (), 1u);
    EXPECT_EQ (result.cells[0].blockerGuid, "near-context");
    EXPECT_NEAR (result.cells[0].distance, 2.0, 1.0e-9);

    for (size_t vertex = 0; vertex < snapshot->meshes[2].vertices.size (); vertex += 3)
        snapshot->meshes[2].vertices[vertex] += 100.0;
    record.occluders = BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, {}, {}));
    ASSERT_TRUE (AttributeSunObstruction (record, { 0 }, Noon (), result)) << result.error;
    EXPECT_EQ (result.cells[0].blockerGuid, "second-analysis");
    EXPECT_EQ (result.cells[0].sourceTriangle, 1u); // global triangle 13 -> local triangle 1
    EXPECT_FALSE (result.cells[0].context);
}

TEST (SunStudyObstruction, WholeDayKeepsEachTimedHitAndDropsCancelledPartialResults)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 43;
    snapshot->meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    StudyRecord record;
    record.occluders = BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, {}, {}));
    record.positions = { 0.25, 0.25, 0.0 }; // null normal: a ground sample faces every direction
    std::vector<SunStep> steps (2, Noon ());
    steps[0].time = TimeOfDay { 8, 0 };
    steps[1].time = TimeOfDay { 9, 0 };
    steps[1].direction[2] = -1.0;
    record.series = SunSeries::FromSteps (steps, 60);

    SunObstructionDayResult result;
    ASSERT_TRUE (AttributeSunObstructionDay (record, { 0 }, result)) << result.error;
    ASSERT_EQ (result.steps.size (), 2u);
    EXPECT_EQ (result.steps[0].minute, 480);
    ASSERT_EQ (result.steps[0].cells.size (), 1u);
    EXPECT_EQ (result.steps[0].cells[0].state, SunObstructionState::Blocked);
    EXPECT_EQ (result.steps[0].cells[0].blockerGuid, "roof");
    EXPECT_EQ (result.steps[1].minute, 540);
    EXPECT_EQ (result.steps[1].cells[0].state, SunObstructionState::Lit);

    int calls = 0;
    EXPECT_FALSE (AttributeSunObstructionDay (record, { 0 }, result, [&] { return ++calls > 1; }));
    EXPECT_TRUE (result.steps.empty ());
    EXPECT_TRUE (result.error.empty ());
}

TEST (SunStudyObstruction, CustomBoundsAndInvalidSamplesAreNotInvented)
{
    StudyRecord record;
    record.defaultRayBounds = false;
    record.positions = { 0, 0, 0 };
    record.normals = { 0, 0, 1 };
    SunObstructionResult result;
    EXPECT_FALSE (AttributeSunObstruction (record, { 0 }, Noon (), result));
    EXPECT_NE (result.error.find ("custom ray bounds"), std::string::npos);
    EXPECT_TRUE (result.cells.empty ());

    record.defaultRayBounds = true;
    ASSERT_TRUE (AttributeSunObstruction (record, { 0, 9 }, Noon (), result)) << result.error;
    ASSERT_EQ (result.cells.size (), 2u);
    EXPECT_EQ (result.cells[0].state, SunObstructionState::Lit);
    EXPECT_EQ (result.cells[1].state, SunObstructionState::Unavailable);
}

} // namespace
