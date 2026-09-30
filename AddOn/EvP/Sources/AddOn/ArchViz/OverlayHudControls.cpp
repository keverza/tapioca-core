// ArchViz/OverlayHudControls -- the HUD's controls: the engine's (OverlayHudEngine.hpp)
// checkboxes, sliders, dropdowns, buttons and tab bar, and the values it holds for them.
// See OverlayHud.hpp's notes on what the add-on holds.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <algorithm>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace {

// `given` is what the layer says now. ⚠️ THE CALLER'S VALUE WINS ONLY WHEN IT CHANGES: a
// layer set again with what it said before keeps what the user set; one that says
// something new -- Python changed its mind -- sets it.
void Hold (State::Panel& state, const std::string& id, double given)
{
    const auto made = state.values.try_emplace (id);
    State::Held& held = made.first->second;
    if (made.second || given != held.sent) {
        held.sent = given;
        held.current = given;
    }
}

} // namespace

std::string TabBarId (const layers::Panel& panel, size_t* first)
{
    for (size_t i = 0; i < panel.items.size (); ++i)
        if (panel.items[i].kind == layers::ItemKind::Tab) {
            if (first != nullptr)
                *first = i;
            return panel.items[i].id.empty () ? std::string ("tabs") : panel.items[i].id;
        }
    return std::string ();
}

void Engine::Impl::Reconcile (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys)
{
    for (size_t p = 0; p < panels.size (); ++p) {
        const layers::Panel& panel = *panels[p];
        PanelState& state = StateOf (keys[p], panel);
        for (const layers::PanelItem& item : panel.items) {
            switch (item.kind) {
                case layers::ItemKind::Checkbox:
                    Hold (state, item.id, item.checked ? 1.0 : 0.0);
                    break;
                case layers::ItemKind::Slider:
                    Hold (state, item.id, (std::min) ((std::max) (item.number, item.min), item.max));
                    break;
                case layers::ItemKind::Combo:
                    Hold (state, item.id, double (item.selected));
                    break;
                default:
                    break;
            }
        }
        size_t first = panel.items.size ();
        const std::string bar = TabBarId (panel, &first);
        if (!bar.empty ())
            Hold (state, bar, double (panel.items[first].selected));
    }
}

void Engine::Impl::Control (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                            float width, float scale)
{
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    auto said = [&] (const char* kind, double value, std::string text, bool final) {
        changes.push_back ({ kind, key, panel.title, item.id, int32_t (index), value, std::move (text), final });
    };
    if (item.kind == layers::ItemKind::Button) {
        if (items::Button (item, w))
            said ("button", 1.0, item.text, true);
        return;
    }
    State::Held& held = state.values[item.id]; // Reconcile made it
    switch (item.kind) {
        case layers::ItemKind::Checkbox: {
            bool on = held.current != 0.0;
            if (items::Checkbox (item, on)) {
                held.current = on ? 1.0 : 0.0;
                said ("checkbox", held.current, on ? "on" : "off", true);
            }
            break;
        }
        case layers::ItemKind::Slider: {
            double value = held.current;
            bool released = false;
            const bool moved = items::Slider (panel, item, value, w, released);
            // ⚠️ A DRAG SAYS EVERY VALUE ON THE WAY, NOT FINAL, AND THE RELEASE SAYS THE LAST ONE,
            // FINAL: a script that only acts on the result waits for `final`.
            if (moved) {
                held.current = value;
                said ("slider", value, items::SliderText (item, value), false);
            }
            if (released)
                said ("slider", held.current, items::SliderText (item, held.current), true);
            break;
        }
        case layers::ItemKind::Combo: {
            uint32_t chosen = uint32_t ((std::max) (held.current, 0.0));
            if (items::Combo (panel, item, chosen, w)) {
                held.current = double (chosen);
                said ("combo", held.current, chosen < item.labels.size () ? item.labels[chosen] : std::string (), true);
            }
            break;
        }
        default:
            break;
    }
}

bool Engine::Impl::TabItem (const layers::PanelItem& tab, uint32_t number, const std::string& bar, PanelState& state)
{
    const int held = int (state.values[bar].current);
    const auto shown = shownTabs.find (key + "/" + bar);
    const int before = shown == shownTabs.end () ? -1 : shown->second;
    // ⚠️ THE HELD TAB IS ASKED FOR ONLY WHERE THIS CONTEXT SHOWED ANOTHER -- the first
    // frame, Python's new choice, the other view's press. Asked every frame, it would be
    // applied over the user's click, which ImGui takes a frame later.
    const ImGuiTabItemFlags flags = held == int (number) && before != held ? ImGuiTabItemFlags_SetSelected : 0;
    const std::string label = tab.text + "###tab" + std::to_string (number);
    if (!ImGui::BeginTabItem (label.c_str (), nullptr, flags))
        return false;
    tabNow = int (number);
    return true;
}

void Engine::Impl::TabsDone (const layers::Panel& panel, size_t first, const std::string& bar, PanelState& state)
{
    int& before = shownTabs.try_emplace (key + "/" + bar, -1).first->second;
    State::Held& held = state.values[bar];
    // The user's press: a tab this context did not show last frame, and not the held one
    // it was asked to show.
    if (tabNow >= 0 && before >= 0 && tabNow != before && tabNow != int (held.current)) {
        held.current = double (tabNow);
        std::string title;
        int number = 0;
        for (size_t i = first; i < panel.items.size (); ++i)
            if (panel.items[i].kind == layers::ItemKind::Tab && number++ == tabNow)
                title = panel.items[i].text;
        changes.push_back ({ "tab", key, panel.title, bar, int32_t (first), held.current, title, true });
    }
    before = tabNow;
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
