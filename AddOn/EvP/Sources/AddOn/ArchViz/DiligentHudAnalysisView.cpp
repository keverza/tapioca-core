// ArchViz/DiligentHudAnalysisView -- the HUD's per-frame word to the scene about
// the analysis tint: which Analysis tab owns the resident study (AnalysisTabs.hpp),
// and so whether the tint draws at all. Runs every frame, whichever HUD page is
// open, so a study a command runs is followed even while the Analysis page is shut.

#include "ArchViz/AnalysisTabs.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "ArchViz/ViewpointOverlayGeometry.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "Geometry/MeshStore.hpp"

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
                                   uint32_t depthFormat, bool modelIsDrawn)
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
                                       modelIsDrawn);
            return;
        }
        GhPreviewDrawables drawables = BuildPointVisibilityGuides (guide);
        if (state.analysisPath != nullptr)
            AppendPolyline (state.analysisPath->points, kAnalysisPathRgba, drawables.depthTested);
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, &drawables, key,
                                   modelIsDrawn);
        return;
    }
    const auto settings = viewpointstudy::GetSettings ();
    if (state.readOnly || !settings->enabled || !settings->pointValid ||
        state.analysisFocus.tab != analysistabs::kViewpointTab) {
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, 0,
                                   modelIsDrawn);
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
                                   modelIsDrawn);
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
                               modelIsDrawn);
}

} // namespace geomsrv::archviz
