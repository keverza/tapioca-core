// ArchViz/OverlayHudHost -- the floating panel the titled panels are tabs of, its Settings,
// and the dock's one tab that opens and closes it and shows and hides the whole overlay:
// the engine's (OverlayHudEngine.hpp), apart from the frame and the panels. See
// OverlayHud.hpp's notes on the host.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <algorithm>
#include <cctype>
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
// The close button at the end of the tab row.
constexpr char kClose[] = "\xC3\x97##tapioca.close";
// The built-in tab, after the panels'.
constexpr char kSettingsTab[] = "Settings###tapioca.settings";

// The look the host takes when it shows no layer's panel -- Settings, with no titled panel to
// borrow from: the light card, small.
const layers::Panel& PlainPanel ()
{
    static const layers::Panel panel = [] () {
        layers::Panel plain;
        layers::ApplyTheme (plain, layers::PanelTheme::Light);
        plain.title = "Overlay";
        plain.sizePixels = 13.0f;
        plain.widthPixels = 220.0f;
        return plain;
    }();
    return panel;
}

std::string LayerOf (const std::string& key)
{
    return key.substr (0, key.rfind ('#'));
}

// A layer's name as Settings says it: the add-on's own (`tapioca.storeySlices`) in words
// ("Storey slices"), a caller's as the caller named it.
std::string Readable (const std::string& name)
{
    if (!layers::Reserved (name))
        return name;
    std::string out;
    for (const char c : name.substr (std::char_traits<char>::length (layers::kReservedPrefix))) {
        if (std::isupper (static_cast<unsigned char> (c)) != 0) {
            out += ' ';
            out += char (std::tolower (static_cast<unsigned char> (c)));
        }
        else {
            out += c;
        }
    }
    if (!out.empty ())
        out[0] = char (std::toupper (static_cast<unsigned char> (out[0])));
    return out;
}

} // namespace

void Engine::Impl::Gather (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys)
{
    titled.clear ();
    for (size_t i = 0; i < panels.size (); ++i)
        if (!panels[i]->title.empty () && LayerShown (*store, LayerOf (keys[i])))
            titled.push_back (i);
    // Any layer drawn here -- shown or hidden -- is a reason for the dock: its circle shows
    // and hides them, Settings lists them.
    present = !titled.empty () || !layerNames.empty ();
    showsPanel = false;
    look = &PlainPanel ();
    if (!present)
        return;
    State::Host& host = store->host;
    // The first titled panel ever says how the host starts, open or closed; before one,
    // it is closed (HudOpen) -- a layer of lines alone does not open a panel.
    if (!host.known && !titled.empty ()) {
        host.known = true;
        host.open = !panels[titled.front ()]->collapsed;
    }
    // The tab held is one of them, or Settings: the first panel, when the one held is gone.
    if (host.selected != kSettingsKey) {
        bool held = false;
        for (const size_t i : titled)
            held = held || keys[i] == host.selected;
        if (!held)
            host.selected = titled.empty () ? std::string (kSettingsKey) : keys[titled.front ()];
    }
    for (const size_t i : titled)
        if (keys[i] == host.selected) {
            shown = i;
            showsPanel = true;
        }
    // Settings takes the look of the first panel, when there is one.
    look = showsPanel ? panels[shown] : !titled.empty () ? panels[titled.front ()] : &PlainPanel ();
    showing = showsPanel ? panels[shown]->title : titled.empty () ? look->title : "Settings";
}

void Engine::Impl::ShowOverlay (bool shownNow)
{
    if (store->shown == shownNow)
        return;
    store->shown = shownNow;
    ++store->revision;
    changes.push_back ({ "overlay", std::string (), std::string (), std::string (), -1, shownNow ? 1.0 : 0.0,
                         shownNow ? "shown" : "hidden", true });
}

void Engine::Impl::ShowLayer (const std::string& name, bool shownNow)
{
    if (LayerShown (*store, name) == shownNow)
        return;
    SetLayerShown (*store, name, shownNow);
    changes.push_back ({ "layer", std::string (), std::string (), name, -1, shownNow ? 1.0 : 0.0,
                         shownNow ? "shown" : "hidden", true });
}

void Engine::Impl::SetHover (bool on)
{
    if (HoverMode (*store) == on)
        return;
    SetHoverMode (*store, on);
    changes.push_back (
        { "hover", std::string (), std::string (), std::string (), -1, on ? 1.0 : 0.0, on ? "on" : "off", true });
}

void Engine::Impl::SetOpen (bool open)
{
    const bool was = HudOpen (*store);
    store->host.known = true;
    store->host.open = open;
    if (was != open)
        Opening (open, showing);
}

void Engine::Impl::ShowSettings ()
{
    State::Host& host = store->host;
    if (host.selected != kSettingsKey) {
        host.selected = kSettingsKey;
        changes.push_back ({ "panel", std::string (), "Settings", std::string (), -1, 1.0, "Settings", true });
    }
    ShowOverlay (true);
    SetOpen (true);
}

