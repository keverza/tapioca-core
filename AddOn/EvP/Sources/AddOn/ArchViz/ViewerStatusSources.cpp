#include "ArchViz/ViewerStatusSources.hpp"

#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/ModelWatch.hpp"
#include "NativeCommands/SunStudyFollowerDriver.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"

namespace geomsrv::archviz::viewerstatus {

namespace {

const char* DirtyReasonName (evp::sunstudy::SunStudyDirtyReason reason)
{
    switch (reason) {
        case evp::sunstudy::SunStudyDirtyReason::Geometry:
            return "geometry";
        case evp::sunstudy::SunStudyDirtyReason::Sun:
            return "sun";
        case evp::sunstudy::SunStudyDirtyReason::Sampling:
            return "sampling";
        case evp::sunstudy::SunStudyDirtyReason::None:
            break;
    }
    return "";
}

// The HUD's click-in-the-model steps, in the words of the button that started
// them. ⚠️ EACH NAMES HOW TO GET OUT: a step that only says what to click leaves
// a user who changed their mind with no way back but guessing.
std::string HudHint (const HudState& hud)
{
    if (hud.sunRunPickingCentre)
        return "click the model to place the sun study centre (Pick centre again cancels)";
    if (hud.viewpointPlacing)
        return "click a model surface to place the viewpoint";
    if (hud.viewpointPlacingTarget)
        return "click a model surface to place the viewpoint's target";
    if (hud.viewpointPickingContext)
        return "click elements to add or remove them from Context, then Done picking";
    return std::string ();
}

} // namespace

Inputs Gather (const DiligentViewportStats& viewport)
{
    Inputs in;
    in.drawing = viewport.running && viewport.initialized && viewport.sceneReady && !viewport.failed;
    in.view = CurrentView ();
    in.selectedCount = viewport.selectedCount;
    in.studyShown = !viewport.sunStudy.studyId.empty ();

    const sunstudyhud::Status run = sunstudyhud::Current ();
    in.runRunning = run.running;
    in.runStage = run.stage;
    in.runResolved = run.resolvedSteps;
    in.runTotal = run.totalSteps;
    in.runError = run.error;
    in.runPeriod = run.period;
    in.runWholeYear = run.wholeYear;
    in.runPeriodProgress = sunstudyhud::PeriodProgress (run);

    const sunfollow::FollowerStats follow = sunfollow::State ();
    in.follow = FollowState (follow.state);
    in.followDirtyReason = DirtyReasonName (follow.dirtyReason);
    in.followResolved = follow.resolvedSteps;
    in.followTotal = follow.totalSteps;
    in.followError = follow.lastError;

    const ExtractionWorker::Progress extraction = ExtractionWorker::Get ().Snapshot ();
    in.extractionRunning = extraction.running && !extraction.done;
    in.extractionGaveUp = extraction.gaveUp;
    in.extractionDone = extraction.extracted + extraction.empty;
    in.extractionTotal = extraction.total;
    in.watchError = modelwatch::Get ().lastError;

    in.hint = CurrentHint ();
    return in;
}

void PublishFromHud (const HudState& hud)
{
    ViewFacts view;
    view.known = true;
    view.orthographic = hud.orthographic;
    view.planMode = hud.planMode;
    // ⚠️ NO "LAST PUBLISHED" CACHE HERE. One would survive ResetPublished at
    // viewer close and the next viewer would never republish an unchanged view;
    // two uncontended locks a frame cost nothing next to that.
    PublishView (view);
    PublishHint ("hud", HudHint (hud));
    // The instruction banner (PLAT-RE111) is a bus caller's "what to do next";
    // the bar repeats it, without the countdown that would repaint it each second.
    PublishHint ("banner", hud.instruction);
}

} // namespace geomsrv::archviz::viewerstatus
