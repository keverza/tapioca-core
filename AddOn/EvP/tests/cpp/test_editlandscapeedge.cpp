#include "Edit/EditLandscapeEdge.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace {
namespace edit = geomsrv::edit;
using namespace tapioca::geometry;

// A landscape with a straight 6 m baseline east from (0, 0).
struct Site {
    edit::Document doc;
    edit::Id line = 0;
    std::string error;
    Site ()
    {
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
        line = edit::AddPath (doc, { { 0, 0, 0 }, { 6, 0, 0 } }, 1.0);
    }
    EdgeSpec Curb (double top = 0.15) const
    {
        EdgeSpec e;
        e.kind = EdgeKind::Curb;
        e.pathId = line;
        e.width = 0.15;
        e.topZ = top;
        return e;
    }
    // Five 0.17 m rises of 0.30 m, then a 1 m landing: 2.5 m of the 6 m baseline.
    EdgeSpec Stair () const
    {
        EdgeSpec e;
        e.kind = EdgeKind::Stair;
        e.pathId = line;
        e.width = 1.2;
        e.bottomZ = 0.0;
        e.topZ = 0.85;
        e.stairSteps = 5;
        e.stairRun = 0.30;
        e.stairRise = 0.17;
        e.landingDepth = 1.0;
        return e;
    }
};

// Ground at `high` on the left of the x axis (y > 0) and `low` on its right.
HeightAt Step (double high, double low)
{
    return [=] (double, double y, double& z) {
        z = y > 0 ? high : low;
        return true;
    };
}
HeightAt Flat (double level)
{
    return [=] (double, double, double& z) {
        z = level;
        return true;
    };
}
} // namespace

TEST (EditLandscapeEdge, AnEdgeFollowsOneStraightBaseline)
{
    Site site;
    Vec3 a, b;
    ASSERT_TRUE (EdgeBaseline (site.doc, site.Curb (), a, b, site.error)) << site.error;
    EXPECT_DOUBLE_EQ (b.x, 6.0);
    const auto bent = edit::AddPath (site.doc, { { 0, 0, 0 }, { 3, 0, 0 }, { 3, 3, 0 } }, 1.0);
    auto curb = site.Curb ();
    curb.pathId = bent;
    EXPECT_FALSE (ValidateEdge (site.doc, curb, {}, site.error));
    EXPECT_EQ (site.error.rfind ("EdgeBaselineNotStraight", 0), 0u) << site.error;
    const auto straight = edit::AddPath (site.doc, { { 0, 0, 0 }, { 2, 0, 0 }, { 5, 0, 0 } }, 1.0);
    curb.pathId = straight;
    EXPECT_TRUE (ValidateEdge (site.doc, curb, {}, site.error)) << site.error; // collinear points are straight
    curb.pathId = 999;
    EXPECT_FALSE (ValidateEdge (site.doc, curb, {}, site.error));
    EXPECT_EQ (site.error.rfind ("EdgeBaselineMissing", 0), 0u);
}

TEST (EditLandscapeEdge, ACurbAboveTheMaximumIsRefusedWithASuggestion)
{
    Site site;
    EdgeLimits limits;
    limits.maxCurbHeight = 0.25;
    EXPECT_TRUE (ValidateEdge (site.doc, site.Curb (0.25), limits, site.error)) << site.error;
    EXPECT_FALSE (ValidateEdge (site.doc, site.Curb (0.30), limits, site.error));
    EXPECT_EQ (site.error.rfind ("CurbTooHigh", 0), 0u);
    EXPECT_NE (site.error.find ("retaining wall or a stair"), std::string::npos);
    EXPECT_FALSE (ValidateEdge (site.doc, site.Curb (0.0), limits, site.error));
    EXPECT_EQ (site.error.rfind ("EdgeLevelsInverted", 0), 0u);
}

