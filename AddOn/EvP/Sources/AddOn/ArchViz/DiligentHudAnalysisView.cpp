// ArchViz/DiligentHudAnalysisView -- the HUD's per-frame word to the scene about
// the analysis tint: which Analysis tab owns the resident study (AnalysisTabs.hpp),
// and so whether the tint draws at all. Runs every frame, whichever HUD page is
// open, so a study a command runs is followed even while the Analysis page is shut.

#include "ArchViz/AnalysisTabs.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SunStudyObstructionController.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "ArchViz/SunStudyPreview.hpp"
#include "ArchViz/ViewpointOverlayGeometry.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "Geometry/MeshStore.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace geomsrv::archviz {

namespace {

// FNV-1a over the bytes of what the isovist is built from.
void Mix (uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

} // namespace

void ApplyAnalysisView (DiligentScene& scene, HudState& state)
{
    const std::string shown = scene.ShownSunStudyId ();
    const uint32_t kind = scene.ShownAnalysisKind ();
    // The HUD's own run knows its origin; a command's is taken to be the
    // visibility tab last used, which is what the old single page assumed.
    int origin = state.visibilityOrigin;
    if (kind == 1) {
        const auto status = visibilitystudy::GetStatus ();
        if (!shown.empty () && status.studyId == shown)
            origin = status.origin;
    }
    viewerhud::FollowAnalysisPath (state);
    auto& queue = SceneCmdQueue::Get ();
    analysistabs::FollowShownStudy (state.analysisFocus, shown, kind, queue.AnalysisDisplayGeneration (), origin);
    // ⚠️ EACH TINT TAB PUTS ITS OWN STUDY BACK. The three share one tint channel,
    // so the last run evicts the others; without this the sun tab said "a
    // visibility study is on screen" and could not show its own again.
    auto& focus = state.analysisFocus;
    for (int tab = 0; tab < 3; ++tab)
        if (focus.keptStudy[tab].empty ())
            state.analysisKept[tab].reset ();
    const int restore = analysistabs::KeepShownStudy (focus, shown, kind);
    if (focus.lastShownTab >= 0) {
        auto payload = scene.ShownAnalysisPayload ();
        if (payload != nullptr && payload->studyId == shown)
            state.analysisKept[focus.lastShownTab] = std::move (payload);
    }
    if (restore >= 0 && state.analysisKept[restore] != nullptr) {
        const auto& kept = state.analysisKept[restore];
        const bool stale = kept->analysisKind == 1 && kept->captureStamp != 0 &&
                           kept->captureStamp != MeshStore::Get ().CaptureStamp ();
        // Through the normal path, as a whole upload; the claim it makes is
        // marked seen so the put-back does not move the focus. A study the model
        // moved under goes back too: its unchanged elements still read right, and
        // the tab's follow re-runs it against the new capture.
        const uint64_t claim = queue.ClaimAnalysisDisplay (kept->analysisKind);
        focus.seenClaim = claim;
        queue.PushAnalysisAtlas (claim, std::make_unique<SunStudyAtlasUpload> (*kept));
        if (restore == analysistabs::kPointVisibilityTab) {
            const auto status = visibilitystudy::GetStatus ();
            if (stale || status.studyId != focus.keptStudy[restore])
                state.visibilityLiveKey.clear (); // a newer run was lost to the switch
        }
    }
    SunStudyViewSettings view = SunStudyViewOf (state);
    view.suppressed = !analysistabs::ShowsTint (state.analysisFocus, shown, kind);
    view.visibilityLowRgba = state.visibilityLowRgba;
    view.visibilityHighRgba = state.visibilityHighRgba;
    {
        std::vector<overlaylayers::ColourStop> stops;
        if (state.visibilityGradientPreset == overlaylayers::kCustomGradientId)
            stops = state.visibilityGradientCustom;
        else if (const overlaylayers::GradientPreset* preset =
                     overlaylayers::FindGradientPreset (state.visibilityGradientPreset))
            stops.assign (preset->Stops ().begin (), preset->Stops ().end ());
        if (state.visibilityGradientFlipped)
            stops = overlaylayers::ReversedStops (stops);
        if (stops.size () >= 2 && stops.size () <= overlaylayers::kMaxStops) {
            view.visibilityRampCount = int (stops.size ());
            std::copy (stops.begin (), stops.end (), view.visibilityRamp);
        }
    }
    scene.SetSunStudyView (view);
    // Outline what the visibility tab in focus measures: Analysis, which carries
    // the gradient on both, and Focus on the surface tab, where it is looked at.
    static const std::vector<std::string> kNone;
    const int tab = state.analysisFocus.tab;
    const bool visibilityTab = tab == analysistabs::kSurfaceVisibilityTab || tab == analysistabs::kPointVisibilityTab;
    scene.SetAnalysisHighlight (tab == analysistabs::kSurfaceVisibilityTab ? state.visibilityTo : kNone,
                                visibilityTab ? state.visibilityFrom : kNone);
}

void UpdateAndDrawAnalysisOverlay (DiligentScene& scene, Diligent::IDeviceContext* context, const HudState& state,
                                   const float viewProj[16], uint32_t width, uint32_t height, uint32_t colorFormat,
                                   uint32_t depthFormat, bool modelIsDrawn, const Camera& camera)
{
    PointVisibilityGuide guide;
    if (viewerhud::PointVisibilityGuideOf (state, guide)) {
        // Field by field: the struct's padding is not part of what it says.
        uint64_t key = 1469598103934665603ull ^ 0x9E3779B97F4A7C15ull; // never a viewpoint key
        Mix (key, guide.point, sizeof (guide.point));
        Mix (key, guide.direction, sizeof (guide.direction));
        Mix (key, guide.target, sizeof (guide.target));
        for (const double value : { guide.coneDegrees, guide.minElevationDegrees, guide.maxElevationDegrees })
            Mix (key, &value, sizeof (value));
        for (const bool flag : { guide.sphere, guide.hasTarget })
            Mix (key, &flag, sizeof (flag));
        const void* path = state.analysisPath.get ();
        Mix (key, &path, sizeof (path));
        key |= 1u;
        if (key == scene.AnalysisOverlayKey ()) {
            scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, key,
                                       modelIsDrawn, camera);
            return;
        }
        GhPreviewDrawables drawables = BuildPointVisibilityGuides (guide);
        if (state.analysisPath != nullptr)
            AppendPolyline (state.analysisPath->points, kAnalysisPathRgba, drawables.depthTested);
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, &drawables, key,
                                   modelIsDrawn, camera);
        return;
    }
    const auto settings = viewpointstudy::GetSettings ();
    if (state.readOnly || !settings->enabled || !settings->pointValid ||
        state.analysisFocus.tab != analysistabs::kViewpointTab) {
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, 0, modelIsDrawn,
                                   camera);
        return;
    }
    const auto status = viewpointstudy::GetStatus ();
    ViewpointOverlayStyle style;
    style.fillRgba = state.viewpointFillRgba;
    style.openEdgeRgba = state.viewpointEdgeRgba;
    style.blockedRgba = state.viewpointBlockedRgba;
    // The guide circle is the open edge's colour, quieter.
    style.guideRgba = (state.viewpointEdgeRgba & 0xFFFFFF00u) | ((state.viewpointEdgeRgba & 0xFFu) * 2u / 3u);
    // Rebuilt only when what it draws changes; the camera is the layer's viewProj.
    uint64_t key = 1469598103934665603ull;
    const void* result = status.result.get ();
    Mix (key, &result, sizeof (result));
    const auto& options = settings->options;
    Mix (key, options.point.data (), sizeof (double) * 3);
    for (const double value : { options.radius, options.bearingDegrees, options.fieldOfViewDegrees,
                                options.minElevationDegrees, options.maxElevationDegrees })
        Mix (key, &value, sizeof (value));
    const auto mode = uint8_t (options.mode);
    Mix (key, &mode, sizeof (mode));
    // A target-facing field of view: the line to what it faces.
    const bool target = viewerhud::ShowsTargetPoint (state);
    Mix (key, &target, sizeof (target));
    if (target)
        Mix (key, state.visibilityTarget, sizeof (state.visibilityTarget));
    Mix (key, &style, sizeof (style));
    const void* path = state.analysisPath.get ();
    Mix (key, &path, sizeof (path));
    key |= 1u; // never the "hidden" key
    if (key == scene.AnalysisOverlayKey ()) {
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, key,
                                   modelIsDrawn, camera);
        return;
    }
    GhPreviewDrawables drawables = BuildViewpointOverlay (settings->options, status.result.get (), style);
    if (target)
        AppendGuideSegment (options.point,
                            { state.visibilityTarget[0], state.visibilityTarget[1], state.visibilityTarget[2] },
                            style.guideRgba, drawables.depthTested);
    if (state.analysisPath != nullptr)
        AppendPolyline (state.analysisPath->points, kAnalysisPathRgba, drawables.depthTested);
    scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, &drawables, key,
                               modelIsDrawn, camera);
}

