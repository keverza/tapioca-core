#include "ArchViz/ViewerStatus.hpp"

#include <gtest/gtest.h>

namespace vs = geomsrv::archviz::viewerstatus;

namespace {

vs::Inputs Drawing ()
{
    vs::Inputs in;
    in.drawing = true;
    in.view.known = true;
    return in;
}

} // namespace

TEST (ViewerStatus, AnIdleViewerAnnouncesOnlyItsViewAndModel)
{
    const vs::Blocks blocks = vs::Compose (Drawing ());
    EXPECT_EQ (blocks.view, "Perspective");
    EXPECT_EQ (blocks.inspection, "");
    EXPECT_EQ (blocks.analysis, "");
    EXPECT_EQ (blocks.model, "Model current");
    EXPECT_FALSE (blocks.analysisAlert);
    EXPECT_EQ (vs::Join (blocks), "Perspective  |  Model current");
}

TEST (ViewerStatus, TheViewBlockNamesTheProjection)
{
    vs::Inputs in = Drawing ();
    in.view.orthographic = true;
    EXPECT_EQ (vs::Compose (in).view, "Parallel");
    in.view.planMode = true;
    EXPECT_EQ (vs::Compose (in).view, "Plan");
    in.view.known = false;
    EXPECT_EQ (vs::Compose (in).view, "");
}

TEST (ViewerStatus, SelectionIsCountedOnlyWhenThereIsOne)
{
    vs::Inputs in = Drawing ();
    in.selectedCount = 3;
    EXPECT_EQ (vs::Compose (in).inspection, "3 selected");
}

TEST (ViewerStatus, ARunningStudyReportsQuantisedProgress)
{
    vs::Inputs in = Drawing ();
    in.runRunning = true;
    in.runStage = "calculating";
    in.runResolved = 34;
    in.runTotal = 50; // 68% -> 65%
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study 65%");
    in.runStage = "waiting";
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study: waiting for the model");
    in.runStage = "showing";
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study: drawing results");
}

TEST (ViewerStatus, APeriodReportsOneOverallPercentageThroughEveryDay)
{
    vs::Inputs in = Drawing ();
    in.runRunning = true;
    in.runPeriod = true;
    in.runWholeYear = true;
    in.runStage = "calculating";
    in.runResolved = 50;
    in.runTotal = 50; // the current day is complete: not 100% of the year
    in.runPeriodProgress = 37.0 / 365.0;
    EXPECT_EQ (vs::Compose (in).analysis, "Year study 10%");
    in.runStage = "snapshot"; // the next day's capture keeps the same number
    EXPECT_EQ (vs::Compose (in).analysis, "Year study 10%");
    in.runWholeYear = false;
    in.runPeriodProgress = 1.0;
    EXPECT_EQ (vs::Compose (in).analysis, "Period study 100%");
    in.runStage = "showing";
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study: drawing results");
}

TEST (ViewerStatus, PercentagesStepSoTheBarDoesNotRepaintEveryStep)
{
    EXPECT_EQ (vs::QuantisedPercent (0, 0), 0);
    EXPECT_EQ (vs::QuantisedPercent (1, 200), 0);
    EXPECT_EQ (vs::QuantisedPercent (19, 200), 5);
    EXPECT_EQ (vs::QuantisedPercent (199, 200), 95);
    EXPECT_EQ (vs::QuantisedPercent (200, 200), 100);
    EXPECT_EQ (vs::QuantisedPercent (500, 200), 100);
}

TEST (ViewerStatus, ARunTheUserStartedOutranksTheFollower)
{
    vs::Inputs in = Drawing ();
    in.follow = vs::FollowState::DirtyVisible;
    in.runRunning = true;
    in.runStage = "snapshot";
    const vs::Blocks blocks = vs::Compose (in);
    EXPECT_EQ (blocks.analysis, "Sun study: preparing");
    EXPECT_FALSE (blocks.analysisAlert);
}

TEST (ViewerStatus, OutdatedResultsAreAnAlertSeparateFromModelFreshness)
{
    vs::Inputs in = Drawing ();
    in.follow = vs::FollowState::DirtyVisible;
    in.followDirtyReason = "geometry";
    const vs::Blocks blocks = vs::Compose (in);
    EXPECT_EQ (blocks.analysis, "Sun study results outdated (geometry)");
    EXPECT_TRUE (blocks.analysisAlert);
    // The model itself is current: the two freshnesses are not one flag.
    EXPECT_EQ (blocks.model, "Model current");
    EXPECT_FALSE (blocks.modelAlert);
}