TEST (EditLandscapeEdge, AWallIsAtLeastFiftyMillimetresThick)
{
    Site site;
    EdgeSpec wall = site.Curb (1.2);
    wall.kind = EdgeKind::RetainingWall;
    wall.width = 0.049;
    EXPECT_FALSE (ValidateEdge (site.doc, wall, {}, site.error));
    EXPECT_EQ (site.error.rfind ("WallTooThin", 0), 0u);
    wall.width = kMinWallThickness;
    EXPECT_TRUE (ValidateEdge (site.doc, wall, {}, site.error)) << site.error;
    wall.footingDepth = -0.1;
    EXPECT_FALSE (ValidateEdge (site.doc, wall, {}, site.error));
}

TEST (EditLandscapeEdge, AStairsStepsClimbFromItsFootToItsTopAndFitTheBaseline)
{
    Site site;
    EXPECT_TRUE (ValidateEdge (site.doc, site.Stair (), {}, site.error)) << site.error;
    auto stair = site.Stair ();
    stair.stairSteps = 4;
    EXPECT_FALSE (ValidateEdge (site.doc, stair, {}, site.error));
    EXPECT_EQ (site.error.rfind ("StairLevelsMismatch", 0), 0u);
    stair = site.Stair ();
    stair.landingDepth = 5.0;
    EXPECT_FALSE (ValidateEdge (site.doc, stair, {}, site.error));
    EXPECT_EQ (site.error.rfind ("StairTooLong", 0), 0u);
    stair = site.Stair ();
    stair.stairRun = 0;
    EXPECT_FALSE (ValidateEdge (site.doc, stair, {}, site.error));
    EXPECT_EQ (site.error.rfind ("StairInvalid", 0), 0u);
}

TEST (EditLandscapeEdge, EdgesAreOneDocumentEditEachAndRefusalsChangeNothing)
{
    Site site;
    const auto revision = site.doc.revision;
    Id curb = 0;
    ASSERT_TRUE (AddLandscapeEdge (site.doc, site.Curb (), {}, curb, site.error)) << site.error;
    EXPECT_EQ (site.doc.revision, revision + 1);
    ASSERT_NE (FindLandscapeEdge (site.doc, curb), nullptr);
    const auto before = site.doc;
    Id refused = 0;
    EXPECT_FALSE (AddLandscapeEdge (site.doc, site.Curb (0.9), {}, refused, site.error));
    auto changed = *FindLandscapeEdge (site.doc, curb);
    EXPECT_FALSE (UpdateLandscapeEdge (site.doc, changed, {}, site.error)); // unchanged: no edit
    changed.topZ = 0.9;
    EXPECT_FALSE (UpdateLandscapeEdge (site.doc, changed, {}, site.error));
    EXPECT_EQ (site.doc.revision, before.revision);
    EXPECT_EQ (site.doc.landscape, before.landscape);
    EXPECT_EQ (site.doc.nextId, before.nextId);
    changed.topZ = 0.12;
    ASSERT_TRUE (UpdateLandscapeEdge (site.doc, changed, {}, site.error)) << site.error;
    EXPECT_DOUBLE_EQ (FindLandscapeEdge (site.doc, curb)->topZ, 0.12);
    ASSERT_TRUE (RemoveLandscapeEdge (site.doc, curb, site.error)) << site.error;
    EXPECT_EQ (FindLandscapeEdge (site.doc, curb), nullptr);
    EXPECT_FALSE (RemoveLandscapeEdge (site.doc, curb, site.error));
}

TEST (EditLandscapeEdge, EdgesRoundTripThroughTheSessionCodec)
{
    Site site;
    Id curb = 0, stair = 0;
    ASSERT_TRUE (AddLandscapeEdge (site.doc, site.Curb (), {}, curb, site.error)) << site.error;
    auto spec = site.Stair ();
    spec.highSide = EdgeSide::Right;
    spec.stairGuid = "12345678-1234-4234-9234-123456789ABC";
    spec.material.preset = MaterialPreset::HardPaving;
    ASSERT_TRUE (AddLandscapeEdge (site.doc, spec, {}, stair, site.error)) << site.error;
    std::string text;
    ASSERT_TRUE (WriteLandscape (site.doc, LandscapeStorage::Virtual, text, site.error)) << site.error;
    edit::Document loaded;
    ASSERT_TRUE (ReadLandscape (text, loaded, site.error)) << site.error;
    EXPECT_EQ (loaded.landscape->edges, site.doc.landscape->edges);
    // A stored edge must still be well formed.
    auto broken = site.doc;
    broken.landscape->edges[0].stairGuid = "not a guid";
    EXPECT_FALSE (ValidateLandscape (broken, site.error));
    broken = site.doc;
    broken.landscape->edges[1].id = broken.landscape->edges[0].id;
    EXPECT_FALSE (ValidateLandscape (broken, site.error));
}

