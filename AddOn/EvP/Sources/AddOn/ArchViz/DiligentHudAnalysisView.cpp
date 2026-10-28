// ArchViz/DiligentHudAnalysisView -- the HUD's per-frame word to the scene about
// the analysis tint: which Analysis tab owns the resident study (AnalysisTabs.hpp),
// and so whether the tint draws at all. Runs every frame, whichever HUD page is
// open, so a study a command runs is followed even while the Analysis page is shut.

#include "ArchViz/AnalysisTabs.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "ArchViz/VisibilityStudyController.hpp"

namespace geomsrv::archviz {

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

} // namespace geomsrv::archviz
