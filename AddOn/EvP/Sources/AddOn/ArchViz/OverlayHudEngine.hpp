#ifndef EVP_ARCHVIZ_OVERLAYHUDENGINE_HPP
#define EVP_ARCHVIZ_OVERLAYHUDENGINE_HPP

// ArchViz/OverlayHudEngine -- the HUD engine's insides (OverlayHud.hpp `Engine`), shared by
// the translation units that lay it out: OverlayHud.cpp (the frame, the panels, the font
// atlas), OverlayHudHost.cpp (the floating panel, its tabs, the dock and the text size) and
// OverlayHudControls.cpp (the controls and the values held for them). Not an interface: nothing outside the engine
// includes it.
//
// MAIN THREAD, inside ImGui's lock (ImGuiContextLock.hpp) wherever a frame is laid out.

#include "ArchViz/OverlayHud.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow: which draw list is whose

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace layers = overlaylayers;

// The host's own tab, after the panels': the HUD's settings.
constexpr char kSettingsKey[] = "tapioca.settings";

// The dock's one tab: its font, its padding across and along its turned title, and the gap
// between it and a panel on the view's right column.
constexpr float kDockFontPixels = 12.0f;
constexpr float kDockPadding[2] = { 5.0f, 12.0f };
constexpr float kDockGap = 6.0f;
// ⚠️ THE TEXT SIZE IS A FEW STEPS, NOT A NUMBER (the user, 2026-09-29: a control for the
// HUD's font size). Settings and the HUD's menu choose one; every size of the HUD -- text,
// padding, widths, the dock itself -- follows, the distances from the view's edges do not.
constexpr float kFontSteps[] = { 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.4f, 1.6f, 1.8f, 2.0f };
constexpr uint32_t kFontStepCount = uint32_t (sizeof (kFontSteps) / sizeof (kFontSteps[0]));
constexpr uint32_t kFontStepDefault = 2;

// What the user did to each panel, by its key (OverlayHud.hpp's third note).
struct State {
    // A control's value: what its layer last said (`sent`) and what it is now (`current`)
    // -- the user's, until the layer says something new.
    struct Held {
        double sent = 0.0;
        double current = 0.0;
    };
    struct Panel {
        std::map<uint32_t, bool> sections;  // by item index
        std::map<std::string, Held> values; // by control id; the tab bar's by its id
    };
    std::map<std::string, Panel> panels;
    uint32_t fontStep = kFontStepDefault; // kFontSteps
    // ⚠️ SHOWN OR HIDDEN, NEVER DESTROYED (the user, 2026-09-30: the dock's circle toggles
    // the whole overlay and its HUD; Settings, each layer). Hidden, nothing of the overlay
    // is drawn but the dock's tab; a hidden layer's content and panels are not drawn. The
    // controller follows `revision` (overlaycontrol::FollowHudState).
    bool shown = true;
    std::set<std::string> hidden; // layer names
    bool hover = false;           // hover mode (OverlayHud.hpp `Hover`)
    uint64_t revision = 0;
    // The floating panel the titled panels are tabs of: open or closed to the dock, the tab
    // shown (a panel key, or kSettingsKey), and -- once the user has dragged it -- where: `offset` logical pixels
    // in from the edges of the view's `corner` nearest it (1 right, 2 bottom), so a view
    // resized keeps it that far from them.
    struct Host {
        bool known = false; // set from the panels the first time there were any
        bool open = true;
        std::string selected;
        bool placed = false;
        uint8_t corner = 0;
        float offset[2] = { 0.0f, 0.0f };
    };
    Host host;
};

// A panel's own look over the base; the number of colours pushed, and of style vars.
int PushPanelStyle (const layers::Panel& panel, float scale);
constexpr int kStyleVars = 3;

struct Engine::Impl {
    ImGuiContext* context = nullptr;
    ImFont* font = nullptr;
    std::vector<uint8_t> fontBytes; // ImGui reads it for as long as the atlas lives
    // A panel's own fonts, by path, their bytes kept as long; a path that failed maps
    // to the bundled font, tried once.
    FontLoader loader;
    std::map<std::string, ImFont*> fonts;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> fontData;
    std::string fontError;
    // One slot per ImGui texture, its TexID the slot's index plus one.
    std::vector<ImTextureData*> textures;
    std::vector<std::shared_ptr<const overlaytext::Page>> pages;
    uint64_t atlasVersion = 0;
    Stats stats;
    bool ready = false;