TEST (EditLandscapeEdge, AStairsUndersideStepsOneRiseBelowItsTreads)
{
    Site site;
    StairEnvelope env;
    ASSERT_TRUE (BuildStairEnvelope (site.doc, site.Stair (), env, site.error)) << site.error;
    EXPECT_DOUBLE_EQ (env.Length (), 2.5);
    double z = 0;
    ASSERT_TRUE (StairUnderside (env, { 0.1, 0, 0 }, z));
    EXPECT_NEAR (z, 0.0, 1e-12); // the first tread at 0.17, its underside at the foot
    ASSERT_TRUE (StairUnderside (env, { 0.75, 0.5, 0 }, z));
    EXPECT_NEAR (z, 0.34, 1e-12); // third tread
    ASSERT_TRUE (StairUnderside (env, { 2.0, -0.6, 0 }, z));
    EXPECT_NEAR (z, 0.85 - 0.17, 1e-12);                     // the landing
    EXPECT_FALSE (StairUnderside (env, { 3.0, 0, 0 }, z));   // past the landing
    EXPECT_FALSE (StairUnderside (env, { 1.0, 0.7, 0 }, z)); // beside the flight
    EXPECT_FALSE (BuildStairEnvelope (site.doc, site.Curb (), env, site.error));
}

TEST (EditLandscapeEdge, TerrainUnderAStairIsCutToTheClearanceAndNeverFilled)
{
    Site site;
    StairEnvelope env;
    ASSERT_TRUE (BuildStairEnvelope (site.doc, site.Stair (), env, site.error)) << site.error;
    // Cut where the ground rises into the clearance, left where it is below.
    EXPECT_NEAR (StairCutHeight (env, { 0.75, 0, 0 }, 1.0), 0.34 - 0.05, 1e-12);
    EXPECT_NEAR (StairCutHeight (env, { 0.75, 0, 0 }, -1.0), -1.0, 1e-12);
    EXPECT_NEAR (StairCutHeight (env, { 4.0, 0, 0 }, 1.0), 1.0, 1e-12); // outside: untouched

    StairClearance check;
    ASSERT_TRUE (CheckStairClearance (env, Flat (-0.5), 0.1, check, site.error)) << site.error;
    EXPECT_GT (check.samples, 100u);
    EXPECT_TRUE (check.cuts.empty ());
    EXPECT_NEAR (check.maxGap, 0.85 - 0.17 - 0.05 + 0.5, 1e-9); // air under the landing, reported not filled
    ASSERT_TRUE (CheckStairClearance (env, Flat (0.2), 0.1, check, site.error)) << site.error;
    EXPECT_FALSE (check.cuts.empty ());
    EXPECT_NEAR (check.maxCut, 0.2 - (0.0 - 0.05), 1e-9); // under the first tread
    for (const auto& c : check.cuts) {
        double under = 0;
        ASSERT_TRUE (StairUnderside (env, c, under));
        EXPECT_GT (c.z, under - env.clearance);
    }
    const HeightAt none = [] (double, double, double&) { return false; };
    EXPECT_FALSE (CheckStairClearance (env, none, 0.1, check, site.error));
    EXPECT_FALSE (CheckStairClearance (env, Flat (0), 0.0, check, site.error));
}

