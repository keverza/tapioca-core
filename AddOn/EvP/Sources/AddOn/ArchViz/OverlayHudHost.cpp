// ArchViz/OverlayHudHost -- the floating panel the titled panels are tabs of, the dock's one
// tab that opens and closes it, and the text size: the engine's (OverlayHudEngine.hpp),
// apart from the frame and the panels. See OverlayHud.hpp's notes on the host.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <algorithm>
#include <cfloat>
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

// The host's window: one name in every context, whatever tab it shows.
constexpr char kHostName[] = "###tapioca.hud";

// The size buttons and the close button at the end of the tab row.
constexpr char kSmaller[] = "A\xE2\x88\x92##tapioca.smaller";
constexpr char kLarger[] = "A+##tapioca.larger";
constexpr char kClose[] = "\xC3\x97##tapioca.close";

// What a pointed size button gives, under it.
void SizeTip (float scale, float ui)
{
    if (!ImGui::IsItemHovered ())
        return;
    ImGui::SetNextWindowPos (ImVec2 (ImGui::GetItemRectMin ().x, ImGui::GetItemRectMax ().y + 4.0f * ui),
                             ImGuiCond_Always);
    if (ImGui::BeginTooltip ()) {
        ImGui::TextUnformatted (("Text size " + Percent (scale)).c_str ());
        ImGui::EndTooltip ();
    }
}

} // namespace

void Engine::Impl::Gather (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys)
{
    titled.clear ();
    for (size_t i = 0; i < panels.size (); ++i)
        if (!panels[i]->title.empty ())
            titled.push_back (i);
    if (titled.empty ())
        return;
    State::Host& host = store->host;
    // The first panels ever: the host starts as the first of them asks, open or docked.
    if (!host.known) {
        host.known = true;
        host.open = !panels[titled.front ()]->collapsed;
    }
    // The tab held is one of them: the first, when the one held is gone.
    shown = titled.front ();
    for (const size_t i : titled)
        if (keys[i] == host.selected)
            shown = i;
    host.selected = keys[shown];
}

