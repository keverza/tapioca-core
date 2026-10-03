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

using hudshell::Percent;

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
    statsCards.clear ();
    const bool standalone = own.standalone;
    for (size_t i = 0; i < panels.size (); ++i) {
        if (!LayerShown (*store, LayerOf (keys[i])))
            continue;
        // A card on the Stats page where the HUD has one; elsewhere what it would be without.
        if (standalone && panels[i]->tab == hudshell::kStatsTab)
            statsCards.push_back (i);
        else if (!panels[i]->title.empty ())
            titled.push_back (i);
    }
    // Any layer drawn here -- shown or hidden -- is a reason for the dock: its circle shows
    // and hides them, Settings lists them. The overlay running here is reason enough.
    present = standalone || !titled.empty () || !layerNames.empty ();
    showsPanel = false;
    // The look the host takes when it shows no layer's panel -- Settings, with no titled
    // panel to borrow from: the light card, small.
    look = &hudshell::PlainLook ();
    if (!present)
        return;
    State::Host& host = store->host;
    // The first titled panel ever says how the host starts, open or closed; before one,
    // it is closed (HudOpen) -- a layer of lines alone does not open a panel. A HUD that is
    // there for the overlay starts open.
    if (!host.known && !titled.empty ()) {
        host.known = true;
        host.open = !panels[titled.front ()]->collapsed;
    }
    if (!host.known && standalone) {
        host.known = true;
        host.open = true;
    }
    // ⚠️ A PANEL THAT JUST ARRIVED IS SHOWN: the user ran what made it. Once -- a layer set
    // again with the same panels takes nothing from the tab the user chose since.
    if (standalone) {
        std::string arrived;
        for (const size_t i : titled)
            if (store->seenPanels.insert (keys[i]).second && arrived.empty ())
                arrived = keys[i];
        if (!arrived.empty ())
            host.selected = arrived;
    }
    // The tab held is one of them or an own page: otherwise Stats, or -- without own pages --
    // the first panel, when the one held is gone.
    bool held =
        host.selected == kSettingsKey ||
        (standalone && (host.selected == kStatsKey || host.selected == kSelectionKey || host.selected == kDebugKey));
    for (const size_t i : titled)
        held = held || keys[i] == host.selected;
    if (!held)
        host.selected = standalone        ? std::string (kStatsKey)
                        : titled.empty () ? std::string (kSettingsKey)
                                          : keys[titled.front ()];
    for (const size_t i : titled)
        if (keys[i] == host.selected) {
            shown = i;
            showsPanel = true;
        }
    // An own page takes the look of the first panel, when there is one.
    look = showsPanel             ? panels[shown]
           : !titled.empty ()     ? panels[titled.front ()]
           : !statsCards.empty () ? panels[statsCards.front ()]
                                  : &hudshell::PlainLook ();
    showing = showsPanel        ? panels[shown]->title
              : standalone      ? TitleOf (host.selected, panels, keys)
              : titled.empty () ? look->title
                                : "Settings";
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
    // The readout is in the host: on, the host opens to show it.
    if (on)
        SetOpen (true);
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
    if (!store->host.placement.placed)
        return;
    store->host.placement.placed = false;
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
        // The overlay's circle says its state and whether it is shown; the viewer's, where the
        // HUD is the overlay's own, is the switch to it. Without own pages: the one circle.
        hudshell::Circle top = own.standalone ? own.overlay : hudshell::Circle ();
        if (!own.standalone)
            top.phase = hudshell::Phase::Ready;
        top.active = store->shown;
        const hudshell::DockPress press =
            hudshell::DockTab ("##hud", showing, colours, open, top, own.standalone ? &own.viewer : nullptr,
                               ImVec2 (kDockPadding[0] * scale, kDockPadding[1] * scale), scale);
        const bool pressed = press.title;
        if (press.bottom) {
            store->viewerRequested = true;
            changes.push_back ({ "surface", std::string (), std::string (), std::string (), -1, 1.0, "viewer", true });
        }
        else if (press.top) {
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
    // Every HUD's own rows (HudShell.hpp): the step and the position, each change said once.
    uint32_t step = store->fontStep;
    hudshell::Placement placement = store->host.placement;
    bool reset = false;
    if (hudshell::HudSettings (step, placement, reset)) {
        if (reset)
            ResetPosition ();
        else
            SetFontStep (step);
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

// ⚠️ HOVER MODE'S READOUT IS IN THE HOST (the user, 2026-10-01: in the ImGui panel, not near
// the pointer). Under the page, whichever tab it shows: what the pointer is on -- a slice's
// figures, a heatmap's value -- held while the pointer is on the HUD (Frame). A view that does
// not read under the pointer says so rather than "nothing".
void Engine::Impl::Readout ()
{
    ImGui::SeparatorText ("Under the pointer");
    if (!readout.picks) {
        ImGui::TextDisabled ("Not read in this view yet");
        return;
    }
    if (!readout.active) {
        ImGui::TextDisabled ("Nothing here");
        return;
    }
    if (!readout.title.empty ())
        ImGui::TextUnformatted (readout.title.c_str ());
    if (readout.rows.empty () ||
        !ImGui::BeginTable ("##readout", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
        return;
    for (const auto& row : readout.rows) {
        ImGui::TableNextRow ();
        ImGui::TableSetColumnIndex (0);
        ImGui::TextDisabled ("%s", row.first.c_str ());
        ImGui::TableSetColumnIndex (1);
        ImGui::TextUnformatted (row.second.c_str ());
    }
    ImGui::EndTable ();
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
    hostScrolls = false;
    if (!present || !HudOpen (*store) || !store->shown)
        return;
    hudshell::HostSpec spec;
    spec.look = look;
    spec.font = FontFor (look->font);
    spec.scale = scale;
    spec.ui = ui;
    spec.view = view;
    spec.inset = inset;
    // Stats and Selection, the panels, Settings and Debug; without own pages the panels and
    // Settings.
    if (own.standalone) {
        spec.tabs.push_back ({ kStatsKey, "Stats" });
        spec.tabs.push_back ({ kSelectionKey, "Selection" });
    }
    for (const size_t i : titled)
        spec.tabs.push_back ({ keys[i], panels[i]->title });
    spec.tabs.push_back ({ kSettingsKey, "Settings" });
    if (own.standalone)
        spec.tabs.push_back ({ kDebugKey, "Debug" });
    const auto page = [&] (const std::string& k) {
        if (k == kSettingsKey) {
            Settings ();
            return;
        }
        if (k == kStatsKey) {
            StatsPage (panels, keys, ui);
            return;
        }
        if (k == kSelectionKey) {
            SelectionPage (ui);
            return;
        }
        if (k == kDebugKey) {
            DebugPage (ui);
            return;
        }
        for (const size_t i : titled)
            if (keys[i] == k) {
                key = k;
                layer = LayerOf (k);
                Items (*panels[i], StateOf (k, *panels[i]), ui);
                return;
            }
    };
    const hudshell::HostResult result = hudshell::Host (spec, host.selected, shownHost, host.placement, page, [&] () {
        if (store->hover)
            Readout ();
    });
    windows.back () = result.window;
    hostScrolls = result.scrolls;
    if (!result.pressed.empty ()) {
        host.selected = result.pressed;
        const std::string title = TitleOf (result.pressed, panels, keys);
        bool panel = false;
        for (const size_t i : titled)
            panel = panel || keys[i] == result.pressed;
        changes.push_back (
            { "panel", panel ? result.pressed : std::string (), title, std::string (), -1, 1.0, title, true });
    }
    if (result.closed)
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
    // Released over any of the HUD's windows -- the menu's own included: it opens again there --
    // but not where a control of the HUD answered it (hudshell::ClaimRightClick).
    if (ImGui::IsMouseReleased (ImGuiMouseButton_Right) && ImGui::GetCurrentContext ()->HoveredWindow != nullptr &&
        !hudshell::RightClickClaimed ())
        ImGui::OpenPopup (kMenu);
    const layers::Panel& panel = *look;
    const int colours = hudshell::PushLook (panel, ui);
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
        if (ImGui::MenuItem ("Reset position", nullptr, false, store->host.placement.placed))
            ResetPosition ();
        ImGui::EndPopup ();
    }
    ImGui::PopFont ();
    ImGui::PopStyleColor (colours);
    ImGui::PopStyleVar (hudshell::kLookVars);
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