TEST (EditLandscapeEdge, ACurbMeetsTheGroundAtItsLevelOnEachSide)
{
    Site site;
    const auto curb = site.Curb (0.15);
    EXPECT_TRUE (CheckCurbLevels (site.doc, curb, Step (0.15, 0.0), 0.5, 0.005, site.error)) << site.error;
    EXPECT_FALSE (CheckCurbLevels (site.doc, curb, Step (0.15, 0.05), 0.5, 0.005, site.error));
    EXPECT_NE (site.error.find ("lower side is up to 0.050 m"), std::string::npos) << site.error;
    auto right = curb;
    right.highSide = EdgeSide::Right;
    EXPECT_FALSE (CheckCurbLevels (site.doc, right, Step (0.15, 0.0), 0.5, 0.005, site.error));
    EXPECT_TRUE (CheckCurbLevels (site.doc, right, Step (0.0, 0.15), 0.5, 0.005, site.error)) << site.error;
}

TEST (EditLandscapeEdge, AWallCoversTheGroundRunningThroughIt)
{
    Site site;
    EdgeSpec wall = site.Curb (1.2);
    wall.kind = EdgeKind::RetainingWall;
    wall.width = 0.25;
    wall.footingDepth = 0.3;
    // A steep slope through the wall body: 1.0 m on the high face, 0.1 m on the low face.
    const HeightAt through = [] (double, double y, double& z) {
        z = 0.55 + y * 3.6;
        return true;
    };
    EXPECT_TRUE (CheckWallCoverage (site.doc, wall, through, 0.5, site.error)) << site.error;
    EXPECT_FALSE (CheckWallCoverage (site.doc, wall, Step (1.3, 0.0), 0.5, site.error)); // buried
    EXPECT_NE (site.error.find ("above the wall top"), std::string::npos);
    EXPECT_FALSE (CheckWallCoverage (site.doc, wall, Step (1.0, -0.4), 0.5, site.error)); // undermined
    EXPECT_NE (site.error.find ("below the wall foot"), std::string::npos);
    EXPECT_TRUE (CheckWallCoverage (site.doc, wall, Step (1.0, -0.3), 0.5, site.error)) << site.error;
}

TEST (EditLandscapeEdge, SteepJumpsAreFoundAndSuggestedButNeverInserted)
{
    Site site;
    EdgeLimits limits;
    limits.maxCurbHeight = 0.2;
    limits.maxGrade = 50;
    // Ground along y: level, a 0.15 m step at y = 0, level, then a 1 m drop at y = 4.
    const HeightAt ground = [] (double, double y, double& z) {
        z = (y < 0 ? 0.0 : 0.15) + (y < 4 ? 0.0 : -1.0);
        return true;
    };
    const std::vector<Vec3> across { { 3, -2, 0 }, { 3, 6, 0 } };
    auto jumps = FindSteepJumps (site.doc, across, ground, 0.1, limits, false);
    ASSERT_EQ (jumps.size (), 2u);
    EXPECT_NEAR (jumps[0].drop, 0.15, 1e-9);
    EXPECT_EQ (jumps[0].suggestion, EdgeKind::Curb);
    EXPECT_NEAR (jumps[1].drop, 1.0, 1e-9);
    EXPECT_EQ (jumps[1].suggestion, EdgeKind::RetainingWall);
    EXPECT_EQ (FindSteepJumps (site.doc, across, ground, 0.1, limits, true)[1].suggestion, EdgeKind::Stair);
    EXPECT_TRUE (site.doc.landscape->edges.empty ());
    // A curb on the first jump's line already carries it.
    Id curb = 0;
    ASSERT_TRUE (AddLandscapeEdge (site.doc, site.Curb (), limits, curb, site.error)) << site.error;
    jumps = FindSteepJumps (site.doc, across, ground, 0.1, limits, false);
    ASSERT_EQ (jumps.size (), 2u);
    EXPECT_EQ (jumps[0].existing, curb);
    EXPECT_EQ (jumps[1].existing, kNoId);
    // Below the threshold there is no jump.
    limits.maxGrade = 2000;
    EXPECT_TRUE (FindSteepJumps (site.doc, across, ground, 0.1, limits, false).empty ());
}