// ⚠️ ONE TAB, ITS TITLE TURNED (the user, 2026-09-30: the side tabs too large -- one tab,
// its text rotated 90 degrees, that opens and closes the panel). At the view's right edge,
// half-way down; it says the tab the host shows, filled with its accent while the host is
// open. Laid out before the host, so a press opens or closes it in this frame -- and drawn
// over it, so a host dragged there never hides it.
void Engine::Impl::Dock (const std::vector<const layers::Panel*>& panels, float scale, ImVec2 view)
{
    inset = 0.0f;
    if (titled.empty ())
        return;
    const layers::Panel& panel = *panels[shown];
    ImGui::SetNextWindowPos (ImVec2 (view.x, std::floor (view.y * 0.5f)), ImGuiCond_Always, ImVec2 (1.0f, 0.5f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushFont (font, kDockFontPixels * scale);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBackground;
    const bool drawn = ImGui::Begin ("###tapioca.dock", nullptr, flags);
    ImGuiWindow* const window = ImGui::GetCurrentWindow ();
    windows[windows.size () - 2] = window;
    if (drawn && items::VerticalTab ("##hud", panel.title, panel, store->host.open,
                                     ImVec2 (kDockPadding[0] * scale, kDockPadding[1] * scale), scale)) {
        store->host.open = !store->host.open;
        Opening (store->host.open, panel.title);
    }
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleVar (2);
    inset = window->Size.x + kDockGap * scale;
}

void Engine::Impl::Opening (bool open, const std::string& title)
{
    changes.push_back (
        { "hud", store->host.selected, title, std::string (), -1, open ? 1.0 : 0.0, open ? "open" : "closed", true });
}

// ⚠️ ONE FLOATING PANEL, THE TITLED PANELS ITS TABS (the user, 2026-09-30: the STUDY panel's
// design as the main one, its tabs switching between the studies; floating, for the user
// to place anywhere in the view; a small, dense inspection panel). No title bar: the tab
// row is its head, the text size and the close button at its end. It moves where it is
// dragged by anything that is not a control -- the tab row's empty end, its background.
void Engine::Impl::Host (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys,
                         float scale, float ui, ImVec2 view)
{
    State::Host& host = store->host;
    if (titled.empty () || !host.open)
        return;
    const layers::Panel& panel = *panels[shown];
    ImGuiWindow* const existing = ImGui::FindWindowByName (kHostName);
    const ImGuiContext& g = *ImGui::GetCurrentContext ();
    const bool moving = existing != nullptr && g.MovingWindow != nullptr && g.MovingWindow->RootWindow == existing;
    // ⚠️ NOT PLACED WHILE IT IS DRAGGED: ImGui moved it before this Begin, and a place set
    // now would put it back under the pointer's start.
    if (!moving) {
        if (host.placed) {
            const ImVec2 size = existing != nullptr ? existing->Size : ImVec2 (0.0f, 0.0f);
            const bool right = (host.corner & 1u) != 0, bottom = (host.corner & 2u) != 0;
            float x = right ? view.x - host.offset[0] * scale - size.x : host.offset[0] * scale;
            float y = bottom ? view.y - host.offset[1] * scale - size.y : host.offset[1] * scale;
            // Inside the view, however it was resized.
            x = (std::min) ((std::max) (x, 0.0f), (std::max) (view.x - size.x, 0.0f));
            y = (std::min) ((std::max) (y, 0.0f), (std::max) (view.y - size.y, 0.0f));
            ImGui::SetNextWindowPos (ImVec2 (std::floor (x), std::floor (y)), ImGuiCond_Always);
        }
        else {
            // Where the tab it shows asks to be, as an untitled panel is placed.
            const int column = int (panel.anchor) % 3, row = int (panel.anchor) / 3;
            const ImVec2 pivot (float (column) * 0.5f, float (row) * 0.5f);
            const float inwardX = column == 2 ? -1.0f : 1.0f, inwardY = row == 2 ? -1.0f : 1.0f;
            ImGui::SetNextWindowPos (
                ImVec2 (pivot.x * view.x + inwardX * panel.offsetPixels[0] * scale - (column == 2 ? inset : 0.0f),
                        pivot.y * view.y + inwardY * panel.offsetPixels[1] * scale),
                ImGuiCond_Always, pivot);
        }
    }
    const int colours = PushPanelStyle (panel, ui);
    ImFont* const face = FontFor (panel.font);
    ImGui::PushFont (face, panel.sizePixels * ui);
    // As wide as its tab row needs, or the panel's width when that is wider.
    const ImGuiStyle& style = ImGui::GetStyle ();
    float row = 2.0f * style.WindowPadding.x;
    for (const size_t i : titled)
        row +=
            ImGui::CalcTextSize (panels[i]->title.c_str ()).x + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x;
    for (const char* button : { kSmaller, kLarger, kClose })
        row += ImGui::CalcTextSize (button, nullptr, true).x + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x;
    const float width = (std::max) (std::ceil (row), panel.widthPixels * ui);
    ImGui::SetNextWindowSizeConstraints (ImVec2 (width, 0.0f),
                                         ImVec2 (panel.widthPixels > 0.0f ? width : FLT_MAX, FLT_MAX));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse;
    const bool drawn = ImGui::Begin (kHostName, nullptr, flags);
    ImGuiWindow* const window = ImGui::GetCurrentWindow ();
    windows.back () = window;
    bool close = false;
    if (drawn && ImGui::BeginTabBar ("##hud")) {
        std::string now;
        for (const size_t i : titled) {
            const std::string& k = keys[i];
            // ⚠️ THE HELD TAB ASKED FOR ONLY WHERE THIS CONTEXT SHOWED ANOTHER, as a panel's
            // own tab bar (OverlayHudControls.cpp): asked every frame, ImGui applies it over
            // the user's click.
            const ImGuiTabItemFlags asked =
                k == host.selected && shownHost != host.selected ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem ((panels[i]->title + "###" + k).c_str (), nullptr, asked)) {
                now = k;
                key = k;
                layer = k.substr (0, k.rfind ('#'));
                Items (*panels[i], StateOf (k, *panels[i]), ui);
                ImGui::EndTabItem ();
            }
        }
        uint32_t& step = store->fontStep;
        const uint32_t was = step;
        const ImGuiTabItemFlags end = ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip;
        if (ImGui::TabItemButton (kSmaller, end) && step > 0)
            --step;
        SizeTip (kFontSteps[step], ui);
        if (ImGui::TabItemButton (kLarger, end) && step + 1 < kFontStepCount)
            ++step;
        SizeTip (kFontSteps[step], ui);
        close = ImGui::TabItemButton (kClose, end);
        ImGui::EndTabBar ();
        if (step != was)
            changes.push_back ({ "fontScale", std::string (), std::string (), "textSize", -1, double (kFontSteps[step]),
                                 Percent (kFontSteps[step]), true });
        // The user's press on another tab: one this context did not show last frame, and
        // not the held one it was asked to show.
        if (!now.empty () && !shownHost.empty () && now != shownHost && now != host.selected) {
            host.selected = now;
            for (const size_t i : titled)
                if (keys[i] == now)
                    changes.push_back (
                        { "panel", now, panels[i]->title, std::string (), -1, 1.0, panels[i]->title, true });
        }
        shownHost = now;
    }
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleColor (colours);
    ImGui::PopStyleVar (kStyleVars);
    // Dragged: where to, from the view's corner nearest it, in logical pixels.
    if (moving) {
        const ImVec2 pos = window->Pos, size = window->Size;
        const bool right = pos.x + size.x * 0.5f > view.x * 0.5f, bottom = pos.y + size.y * 0.5f > view.y * 0.5f;
        host.placed = true;
        host.corner = uint8_t ((right ? 1u : 0u) | (bottom ? 2u : 0u));
        host.offset[0] = (std::max) (right ? view.x - pos.x - size.x : pos.x, 0.0f) / scale;
        host.offset[1] = (std::max) (bottom ? view.y - pos.y - size.y : pos.y, 0.0f) / scale;
    }
    if (close) {
        host.open = false;
        Opening (false, panel.title);
    }
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
