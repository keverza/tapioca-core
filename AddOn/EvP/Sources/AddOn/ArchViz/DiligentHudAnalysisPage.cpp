// Shared Analysis presentation, independent of the native Selection/Debug owners.
#include "ArchViz/DiligentHudShell.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include <imgui.h>

namespace geomsrv::archviz::viewerhud {
void SunStudyPage (HudState& state, const DiligentSceneStats& scene)
{
    // ⚠️ THE TAB BAR ONLY REPORTS AND OBEYS THE FOCUS (AnalysisTabs.hpp). Which
    // study arrived, and so which tab to select, is decided once a frame by
    // ApplyAnalysisView, whichever page is open; a clear never selects a tab.
    auto& focus = state.analysisFocus;
    const auto flags = [&focus] (int tab) {
        return focus.selectTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (!hudshell::BeginSubTabs ("##analysis-study"))
        return;
    if (ImGui::BeginTabItem ("Sun study", nullptr, flags (analysistabs::kSunTab))) {
        analysistabs::Opened (focus, analysistabs::kSunTab);
        state.viewpointPlacing = false;
        state.viewpointPlacingTarget = false;
        state.viewpointPickingContext = false;
        DrawSunStudyHudSection (state, scene);
        ImGui::EndTabItem ();
    }
    if (ImGui::BeginTabItem ("Surface visibility", nullptr, flags (analysistabs::kSurfaceVisibilityTab))) {
        analysistabs::Opened (focus, analysistabs::kSurfaceVisibilityTab);
        state.viewpointPlacing = false;
        state.viewpointPickingContext = false;
        DrawVisibilityHudSection (state, scene, 0);
        ImGui::EndTabItem ();
    }
    if (ImGui::BeginTabItem ("Point visibility", nullptr, flags (analysistabs::kPointVisibilityTab))) {
        analysistabs::Opened (focus, analysistabs::kPointVisibilityTab);
        state.viewpointPickingContext = false;
        DrawVisibilityHudSection (state, scene, 1);
        ImGui::EndTabItem ();
    }
    if (ImGui::BeginTabItem ("Viewpoint", nullptr, flags (analysistabs::kViewpointTab))) {
        analysistabs::Opened (focus, analysistabs::kViewpointTab);
        DrawViewpointHudSection (state);
        ImGui::EndTabItem ();
    }
    hudshell::EndSubTabs ();
}
} // namespace geomsrv::archviz::viewerhud