    // What the user did to each panel: this engine's own, or the views' shared one.
    using PanelState = State::Panel;
    std::shared_ptr<State> store = NewState ();
    // The panel being laid out -- its key, its layer -- and what the pointer is on this
    // frame; what the user changed in this build, and where that goes after it.
    std::string key;
    std::string layer;
    std::vector<Change> changes;
    ChangeSink sink;
    Layout::Highlight highlight;
    bool hand = false;
    // The windows in the frame being laid out: an untitled panel's by its place in the set
    // (a titled one's is none: it is a tab of the host), then the dock's, then the host's.
    std::vector<ImGuiWindow*> windows;
    // The layers drawn in this view, for Settings; the titled panels of the shown ones by
    // their place in the set; which the host shows (none: Settings, or no titled panel);
    // the look it takes; whether there is any HUD here at all -- a layer to show or hide.
    std::vector<std::string> layerNames;
    std::vector<size_t> titled;
    size_t shown = 0;
    bool showsPanel = false;
    const layers::Panel* look = nullptr;
    bool present = false;
    // The title of what the host shows -- a panel's, "Settings", or "Overlay" while there is
    // no titled panel: the dock's label, and what an open or a close is said with.
    std::string showing;
    // The tab the host's tab bar showed in this context's last frame: a panel key.
    std::string shownHost;
    // The dock's width this frame: how far the view's right column moves in.
    float inset = 0.0f;
    // Which tab each tab bar showed in this context's last frame, by panel key and bar id,
    // and the one it shows in this one; whether a dropdown is open.
    std::map<std::string, int> shownTabs;
    int tabNow = -1;
    bool popup = false;
    std::chrono::steady_clock::time_point lastBuild {};

    PanelState& StateOf (const std::string& key, const layers::Panel& panel);

    // Between frames, the context current and locked: ImGui adds a font to the atlas
    // only then.
    ImFont* FontFor (const std::string& path);

    // ImGui's texture requests, honoured: every created or updated texture becomes a
    // page with a new id, whole.
    void Sync (ImDrawData* data);

    void Items (const layers::Panel& panel, PanelState& state, float scale);

    // A panel without a title, at its anchor on the view: its items. A titled one is a tab
    // of the host; a hidden layer's is not drawn.
    // `scale` is the view's DPI scale, what the distances from its edges take; `ui` that
    // times the text size, what everything else takes.
    void Window (const layers::Panel& panel, const std::string& key, size_t index, float scale, float ui, ImVec2 view);

    // A legend's bar pointed at: the value there, beside the bar at the pointer.
    void LegendTips (const std::vector<LegendBar>& legends, float scale, ImVec2 view);

    // Hover mode: the item under the pointer tinted where it is (`Input::hover`); and what
    // the host reads out under its page -- what the pointer is on, or, while it is on the
    // HUD or a legend, what it was on (`readout`, OverlayHudHost.cpp).
    void HoverTint (const Hover& hover);
    void Readout ();
    Hover readout;

    // The titled panels found, the host's state made from them the first time, the tab it
    // shows held to one of them (OverlayHudHost.cpp).
    void Gather (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys);

    // The dock: one tab at the view's right edge, its title turned a quarter, that opens and
    // closes the host, and a circle on it that shows and hides the whole overlay.
    void Dock (const std::vector<const layers::Panel*>& panels, float scale, ImVec2 view);

    // The host: one floating panel, a tab per titled panel, then Settings, the close button
    // at the end of its tab row; where the user dragged it, kept.
    void Host (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
               float ui, ImVec2 view);

    // The host opened or closed: said, with the tab it shows.
    void Opening (bool open, const std::string& title);

    // The Settings page: the HUD's style, the overlay's display (OverlayHudHost.cpp).
    void Settings ();

    // The HUD's menu at the pointer, on a right click anywhere on it (OverlayHudHost.cpp).
    void Menu (float ui);

    // What Settings, the dock and the menu do, each held and said once: the whole overlay
    // shown or hidden, a layer shown or hidden, the host opened or closed, the host on its
    // Settings tab, the text size's step, the host back where its tab asks to be.
    void ShowOverlay (bool shown);
    void ShowLayer (const std::string& name, bool shown);
    void SetHover (bool on);
    void SetOpen (bool open);
    void ShowSettings ();
    void SetFontStep (uint32_t step);
    void ResetPosition ();

    // Each control's value as its layer says it now, held (OverlayHudControls.cpp).
    void Reconcile (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys);

    // A checkbox, a slider, a dropdown or a button, its value the held one; what the user
    // changes, said.
    void Control (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                  float width, float scale);

    // The tab bar's `number`th tab; true when its page is shown. `TabsDone` after the bar
    // takes the user's press on another.
    bool TabItem (const layers::PanelItem& tab, uint32_t number, const std::string& bar, PanelState& state);
    void TabsDone (const layers::Panel& panel, size_t first, const std::string& bar, PanelState& state);

    // One frame of the whole set.
    void Frame (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
                const Input& input, const std::vector<LegendBar>& legends, float delta);

    // Which of the set's windows a draw list belongs to -- the panels', then the dock's and
    // the host's -- and -1 for what floats over them.
    int PanelOf (const ImDrawList* list) const;

    // The last frame's triangles, each panel's from its top-left, against the pages as
    // they stand.
    void Collect (Layout& out, uint32_t& unsampled);
};

// The panel's tab bar's id -- its first tab's, or "tabs" -- with that tab's place in
// `first`; empty for a panel without tabs.
std::string TabBarId (const layers::Panel& panel, size_t* first = nullptr);

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv

#endif