void Engine::Impl::SetFontStep (uint32_t step)
{
    if (step >= kFontStepCount || step == store->fontStep)
        return;
    store->fontStep = step;
    changes.push_back ({ "fontScale", std::string (), std::string (), "textSize", -1, double (kFontSteps[step]),
                         Percent (kFontSteps[step]), true });
}

void Engine::Impl::ResetPosition ()
{
    if (!store->host.placed)
        return;
    store->host.placed = false;
    changes.push_back ({ "position", std::string (), std::string (), std::string (), -1, 0.0, "reset", true });
}

// ⚠️ ONE TAB, ITS TITLE TURNED, AND A CIRCLE (the user, 2026-09-30: one tab, its text rotated
// 90 degrees, that opens and closes the panel; on it, a filled or empty circle that shows
// and hides the whole overlay and its HUD without destroying them). At the view's right
// edge, half-way down; it says the tab the host shows. Laid out before the host, so a press
// takes effect in this frame -- and drawn over it, so a host dragged there never hides it.
// Hidden, it is all that is drawn; its title then shows everything again.
void Engine::Impl::Dock (const std::vector<const layers::Panel*>& panels, float scale, ImVec2 view)
{
    inset = 0.0f;
    if (!present)
        return;
    const layers::Panel& colours = *look;
    ImGui::SetNextWindowPos (ImVec2 (view.x, std::floor (view.y * 0.5f)), ImGuiCond_Always, ImVec2 (1.0f, 0.5f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0.0f, 0.0f));
    ImGui::PushFont (font, kDockFontPixels * scale);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBackground;
    const bool drawn = ImGui::Begin ("###tapioca.dock", nullptr, flags);
    ImGuiWindow* const window = ImGui::GetCurrentWindow ();
    windows[windows.size () - 2] = window;
    if (drawn) {
        const bool open = HudOpen (*store) && store->shown;
        bool toggled = false;
        const bool pressed =
            items::VerticalTab ("##hud", showing, colours, open, store->shown,
                                ImVec2 (kDockPadding[0] * scale, kDockPadding[1] * scale), scale, toggled);
        if (toggled) {
            ShowOverlay (!store->shown);
        }
        else if (pressed) {
            // Hidden, the title brings everything back, the panel open.
            ShowOverlay (true);
            SetOpen (!open);
        }
    }
    ImGui::End ();
    ImGui::PopFont ();
    ImGui::PopStyleVar (3);
    inset = window->Size.x + kDockGap * scale;
}

void Engine::Impl::Opening (bool open, const std::string& title)
{
    changes.push_back (
        { "hud", store->host.selected, title, std::string (), -1, open ? 1.0 : 0.0, open ? "open" : "closed", true });
}

