#ifndef EVP_ARCHVIZ_OVERLAYHUDENGINE_HPP
#define EVP_ARCHVIZ_OVERLAYHUDENGINE_HPP

// ArchViz/OverlayHudEngine -- the HUD engine's insides (OverlayHud.hpp `Engine`), shared by
// the translation units that lay it out: OverlayHud.cpp (the frame, the panels, the font
// atlas) and OverlayHudDock.cpp (the dock and the text size). Not an interface: nothing
// outside the engine includes it.
//
// MAIN THREAD, inside ImGui's lock (ImGuiContextLock.hpp) wherever a frame is laid out.

#include "ArchViz/OverlayHud.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow: which draw list is whose

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace layers = overlaylayers;

// The dock's tabs: their font, their padding round the title, the gap between them, and
// the gap between the dock and a panel on the view's right column.
constexpr float kDockFontPixels = 13.0f;
constexpr float kDockPadding[2] = { 12.0f, 7.0f };
constexpr float kDockSpacing = 4.0f;
constexpr float kDockGap = 8.0f;
// ⚠️ THE TEXT SIZE IS A FEW STEPS, NOT A NUMBER (the user, 2026-09-29: a control for the
// HUD's font size). The dock's smaller and larger buttons walk them; every size of the
// HUD -- text, padding, widths, the dock itself -- follows, the distances from the view's
// edges do not.
constexpr float kFontSteps[] = { 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.4f, 1.6f, 1.8f, 2.0f };
constexpr uint32_t kFontStepCount = uint32_t (sizeof (kFontSteps) / sizeof (kFontSteps[0]));
constexpr uint32_t kFontStepDefault = 2;

// What the user did to each panel, by its key (OverlayHud.hpp's third note).
struct State {
    struct Panel {
        bool collapsed = false;            // in the dock
        std::map<uint32_t, bool> sections; // by item index
    };
    std::map<std::string, Panel> panels;
    uint32_t fontStep = kFontStepDefault; // kFontSteps
};

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
    // The panels' windows in the frame being laid out, by the panel's place in the set,
    // and the dock's after them.
    std::vector<ImGuiWindow*> windows;
    // The dock's width this frame: how far the view's right column moves in.
    float inset = 0.0f;
    std::chrono::steady_clock::time_point lastBuild {};

    PanelState& StateOf (const std::string& key, const layers::Panel& panel);

    // Between frames, the context current and locked: ImGui adds a font to the atlas
    // only then.
    ImFont* FontFor (const std::string& path);

    // ImGui's texture requests, honoured: every created or updated texture becomes a
    // page with a new id, whole.
    void Sync (ImDrawData* data);

    void Items (const layers::Panel& panel, PanelState& state, float scale);

    // One panel's window, at its anchor on the view: its title bar -- whose close button
    // sends it to the dock -- when it has a title, then its items. Nothing while it is in
    // the dock.
    // `scale` is the view's DPI scale, what the distances from its edges take; `ui` that
    // times the text size, what everything else takes.
    void Window (const layers::Panel& panel, const std::string& key, size_t index, float scale, float ui, ImVec2 view);

    // A legend's bar pointed at: the value there, beside the bar at the pointer.
    void LegendTips (const std::vector<LegendBar>& legends, float scale, ImVec2 view);

    // ⚠️ THE DOCK: a tab per titled panel down the view's right edge, half-way down, all
    // as wide as the widest title (the user, 2026-09-29). Filled with the panel's accent
    // while the panel is open; its card's own colours while the panel is in the dock. A
    // press on it opens or closes the panel -- in this frame, as the dock is laid out
    // before the panels.
    void Dock (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
               ImVec2 view);

    // A panel sent to the dock or opened from it: said.
    void Docking (const layers::Panel& panel, const std::string& panelKey, bool docked);

    // The text size under the tabs: smaller and larger, side by side, in the first titled
    // panel's colours; pointed at, the size they give, beside the dock.
    void FontButtons (const layers::Panel& colours, ImVec2 tab, float scale);

    // One frame of the whole set.
    void Frame (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys, float scale,
                const Input& input, const std::vector<LegendBar>& legends, float delta);

    // Which of the set's windows a draw list belongs to -- the dock's is the last -- and
    // -1 for what floats over them.
    int PanelOf (const ImDrawList* list) const;

    // The last frame's triangles, each panel's from its top-left, against the pages as
    // they stand.
    void Collect (Layout& out, uint32_t& unsampled);
};

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv

#endif
