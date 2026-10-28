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
    analysistabs::FollowShownStudy (state.analysisFocus, shown, kind,
                                    SceneCmdQueue::Get ().AnalysisDisplayGeneration (), origin);
    SunStudyViewSettings view = SunStudyViewOf (state);
    view.suppressed = !analysistabs::ShowsTint (state.analysisFocus, shown, kind);
    scene.SetSunStudyView (view);
}

void UpdateAndDrawAnalysisOverlay (DiligentScene& scene, Diligent::IDeviceContext* context, const HudState& state,
                                   const float viewProj[16], uint32_t width, uint32_t height, uint32_t colorFormat,
                                   uint32_t depthFormat, bool modelIsDrawn)
{
    const auto settings = viewpointstudy::GetSettings ();
    if (state.readOnly || !settings->enabled || !settings->pointValid ||
        state.analysisFocus.tab != analysistabs::kViewpointTab) {
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, 0,
                                   modelIsDrawn);
        return;
    }
    const auto status = viewpointstudy::GetStatus ();
    const ViewpointOverlayStyle style;
    // Rebuilt only when what it draws changes; the camera is the layer's viewProj.
    uint64_t key = 1469598103934665603ull;
    const void* result = status.result.get ();
    Mix (key, &result, sizeof (result));
    Mix (key, settings->options.point.data (), sizeof (double) * 3);
    Mix (key, &settings->options.radius, sizeof (settings->options.radius));
    Mix (key, &style, sizeof (style));
    key |= 1u; // never the "hidden" key
    if (key == scene.AnalysisOverlayKey ()) {
        scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, nullptr, key,
                                   modelIsDrawn);
        return;
    }
    const GhPreviewDrawables drawables =
        BuildViewpointOverlay (settings->options.point, settings->options.radius, status.result.get (), style);
    scene.DrawAnalysisOverlay (context, viewProj, width, height, colorFormat, depthFormat, &drawables, key,
                               modelIsDrawn);
}

} // namespace geomsrv::archviz