// ⚠️ THE HUD'S OWN SETTINGS, NOT A LAYER'S (the user, 2026-09-30: a Settings tab instead of
// A- A+, for the HUD's style and the overlay's display). Small and dense, as a page is.
void Engine::Impl::Settings ()
{
    ImGui::SeparatorText ("HUD");
    if (ImGui::BeginTable ("##hudsettings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn ("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn ("##value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex (0);
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Text size");
        ImGui::TableSetColumnIndex (1);
        const uint32_t step = store->fontStep;
        ImGui::SetNextItemWidth (-FLT_MIN);
        if (ImGui::BeginCombo ("##textsize", Percent (kFontSteps[step]).c_str (), ImGuiComboFlags_HeightLargest)) {
            for (uint32_t k = 0; k < kFontStepCount; ++k)
                if (ImGui::Selectable (Percent (kFontSteps[k]).c_str (), k == step))
                    SetFontStep (k);
            ImGui::EndCombo ();
        }
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex (0);
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Position");
        ImGui::TableSetColumnIndex (1);
        ImGui::BeginDisabled (!store->host.placed);
        if (ImGui::Button ("Reset##position", ImVec2 (-FLT_MIN, 0.0f)))
            ResetPosition ();
        ImGui::EndDisabled ();
        ImGui::EndTable ();
    }
    ImGui::SeparatorText ("Overlay");
    bool all = store->shown;
    if (ImGui::Checkbox ("Show overlay##tapioca.shown", &all))
        ShowOverlay (all);
    // What is under the pointer -- a slice's figures, a heatmap's value -- by it.
    bool hover = HoverMode (*store);
    if (ImGui::Checkbox ("Hover readout##tapioca.hover", &hover))
        SetHover (hover);
    if (layerNames.empty ()) {
        ImGui::TextDisabled ("No layers");
        return;
    }
    // Each layer drawn here, shown or hidden: a hidden one draws nothing, its panels no tab.
    for (const std::string& name : layerNames) {
        bool on = LayerShown (*store, name);
        if (ImGui::Checkbox ((Readable (name) + "##layer." + name).c_str (), &on))
            ShowLayer (name, on);
    }
}

// ⚠️ ONE FLOATING PANEL, THE TITLED PANELS ITS TABS, SETTINGS LAST (the user, 2026-09-30: the
// STUDY panel's design as the main one, its tabs switching between the studies; floating,
// for the user to place anywhere in the view; a small, dense inspection panel). No title
// bar: the tab row is its head, the close button at its end. It moves where it is dragged
// by anything that is not a control -- the tab row's empty end, its background.
void Engine::Impl::Host (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys,
                         float scale, float ui, ImVec2 view)
{
    State::Host& host = store->host;
    if (!present || !HudOpen (*store) || !store->shown)
        return;
    const layers::Panel& panel = *look;
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
    for (const char* tab : { kSettingsTab, kClose })
        row += ImGui::CalcTextSize (tab, nullptr, true).x + 2.0f * style.FramePadding.x + style.ItemInnerSpacing.x;
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
        // ⚠️ THE HELD TAB ASKED FOR ONLY WHERE THIS CONTEXT SHOWED ANOTHER, as a panel's own
        // tab bar (OverlayHudControls.cpp): asked every frame, ImGui applies it over the
        // user's click.
        const auto asked = [&] (const std::string& k) {
            return k == host.selected && shownHost != host.selected ? ImGuiTabItemFlags_SetSelected : 0;
        };
        for (const size_t i : titled) {
            const std::string& k = keys[i];
            if (ImGui::BeginTabItem ((panels[i]->title + "###" + k).c_str (), nullptr, asked (k))) {
                now = k;
                key = k;
                layer = LayerOf (k);
                Items (*panels[i], StateOf (k, *panels[i]), ui);
                ImGui::EndTabItem ();
            }
        }
        if (ImGui::BeginTabItem (kSettingsTab, nullptr, asked (kSettingsKey))) {
            now = kSettingsKey;
            Settings ();
            ImGui::EndTabItem ();
        }
        close = ImGui::TabItemButton (kClose, ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
        ImGui::EndTabBar ();
        // The user's press on another tab: one this context did not show last frame, and
        // not the held one it was asked to show.
        if (!now.empty () && !shownHost.empty () && now != shownHost && now != host.selected) {
            host.selected = now;
            std::string title = "Settings";
            for (const size_t i : titled)
                if (keys[i] == now)
                    title = panels[i]->title;
            changes.push_back (
                { "panel", now == kSettingsKey ? std::string () : now, title, std::string (), -1, 1.0, title, true });
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
    if (close)
        SetOpen (false);
}

// ⚠️ THE HUD'S OWN MENU ON A RIGHT CLICK (the user, 2026-09-30: right click on the HUD to
// offer HUD specific options). One menu for the whole HUD -- the host, the dock, a panel --
// at the pointer, in the host's look; while it is open the whole view is the HUD's, as while
// a dropdown's list is (OverlaySceneScreen.cpp). Archicad's own menu never shows over the
// HUD: the input layer eats the button there, and the canvas's WM_CONTEXTMENU with it
// (OverlayInput.cpp).
void Engine::Impl::Menu (float ui)
{
    constexpr char kMenu[] = "##tapioca.menu";
    // ⚠️ NOTHING LEFT TO ACT ON, THE MENU GOES: left open, the view would stay the HUD's.
    if (!present) {
        if (ImGui::IsPopupOpen (kMenu))
            ImGui::ClosePopupsExceptModals ();
        return;
    }
    // Released over any of the HUD's windows -- the menu's own included: it opens again there.
    if (ImGui::IsMouseReleased (ImGuiMouseButton_Right) && ImGui::GetCurrentContext ()->HoveredWindow != nullptr)
        ImGui::OpenPopup (kMenu);
    const layers::Panel& panel = *look;
    const int colours = PushPanelStyle (panel, ui);
    ImGui::PushFont (FontFor (panel.font), panel.sizePixels * ui);
    if (ImGui::BeginPopup (kMenu, ImGuiWindowFlags_NoMove)) {
        const bool all = store->shown;
        const bool open = HudOpen (*store) && all;
        if (ImGui::MenuItem ("Show overlay", nullptr, all))
            ShowOverlay (!all);
        if (ImGui::MenuItem ("Show panel", nullptr, open)) {
            ShowOverlay (true);
            SetOpen (!open);
        }
        if (ImGui::MenuItem ("Settings"))
            ShowSettings ();
        if (ImGui::MenuItem ("Hover readout", nullptr, HoverMode (*store)))
            SetHover (!HoverMode (*store));
        ImGui::Separator ();
        if (ImGui::BeginMenu ("Text size")) {
            for (uint32_t k = 0; k < kFontStepCount; ++k)
                if (ImGui::MenuItem (Percent (kFontSteps[k]).c_str (), nullptr, k == store->fontStep))
                    SetFontStep (k);
            ImGui::EndMenu ();
        }
        if (!layerNames.empty () && ImGui::BeginMenu ("Layers")) {
            for (const std::string& name : layerNames) {
                const bool on = LayerShown (*store, name);
                if (ImGui::MenuItem ((Readable (name) + "##layer." + name).c_str (), nullptr, on))
                    ShowLayer (name, !on);
            }
            ImGui::EndMenu ();
        }
        if (ImGui::MenuItem ("Reset position", nullptr, false, store->host.placed))
            ResetPosition ();
        ImGui::EndPopup ();
    }
    ImGui::PopFont ();
    ImGui::PopStyleColor (colours);
    ImGui::PopStyleVar (kStyleVars);
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
