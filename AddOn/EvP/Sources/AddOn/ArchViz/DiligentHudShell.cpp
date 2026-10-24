// ArchViz/DiligentHudShell -- see the header.

#include "ArchViz/DiligentHudShell.hpp"

#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/SurfaceSwitch.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow: the dock's width, for the view's right column

#include <cmath>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace viewerhud {

namespace {

namespace layers = overlaylayers;

// The overlays' plain card, wider: the viewer's pages hold the render settings' labelled
// sliders, which the overlays' 220 px leaves no room to name.
const layers::Panel& ViewerLook ()
{
    static const layers::Panel panel = [] () {
        layers::Panel look = hudshell::PlainLook ();
        look.widthPixels = 280.0f;
        return look;
    }();
    return panel;
}

// The viewer's own circle: busy while it still reads the model, neutral once it holds it.
hudshell::Circle ViewerCircle (const DiligentSceneStats& scene)
{
    hudshell::Circle circle;
    circle.active = true;
    if (scene.pending > 0) {
        circle.phase = hudshell::Phase::Busy;
        circle.tip = "The viewer is reading the model: " + std::to_string (scene.pending) + " element(s) queued";
    }
    else {
        circle.phase = hudshell::Phase::Ready;
        circle.tip = "The separate viewer";
    }
    return circle;
}

} // namespace

void Draw (Shell& shell, HudState& state, const DiligentSceneStats& scene, uint32_t width, uint32_t height,
           ImFont* font, const Frame& frame)
{
    const ImVec2 view = ImVec2 (float (width), float (height));
    const float scale = frame.dpiScale;
    const float ui = scale * hudshell::FontScaleOfStep (shell.fontStep);
    const layers::Panel& look = ViewerLook ();
    // Keep command/step metadata current even with the Analysis page closed.
    state.sunStepCount = scene.sunStudy.stepCount;
    state.sunStepMinutes = scene.sunStudy.stepMinutes;
    if (scene.sunStudy.debugMode != state.sunSeenCommandedMode) {
        state.sunSeenCommandedMode = scene.sunStudy.debugMode;
        state.sunView = 0;
    }

    // Analysis stays discoverable before the first run.
    std::vector<hudshell::HostTab> tabs = { { hudshell::kStatsKey, "Stats" },
                                            { hudshell::kSelectionKey, "Selection" } };
    tabs.push_back ({ kSunStudyKey, "Analysis", {}, archviz::ui::IconId::Glasses });
    tabs.push_back ({ hudshell::kSettingsKey, "Settings" });
    // The console's errors and warnings not shown yet, counted on the tab (HudConsole.hpp).
    const uint32_t unseen = hudconsole::Unseen (hudconsole::Entries (), shell.consoleSeen, shell.consoleCleared);
    tabs.push_back ({ hudshell::kDebugKey, "Debug", unseen > 0 ? std::to_string (unseen) : std::string () });
    bool held = false;
    for (const hudshell::HostTab& tab : tabs)
        held = held || tab.key == shell.held;
    if (!held)
        shell.held = hudshell::kStatsKey;

    hudshell::Circle overlay;
    overlay.tip = "The overlay: press to switch to it (the viewer closes)";
    const hudshell::Circle viewer = ViewerCircle (scene);
    const auto dock =
        hudshell::Dock (shell.dock, view, scale, ui, font,
                        shell.navigation.workflowTitle.empty () ? "Tapioca" : shell.navigation.workflowTitle, look,
                        shell.open, overlay, &viewer);
    if (dock.press.top)
        surfaceswitch::Request (surfaceswitch::Surface::Overlay);
    if (dock.press.title)
        shell.open = true;
    if (dock.press.collapse)
        shell.open = !shell.open;

    // ---- the floating panel -------------------------------------------------------------------
    if (!shell.open)
        return;
    hudshell::HostSpec spec;
    spec.look = &look;
    spec.font = font;
    spec.scale = scale;
    spec.ui = ui;
    spec.view = view;
    spec.attached = true;
    spec.dockMin = dock.window->Pos;
    spec.dockMax = ImVec2 (dock.window->Pos.x + dock.window->Size.x, dock.window->Pos.y + dock.window->Size.y);
    spec.dockEdge = shell.dock.edge;
    spec.dockHorizontal = shell.dock.horizontal;
    spec.tabs = tabs;
    const auto page = [&] (const std::string& key) {
        // ⚠️ A CONTROL LEAVES ITS LABEL HALF THE ROW: ImGui's own width in an auto-sized window
        // is 16 em -- the whole panel -- and every label after a slider was cut off.
        ImGui::PushItemWidth (std::floor (ImGui::GetContentRegionAvail ().x * 0.5f));
        if (key == hudshell::kSelectionKey)
            SelectionPage (shell, state, ui);
        else if (key == kSunStudyKey)
            SunStudyPage (state, scene);
        else if (key == hudshell::kSettingsKey)
            SettingsPage (shell, state, scene);
        else if (key == hudshell::kDebugKey)
            DebugPage (shell, state, scene, frame, width, height, ui);
        else
            StatsPage (state, scene, ui);
        ImGui::PopItemWidth ();
    };
    const hudshell::HostResult result =
        hudshell::Host (spec, shell.held, shell.shownLast, shell.placement, page, nullptr, &shell.navigation);
    if (!result.pressed.empty ())
        shell.held = result.pressed;
    if (result.closed)
        shell.open = false;
    // The dock over everything: a panel dragged onto it never hides it.
    if (ImGuiWindow* const dock = ImGui::FindWindowByName ("###tapioca.dock"); dock != nullptr)
        ImGui::BringWindowToDisplayFront (dock);
}

} // namespace viewerhud
} // namespace archviz
} // namespace geomsrv