void UpdateAndDrawObstructionOverlay (DiligentScene& scene, Diligent::IDeviceContext* context, HudState& state,
                                      const float viewProj[16], uint32_t width, uint32_t height, uint32_t colorFormat,
                                      uint32_t depthFormat, bool modelIsDrawn)
{
    const std::string shown = scene.ShownSunStudyId ();
    SunObstructionRequest request;
    const bool sunTab = state.analysisFocus.tab == analysistabs::kSunTab && scene.ShownAnalysisKind () == 0;
    if (!state.readOnly && sunTab && state.sunView == kSunObstructionView && !state.sunShowsPeriod &&
        state.sunPointPlaced && state.sunObstructionMode != 0 && !shown.empty () &&
        RefreshSunStudyPointSamples (state, shown)) {
        request.studyId = shown;
        request.samples = state.sunPointSamples;
        request.mode = state.sunObstructionMode == 2 ? SunObstructionMode::WholeDay : SunObstructionMode::TimeStep;
        request.step = state.sunStep;
        // Whole day attributes the diagram's selected slice (#167).
        request.fromMinute = std::clamp (int (std::lround (state.sunTimeFrom * 60.0f)), 0, 1440);
        request.toMinute = std::clamp (int (std::lround (state.sunTimeTo * 60.0f)), 0, 1440);
        if (request.toMinute <= request.fromMinute)
            request.fromMinute = 0, request.toMinute = 1440;
        // The solar cut: the pink slice on the day diagram, for the point as placed.
        if (state.sunObstructionMode == 3 && state.sunCutTo > state.sunCutFrom) {
            request.mode = SunObstructionMode::SolarCut;
            request.fromMinute = int (std::lround (state.sunCutFrom));
            request.toMinute = int (std::lround (state.sunCutTo));
            request.lifted = state.sunPointHeight > 0.0f && state.sunPointLiftedReady;
            std::copy (state.sunPointLifted, state.sunPointLifted + 3, request.liftedPoint);
            request.radius = double (state.sunPointRadius);
        }
        else if (state.sunObstructionMode == 3)
            request.mode = SunObstructionMode::Off;
    }
    auto& tasks = SunObstructionTasks ();
    tasks.Request (request);
    const auto frame = tasks.Latest ();
    static const std::vector<std::string> kNoBlockers;
    const bool current =
        request.mode != SunObstructionMode::Off && SunObstructionFrameCurrent (frame, shown) && frame->error.empty ();
    // The blockers are highlighted as a selection is, in the obstruction colour.
    // Solar cut has its own removed/kept boundary. A second whole-object pink
    // wire paints the uncut interior edges back over that sheet.
    scene.SetObstructionHighlight (current && frame->mode != SunObstructionMode::SolarCut ? frame->blockerGuids
                                                                                          : kNoBlockers);
    if (!current) {
        scene.DrawObstructionOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, 0,
                                      modelIsDrawn);
        return;
    }
    // Each stage of a staged cut re-uploads; so does each clock step shown.
    const uint64_t stageKey = frame->generation * 4 + uint64_t (frame->cutStage);
    if (frame->mode != SunObstructionMode::SolarCut || frame->cutStage != SunSolarCutStage::Done) {
        scene.DrawObstructionOverlay (context, viewProj, width, height, colorFormat, depthFormat, &frame->drawables,
                                      stageKey * 2048, modelIsDrawn);
        return;
    }
    // A finished cut, composed from what the panel shows (#168), once per
    // change rather than per frame, over the cut's own layers:
    //   Sun hours -- the study cells the cut changes, in the hours they would
    //     read after it, and/or their outline;
    //   Shadow at clock -- the lit surface shaded by the minutes it gains, or
    //     only what it lights at the day diagram's clock step.
    const bool hours = state.sunCutShowsHours;
    const bool lit = !hours && state.sunCutShowsGain && frame->exposure.error.empty ();
    const bool stepOnly = lit && state.sunCutStepOnly && !frame->exposure.steps.empty ();
    const int minute = stepOnly && state.sunStep >= 0 && size_t (state.sunStep) < state.sunStepMinutes.size ()
                           ? int (state.sunStepMinutes[size_t (state.sunStep)])
                           : -1;
    const bool fill = hours && state.sunCutShowsCells, outline = hours && state.sunCutShowsOutline;
    const SunStudyViewSettings view = SunStudyViewOf (state);
    // The cells' gradient: the hours map's own unless one is chosen for them.
    const bool ownCells = !state.sunCutCellsPreset.empty ();
    const std::vector<overlaylayers::ColourStop> cellRamp =
        ownCells
            ? SunStudyRampStops (state.sunCutCellsPreset, state.sunCutCellsCustom, state.sunCutCellsFlipped)
            : std::vector<overlaylayers::ColourStop> (view.ramp, view.ramp + size_t ((std::max) (0, view.rampCount)));
    const std::vector<overlaylayers::ColourStop> gainRamp =
        state.sunCutGainPreset.empty ()
            ? std::vector<overlaylayers::ColourStop> ()
            : SunStudyRampStops (state.sunCutGainPreset, state.sunCutGainCustom, state.sunCutGainFlipped);
    SolarCutLitColours colours;
    colours.gain = gainRamp;
    colours.edge = state.sunCutLitEdgeRgba;
    colours.step = state.sunCutStepRgba;
    uint64_t key = 1469598103934665603ull;
    Mix (key, &stageKey, sizeof (stageKey));
    for (const bool flag : { hours, lit, stepOnly, fill, outline })
        Mix (key, &flag, sizeof (flag));
    Mix (key, &minute, sizeof (minute));
    Mix (key, cellRamp.data (), sizeof (cellRamp[0]) * cellRamp.size ());
    Mix (key, gainRamp.data (), sizeof (gainRamp[0]) * gainRamp.size ());
    Mix (key, &view.bandCount, sizeof (view.bandCount));
    Mix (key, view.bandEdges, sizeof (view.bandEdges));
    Mix (key, &view.blueThreshold, sizeof (view.blueThreshold));
    Mix (key, &view.lowSunRgba, sizeof (view.lowSunRgba));
    for (const uint32_t rgba : { state.sunCutOutlineRgba, state.sunCutLitEdgeRgba, state.sunCutStepRgba })
        Mix (key, &rgba, sizeof (rgba));
    key = (key << 1) | 1u; // odd: never a staged (stageKey * 2048) key
    static uint64_t composedKey = 0;
    static GhPreviewDrawables composed;
    if (composedKey != key) {
        composed = frame->cutDrawables;
        if (stepOnly)
            AppendSolarExposureStep (frame->exposure, minute, composed, colours);
        else if (lit)
            AppendSolarExposure (frame->exposure, composed, colours);
        if (fill || outline) {
            const int bands = view.bandCount;
            const float* edges = bands >= kSunBandsMin ? view.bandEdges : nullptr;
            AppendSolarCutCells (
                frame->cutCells,
                [&] (float h) {
                    return SunStudyHoursRgba (cellRamp, h, bands, edges, view.blueThreshold, view.lowSunRgba);
                },
                composed, fill, outline, state.sunCutOutlineRgba);
        }
        composedKey = key;
    }
    scene.DrawObstructionOverlay (context, viewProj, width, height, colorFormat, depthFormat, &composed, key,
                                  modelIsDrawn);
}

void UpdateAndDrawAnalysisOverlays (DiligentScene& scene, Diligent::IDeviceContext* context, HudState& state,
                                    const float viewProj[16], uint32_t width, uint32_t height, uint32_t colorFormat,
                                    uint32_t depthFormat, bool modelIsDrawn, const Camera& camera)
{
    UpdateAndDrawAnalysisOverlay (scene, context, state, viewProj, width, height, colorFormat, depthFormat,
                                  modelIsDrawn, camera);
    UpdateAndDrawObstructionOverlay (scene, context, state, viewProj, width, height, colorFormat, depthFormat,
                                     modelIsDrawn);
    // The sun path last: built by last frame's HUD (DrawSunStudySunPath), which
    // runs after the scene. Its key is 0 whenever the path is hidden.
    scene.DrawSunPathOverlay (context, viewProj, width, height, colorFormat, depthFormat, state.sunPathScene.get (),
                              state.sunPathSceneKey, modelIsDrawn);
}

} // namespace geomsrv::archviz
