#include "SunStudy/SunStudyObstruction.hpp"
#include "SunStudy/SunStudyLiftedPoint.hpp"

#include "MeshFixtures.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <gtest/gtest.h>

#include <cmath>

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

TEST (SunStudyObstruction, FootprintIsTheCellCastOntoTheBlockerWhateverItsTessellation)
{
    // One large roof slab over a 1 m cell: every sub-cell lands on the slab's
    // underside, and the footprint's area is the cell's, not a triangle's.
    for (const double slab : { 1.0, 20.0 }) {
        auto snapshot = std::make_shared<geomsrv::Snapshot> ();
        snapshot->id = 44;
        snapshot->meshes = { evptest::MakeBox ("roof", -slab * 0.5, -slab * 0.5, 5, slab, slab, 0.2) };
        StudyRecord record;
        record.snapshot = snapshot;
        record.snapshotId = snapshot->id;
        record.occluders = BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, {}, {}));
        record.positions = { 0.0, 0.0, 0.0 };
        record.normals = { 0, 0, 1 };
        record.gridSpacing = 1.0;
        SunObstructionResult result;
        ASSERT_TRUE (AttributeSunObstruction (record, { 0 }, Noon (), result, 2)) << result.error;
        ASSERT_EQ (result.cells.size (), 1u);
        const auto& cell = result.cells[0];
        EXPECT_EQ (cell.state, SunObstructionState::Blocked);
        EXPECT_NEAR (cell.hit[2], 5.0, 1.0e-6);
        ASSERT_EQ (cell.fragments.size (), 4u);
        double area = 0.0;
        for (const auto& fragment : cell.fragments) {
            for (const auto& corner : fragment.corners)
                EXPECT_NEAR (corner[2], 5.0, 1.0e-6);
            area += std::abs (fragment.corners[2][0] - fragment.corners[0][0]) *
                    std::abs (fragment.corners[2][1] - fragment.corners[0][1]);
        }
        EXPECT_NEAR (area, 1.0, 1.0e-6) << "slab " << slab;
    }
}

TEST (SunStudyObstruction, PartialFootprintOnALitCellAndNoneBehindTheSurface)
{
    // A narrow blocker over half the cell: the centre ray misses (the study's
    // answer stays Lit) but the footprint shows the shadowed half.
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 45;
    snapshot->meshes = { evptest::MakeBox ("strip", 0.1, -1.0, 3, 0.5, 2.0, 0.1) };
    StudyRecord record;
    record.snapshot = snapshot;
    record.snapshotId = snapshot->id;
    record.occluders = BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, {}, {}));
    record.positions = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    record.normals = { 0, 0, 1, 0, 0, -1 };
    record.gridSpacing = 1.0;
    SunObstructionResult result;
    ASSERT_TRUE (AttributeSunObstruction (record, { 0, 1 }, Noon (), result, 4)) << result.error;
    EXPECT_EQ (result.cells[0].state, SunObstructionState::Lit);
    EXPECT_EQ (result.cells[0].fragments.size (), 8u); // the two sub-cell columns under the strip
    EXPECT_EQ (result.cells[1].state, SunObstructionState::BackFacing);
    EXPECT_TRUE (result.cells[1].fragments.empty ());
}

TEST (SunStudyLiftedPoint, LiftIsClampedUnderAnOverhangAndTracedAsOneReceiver)
{
    // A floor slab at z 0..0.2 and a canopy whose underside is at z 2.0.
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 46;
    snapshot->meshes = { evptest::MakeBox ("floor", -5, -5, -0.2, 10, 10, 0.2),
                         evptest::MakeBox ("canopy", 0, -1, 2.0, 2, 2, 0.2) };
    StudyRecord record;
    record.snapshot = snapshot;
    record.snapshotId = snapshot->id;
    record.occluders = BuildSunStudyOccluders (*snapshot, ResolveElementRoles (*snapshot, {}, {}, {}));
    std::vector<SunStep> steps (2);
    steps[0].time = TimeOfDay { 9, 0 };
    steps[0].direction[2] = 1.0; // overhead: blocked under the canopy
    steps[0].altitudeDegrees = 90.0;
    steps[1].time = TimeOfDay { 10, 0 };
    steps[1].direction[0] = 1.0; // low from +x: passes under the canopy edge
    steps[1].direction[2] = 0.05;
    steps[1].altitudeDegrees = 3.0;
    record.series = SunSeries::FromSteps (steps, 60);
    record.timestepMinutes = 60;
    record.hourFrom = 8;
    record.hourTo = 18;

    // A dragged point lands on the first surface the ray meets, never inside it.
    const double eye[3] = { 1.0, 0.0, 10.0 }, down[3] = { 0.0, 0.0, -1.0 };
    double hit[3];
    ASSERT_TRUE (SunStudySceneHit (record, eye, down, hit));
    EXPECT_NEAR (hit[2], 2.2, 1.0e-6); // the canopy's top is what the ray meets first
    const double floor[3] = { 1.0, 0.0, 0.0 };

    LiftedPoint lifted;
    ASSERT_TRUE (LiftSunStudyPoint (record, floor, 1.8, lifted));
    EXPECT_FALSE (lifted.clamped);
    EXPECT_NEAR (lifted.position[2], 1.8, 1.0e-9);
    ASSERT_TRUE (LiftSunStudyPoint (record, floor, 3.0, lifted));
    EXPECT_TRUE (lifted.clamped); // never inside the canopy
    EXPECT_NEAR (lifted.height, 2.0 - kLiftedPointClearance, 1.0e-6);
    EXPECT_LT (lifted.position[2], 2.0);

    PointDay day;
    ASSERT_TRUE (LiftedPointDay (record, lifted, day));
    EXPECT_EQ (day.minutes, (std::vector<uint16_t> { 540, 600 }));
    EXPECT_EQ (day.lit, (std::vector<double> { 0.0, 1.0 }));
    EXPECT_EQ (day.facing, (std::vector<double> { 1.0, 1.0 }));
    EXPECT_EQ (day.windowFrom, 480);
    EXPECT_EQ (day.windowTo, 1080);
    EXPECT_DOUBLE_EQ (day.minDirectHours, 1.0);
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
