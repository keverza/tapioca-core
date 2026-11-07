#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;
using evp::sunstudy::SunObstructionCell;
using evp::sunstudy::SunObstructionResult;
using evp::sunstudy::SunObstructionState;

namespace {

SunObstructionResult Hit (int minute, size_t sample = 0)
{
    SunObstructionResult step;
    step.minute = minute;
    SunObstructionCell cell;
    cell.sample = sample;
    cell.state = SunObstructionState::Blocked;
    cell.blockerGuid = " {Roof} "; // canonical GUID matching finds the source mesh
    cell.sourceTriangle = 1;
    step.cells.push_back (cell);
    return step;
}

} // namespace

TEST (SunStudyObstructionGeometry, OneStepFillsOnlyTheExactPartAndWiresItsWholeObject)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    snapshot.meshes[0].triWireEdges.assign (12, 0x7); // synthetic fixture: every tessellation edge is a source edge
    SunObstructionGeometry geometry;
    ASSERT_TRUE (BuildSunObstructionGeometry (snapshot, { Hit (480), Hit (480, 1) }, false, geometry))
        << geometry.error;
    EXPECT_EQ (geometry.blockerObjects, 1u);
    EXPECT_EQ (geometry.timedParts, 1u); // two selected cells share the same part and step
    ASSERT_EQ (geometry.drawables.depthTested.meshIndices.size (), 3u);
    ASSERT_EQ (geometry.drawables.depthTested.meshVertices.size (), 3u);
    EXPECT_EQ (geometry.drawables.depthTested.meshVertices[0].rgba, kSunObstructionPink);
    EXPECT_GT (geometry.drawables.depthTested.lineVertices.size (), 0u);
    EXPECT_TRUE (geometry.drawables.xray.Empty ());
}

TEST (SunStudyObstructionGeometry, WholeDayColoursEachTimedPartByItsShadowHour)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    SunObstructionGeometry geometry;
    ASSERT_TRUE (BuildSunObstructionGeometry (snapshot, { Hit (480), Hit (720) }, true, geometry)) << geometry.error;
    EXPECT_EQ (geometry.blockerObjects, 1u); // wireframe is still deduplicated
    EXPECT_EQ (geometry.timedParts, 2u);
    ASSERT_EQ (geometry.drawables.depthTested.meshVertices.size (), 6u);
    EXPECT_EQ (geometry.drawables.depthTested.meshVertices[0].rgba, SunObstructionTimeColour (480));
    EXPECT_EQ (geometry.drawables.depthTested.meshVertices[3].rgba, SunObstructionTimeColour (720));
    EXPECT_NE (SunObstructionTimeColour (480), SunObstructionTimeColour (720));
    EXPECT_EQ (SunObstructionTimeColour (0), SunObstructionTimeColour (1440));
}

TEST (SunStudyObstructionGeometry, MissingOrInvalidSourcePartsRefuseRatherThanDrawWrongGeometry)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes = { evptest::MakeBox ("roof", 0, 0, 5) };
    auto missing = Hit (480);
    missing.cells[0].blockerGuid = "other";
    SunObstructionGeometry geometry;
    EXPECT_FALSE (BuildSunObstructionGeometry (snapshot, { missing }, false, geometry));
    auto invalid = Hit (480);
    invalid.cells[0].sourceTriangle = 99;
    EXPECT_FALSE (BuildSunObstructionGeometry (snapshot, { invalid }, false, geometry));
}
