#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;
using evp::sunstudy::SunObstructionCell;
using evp::sunstudy::SunObstructionResult;
using evp::sunstudy::SunObstructionState;

namespace {

SunObstructionResult Hit (int minute, size_t sample = 0, double x = 0.0)
{
    SunObstructionResult step;
    step.minute = minute;
    SunObstructionCell cell;
    cell.sample = sample;
    cell.state = SunObstructionState::Blocked;
    cell.blockerGuid = " {Roof} "; // canonical GUID matching finds the source mesh
    cell.sourceTriangle = 1;
    cell.origin[0] = x;
    cell.hit[0] = x;
    cell.hit[2] = 5.0;
    evp::sunstudy::SunObstructionFragment fragment;
    fragment.blockerGuid = cell.blockerGuid;
    const double corners[4][2] = { { 0, 0 }, { 0.5, 0 }, { 0.5, 0.5 }, { 0, 0.5 } };
    for (int c = 0; c < 4; ++c) {
        fragment.corners[c][0] = x + corners[c][0];
        fragment.corners[c][1] = corners[c][1];
        fragment.corners[c][2] = 5.0;
    }
    cell.fragments.push_back (fragment);
    step.cells.push_back (cell);
    return step;
}

} // namespace

TEST (SunStudyObstructionGeometry, OneStepFillsTheFootprintsAndRaysInTheStepsTimeColour)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    SunObstructionGeometry geometry;
    auto step = Hit (480);
    step.cells.push_back (Hit (480, 1, 0.5).cells[0]);
    ASSERT_TRUE (BuildSunObstructionGeometry (snapshot, { step }, 60, geometry)) << geometry.error;
    EXPECT_EQ (geometry.blockerObjects, 1u);
    EXPECT_EQ (geometry.footprints, 2u); // one per cell's fragment, no whole triangle
    ASSERT_EQ (geometry.drawables.depthTested.meshVertices.size (), 8u);
    EXPECT_EQ (geometry.drawables.depthTested.meshVertices[0].rgba & 0xFFFFFF00u,
               SunObstructionTimeColour (480) & 0xFFFFFF00u);
    EXPECT_GT (geometry.drawables.depthTested.lineVertices.size (), 0u); // each blocked cell's ray
    ASSERT_EQ (geometry.blockers.size (), 1u);
    EXPECT_EQ (geometry.blockers[0].guid, "roof");
    EXPECT_DOUBLE_EQ (geometry.blockers[0].hours, 1.0); // two cells in one step count one hour, not two
    EXPECT_DOUBLE_EQ (geometry.blockers[0].meanCellShare, 1.0);
    EXPECT_EQ (geometry.blocked, 2u);
    EXPECT_TRUE (geometry.drawables.xray.Empty ());
}

TEST (SunStudyObstructionGeometry, WholeDayKeepsSeparatedIntervalsAndTheirTimeColours)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    SunObstructionGeometry geometry;
    ASSERT_TRUE (BuildSunObstructionGeometry (snapshot, { Hit (480), Hit (540), Hit (900) }, 60, geometry))
        << geometry.error;
    ASSERT_EQ (geometry.blockers.size (), 1u);
    EXPECT_EQ (geometry.blockers[0].intervals,
               (std::vector<std::pair<int, int>> { { 480, 600 }, { 900, 960 } })); // morning AND afternoon kept
    EXPECT_DOUBLE_EQ (geometry.blockers[0].hours, 3.0);
    EXPECT_EQ (geometry.footprints, 3u);
    EXPECT_EQ (geometry.drawables.depthTested.meshVertices[0].rgba & 0xFFFFFF00u,
               SunObstructionTimeColour (480) & 0xFFFFFF00u);
    EXPECT_EQ (geometry.drawables.depthTested.meshVertices[8].rgba & 0xFFFFFF00u,
               SunObstructionTimeColour (900) & 0xFFFFFF00u);
    EXPECT_NE (SunObstructionTimeColour (480), SunObstructionTimeColour (900));
    EXPECT_EQ (SunObstructionTimeColour (0), SunObstructionTimeColour (1440));
}

TEST (SunStudyObstructionGeometry, MissingOrInvalidSourcePartsRefuseRatherThanDrawWrongGeometry)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    auto missing = Hit (480);
    missing.cells[0].blockerGuid = "other";
    SunObstructionGeometry geometry;
    EXPECT_FALSE (BuildSunObstructionGeometry (snapshot, { missing }, 60, geometry));
    auto invalid = Hit (480);
    invalid.cells[0].sourceTriangle = 99;
    EXPECT_FALSE (BuildSunObstructionGeometry (snapshot, { invalid }, 60, geometry));
}