TEST (ViewerStatus, TheFollowerStatesReadAsTheyShould)
{
    vs::Inputs in = Drawing ();
    in.follow = vs::FollowState::UpdatingVisible;
    in.followResolved = 1;
    in.followTotal = 4;
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study updating 25%");
    in.follow = vs::FollowState::Current;
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study current");
    in.follow = vs::FollowState::Failed;
    in.followError = "no snapshot";
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study update failed: no snapshot");
    EXPECT_TRUE (vs::Compose (in).analysisAlert);
}

TEST (ViewerStatus, AFailedRunIsAnAlertWithItsReason)
{
    vs::Inputs in = Drawing ();
    in.runStage = "failed";
    in.runError = "viewer closed";
    const vs::Blocks blocks = vs::Compose (in);
    EXPECT_EQ (blocks.analysis, "Sun study failed: viewer closed");
    EXPECT_TRUE (blocks.analysisAlert);
}

TEST (ViewerStatus, AShownStudyWithoutAFollowerIsStillSaid)
{
    vs::Inputs in = Drawing ();
    in.studyShown = true;
    EXPECT_EQ (vs::Compose (in).analysis, "Sun study shown");
}

TEST (ViewerStatus, TheNextStepComesFirstAndKeepsTheStudyBesideIt)
{
    vs::Inputs in = Drawing ();
    in.hint = "click the model to place the sun study centre";
    EXPECT_EQ (vs::Compose (in).analysis, "Next: click the model to place the sun study centre");
    in.follow = vs::FollowState::Current;
    EXPECT_EQ (vs::Compose (in).analysis, "Next: click the model to place the sun study centre  |  Sun study current");
}

TEST (ViewerStatus, TheModelBlockReportsExtractionAndFaults)
{
    vs::Inputs in = Drawing ();
    in.extractionRunning = true;
    in.extractionDone = 50;
    in.extractionTotal = 200;
    EXPECT_EQ (vs::Compose (in).model, "Reading model 25%");
    in.extractionTotal = 0;
    EXPECT_EQ (vs::Compose (in).model, "Reading model");
    in.extractionRunning = false;
    in.extractionGaveUp = true;
    EXPECT_EQ (vs::Compose (in).model, "Model incomplete");
    EXPECT_TRUE (vs::Compose (in).modelAlert);
    in.extractionGaveUp = false;
    in.watchError = "generator failed";
    EXPECT_EQ (vs::Compose (in).model, "Model sync error");
    EXPECT_TRUE (vs::Compose (in).modelAlert);
}

TEST (ViewerStatus, HintsAreKeptPerSourceAndTheNewestWins)
{
    vs::ResetPublished ();
    EXPECT_EQ (vs::CurrentHint (), "");
    vs::PublishHint ("hud", "pick a point");
    vs::PublishHint ("banner", "orbit now");
    EXPECT_EQ (vs::CurrentHint (), "orbit now");
    // Republishing unchanged text does not make it the newest.
    vs::PublishHint ("hud", "pick a point");
    EXPECT_EQ (vs::CurrentHint (), "orbit now");
    // Clearing one source uncovers the other rather than blanking the line.
    vs::PublishHint ("banner", "");
    EXPECT_EQ (vs::CurrentHint (), "pick a point");
    vs::PublishHint ("hud", "pick a second point");
    vs::PublishHint ("banner", "orbit now");
    vs::PublishHint ("hud", "pick a third point");
    EXPECT_EQ (vs::CurrentHint (), "pick a third point");
    vs::ResetPublished ();
    EXPECT_EQ (vs::CurrentHint (), "");
}

TEST (ViewerStatus, TheViewIsUnknownUntilARenderThreadPublishesIt)
{
    vs::ResetPublished ();
    EXPECT_FALSE (vs::CurrentView ().known);
    vs::ViewFacts view;
    view.known = true;
    view.planMode = true;
    vs::PublishView (view);
    EXPECT_TRUE (vs::CurrentView ().known);
    EXPECT_TRUE (vs::CurrentView ().planMode);
    vs::ResetPublished ();
    EXPECT_FALSE (vs::CurrentView ().known);
}
