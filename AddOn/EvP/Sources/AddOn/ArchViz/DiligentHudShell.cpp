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

std::string TitleOf (const std::string& key)
{
    if (key == hudshell::kSelectionKey)
        return "Selection";
    if (key == hudshell::kSettingsKey)
        return "Settings";
    if (key == hudshell::kDebugKey)
        return "Debug";
    if (key == kSunStudyKey)
        return "Analysis";
    return "Stats";
}

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
    tabs.push_back ({ kSunStudyKey, "Analysis" });
    tabs.push_back ({ hudshell::kSettingsKey, "Settings" });
    // The console's errors and warnings not shown yet, counted on the tab (HudConsole.hpp).
    const uint32_t unseen = hudconsole::Unseen (hudconsole::Entries (), shell.consoleSeen, shell.consoleCleared);
    tabs.push_back ({ hudshell::kDebugKey, unseen > 0 ? "Debug (" + std::to_string (unseen) + ")" : "Debug" });
    bool held = false;
    for (const hudshell::HostTab& tab : tabs)
        held = held || tab.key == shell.held;
    if (!held)
        shell.held = hudshell::kStatsKey;

    // ---- the dock: at the view's right edge, half-way down -----------------------------------
    // ⚠️ THE OVERLAYS' DOCK (OverlayHudHost.cpp), the viewer's circle the shown one here: the
    // overlay's, pressed, switches to the overlay of the view in front.
    ImGui::SetNextWindowPos (ImVec2 (view.x, std::floor (view.y * 0.5f)), ImGuiCond_Always, ImVec2 (1.0f, 0.5f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0.0f, 0.0f));
    ImGui::PushFont (font, hudshell::kDockFontPixels * ui);
    const ImGuiWindowFlags dockFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
    float inset = 0.0f;
    if (ImGui::Begin ("###tapioca.dock", nullptr, dockFlags)) {
        hudshell::Circle overlay;
        overlay.tip = "The overlay: press to switch to it (the viewer closes)";
        const hudshell::Circle viewer = ViewerCircle (scene);
        const hudshell::DockPress press =
            hudshell::DockTab ("##hud", TitleOf (shell.held), look, shell.open, overlay, &viewer,
                               ImVec2 (hudshell::kDockPadding[0] * ui, hudshell::kDockPadding[1] * ui), ui);
        if (press.top)
            surfaceswitch::Request (surfaceswitch::Surface::Overlay);
        else if (press.title)
            shell.open = !shell.open;
    }
    inset = ImGui::GetCurrentWindow ()->Size.x + hudshell::kDockGap * ui;
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleVar (3);

    // ---- the floating panel -------------------------------------------------------------------
    if (!shell.open)
        return;
    hudshell::HostSpec spec;
    spec.look = &look;
    spec.font = font;
    spec.scale = scale;
    spec.ui = ui;
    spec.view = view;
    spec.inset = inset;
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
        hudshell::Host (spec, shell.held, shell.shownLast, shell.placement, page, nullptr);
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
