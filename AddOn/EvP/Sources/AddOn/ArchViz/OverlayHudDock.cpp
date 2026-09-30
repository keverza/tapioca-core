// ArchViz/OverlayHudDock -- the dock and the text size: the engine's (OverlayHudEngine.hpp),
// apart from the frame and the panels. See OverlayHud.hpp's notes on the dock and the
// text size.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace {

std::string Percent (float scale)
{
    return std::to_string (int (std::lround (scale * 100.0f))) + " %";
}

} // namespace

// ⚠️ THE DOCK: a tab per titled panel down the view's right edge, half-way down, all
// as wide as the widest title (the user, 2026-09-29). Filled with the panel's accent
// while the panel is open; its card's own colours while the panel is in the dock. A
// press on it opens or closes the panel -- in this frame, as the dock is laid out
// before the panels.
void Engine::Impl::Dock (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys,
                         float scale, ImVec2 view)
{
    inset = 0.0f;
    std::vector<size_t> titled;
    for (size_t i = 0; i < panels.size (); ++i)
        if (!panels[i]->title.empty ())
            titled.push_back (i);
    if (titled.empty ())
        return;
    ImGui::SetNextWindowPos (ImVec2 (view.x, view.y * 0.5f), ImGuiCond_Always, ImVec2 (1.0f, 0.5f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0.0f, kDockSpacing * scale));
    ImGui::PushFont (font, kDockFontPixels * scale);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
    const bool shown = ImGui::Begin ("###tapioca.dock", nullptr, flags);
    windows.back () = ImGui::GetCurrentWindow ();
    if (shown) {
        float widest = 0.0f;
        for (const size_t i : titled)
            widest = (std::max) (widest, ImGui::CalcTextSize (panels[i]->title.c_str ()).x);
        const ImVec2 size (widest + 2.0f * kDockPadding[0] * scale,
                           ImGui::GetFontSize () + 2.0f * kDockPadding[1] * scale);
        for (const size_t i : titled) {
            PanelState& state = StateOf (keys[i], *panels[i]);
            ImGui::PushID (keys[i].c_str ());
            if (items::DockButton ("##tab", panels[i]->title, *panels[i], !state.collapsed, size,
                                   ImDrawFlags_RoundCornersLeft, scale)) {
                state.collapsed = !state.collapsed;
                Docking (*panels[i], keys[i], state.collapsed);
            }
            ImGui::PopID ();
        }
        FontButtons (*panels[titled.front ()], size, scale);
    }
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleVar (3);
    if (windows.back () != nullptr)
        inset = windows.back ()->Size.x + kDockGap * scale;
}

// A panel sent to the dock or opened from it: said.
void Engine::Impl::Docking (const layers::Panel& panel, const std::string& panelKey, bool docked)
{
    changes.push_back (
        { "dock", panelKey, panel.title, std::string (), -1, docked ? 1.0 : 0.0, docked ? "docked" : "open", true });
}

// The text size under the tabs: smaller and larger, side by side, in the first titled
// panel's colours; pointed at, the size they give, beside the dock.
void Engine::Impl::FontButtons (const layers::Panel& colours, ImVec2 tab, float scale)
{
    uint32_t& step = store->fontStep;
    const uint32_t was = step;
    const float gap = kDockSpacing * scale;
    const ImVec2 half ((tab.x - gap) * 0.5f, tab.y);
    ImGui::PushID ("tapioca.font");
    const float top = ImGui::GetCursorScreenPos ().y;
    if (items::DockButton ("##smaller", "A\xE2\x88\x92", colours, false, half, ImDrawFlags_RoundCornersLeft, scale) &&
        step > 0)
        --step;
    const bool pointed = ImGui::IsItemHovered ();
    ImGui::SameLine (0.0f, gap);
    if (items::DockButton ("##larger", "A+", colours, false, half, ImDrawFlags_RoundCornersNone, scale) &&
        step + 1 < kFontStepCount)
        ++step;
    if (pointed || ImGui::IsItemHovered ()) {
        const std::string text = "Text size " + Percent (kFontSteps[step]);
        ImGui::SetNextWindowPos (ImVec2 (ImGui::GetWindowPos ().x - 6.0f * scale, top + tab.y * 0.5f), ImGuiCond_Always,
                                 ImVec2 (1.0f, 0.5f));
        if (ImGui::BeginTooltip ()) {
            ImGui::TextUnformatted (text.c_str ());
            ImGui::EndTooltip ();
        }
    }
    if (step != was)
        changes.push_back ({ "fontScale", std::string (), std::string (), "textSize", -1, double (kFontSteps[step]),
                             Percent (kFontSteps[step]), true });
    ImGui::PopID ();
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
