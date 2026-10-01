#ifndef EVP_ARCHVIZ_OVERLAYHUD_HPP
#define EVP_ARCHVIZ_OVERLAYHUD_HPP

// ArchViz/OverlayHud -- the overlays' HUD panels (OverlayLayers.hpp `Panel`), laid out by
// Dear ImGui: text, rows of figures, collapsible sections, rules, progress bars, colour
// keys, colour ramps, plots, tables and controls; the titled panels as the tabs of one
// floating host with the HUD's Settings, the untitled ones fixed to a point of the view;
// the tooltips over them and over the legends; the dock's one tab at the view's right
// edge; and the HUD's menu on a right click. Never drawn by ImGui's own renderer.
//
// ⚠️ IMGUI LAYS OUT ON THE MAIN THREAD; THE OVERLAYS DRAW TRIANGLES. The 3D overlay draws
// on Archicad's render thread inside a Present hook, where §11 allows no locks and no
// allocation, and ImGui's current context is one process-wide global (ImGuiContextLock.hpp).
// So a set of panels is laid out here, whenever the layers change or the pointer does
// something to them, under that lock, into plain triangles over ImGui's font atlas -- and
// the guests draw those like any other glyph quads (OverlayScene.hpp, `kPlainTexture`),
// anchored to the view by the vertex shader.
//
// ⚠️ ONE FRAME FOR EVERY PANEL, AT ITS PLACE IN THE VIEW. Hover, the widget being pressed
// and the tooltips are ImGui's context-wide state; a panel laid out in a frame of its own
// would drop what another one holds. The pointer (OverlayInput.hpp) is fed as ImGui input
// in view pixels, so what ImGui hovers is what the user points at. The vertices still go
// out relative to each panel's anchor, so a view resized before the next layout places
// them right.
//
// ⚠️ THE ADD-ON HOLDS THE STATE, NOT ONLY IMGUI. Which of a panel's sections are open and
// its controls' values are kept here by the panel's key (its layer and its place there),
// with the host's -- open, its tab, where it was dragged -- the text size and what is
// shown; set into ImGui every frame and read back after it, so a layer set again keeps
// what the user did to it. And the two views' engines share it (`State`): the host closed
// in the 3D window is closed in the plan.
//
// ⚠️ ONE FLOATING PANEL, THE TITLED PANELS ITS TABS (the user, 2026-09-30). Every panel with
// a title, of every layer, is a tab of the host: one small panel with no title bar, its tab
// row its head, the HUD's Settings its last tab and a close button at the row's end. The user drags
// it by anything that is not a control; where they leave it is kept from the view's
// nearest corner, in logical pixels, so a resized view keeps it there and inside. The dock
// is ONE tab at the view's right edge, its title turned a quarter, that opens and closes
// the host; a circle on it shows and hides the whole overlay (`ContentShown`). A panel
// without a title stands alone at its anchor, as before; one on the view's right column
// moves in beside the dock.
//
// ⚠️ THE TEXT SIZE IS THE USER'S: chosen in Settings or the HUD's menu, it scales the whole
// HUD -- text, padding, widths and the dock -- by a few steps, in both views; the
// distances from the view's edges stay.
//
// ⚠️ THIS ENGINE IS IMGUI'S BACKEND FOR TEXTURES. ImGui 1.92 grows its font atlas as it
// meets new glyphs and sizes (`ImGuiBackendFlags_RendererHasTextures`); each version of
// the atlas becomes an overlay page with a new id (OverlayText.hpp `NewPageId`), and the
// guests' page cache drops the versions nothing samples any more.
//
// ⚠️ LAID OUT AT THE VIEW'S PIXELS. A panel is built at the view's DPI scale, so its
// text is rasterised at the size it is shown; the vertex shader places it without
// scaling (`kPhysicalPixels`).
//
// MAIN THREAD. Pure apart from ImGui, so tests/cpp builds it with the vendored imgui.

#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayText.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

// One corner of a panel's triangles: view pixels from the panel's top-left, the atlas
// uv, the colour as 0xRRGGBBAA, and which of `Engine::Pages` it samples.
struct Vertex {
    float x = 0.0f, y = 0.0f;
    float u = 0.0f, v = 0.0f;
    uint32_t rgba = 0;
    uint32_t page = 0;
};

struct Built {
    std::vector<Vertex> vertices; // a triangle list
    float width = 0.0f;           // the panel, view pixels
    float height = 0.0f;
    // Where it goes: its top-left `offset` pixels from `fraction` of the view (Place).
    float fraction[2] = { 0.0f, 0.0f };
    float offset[2] = { 0.0f, 0.0f };
};

// Where a panel's top-left goes: a fraction of the view, and view pixels from it -- the
// panel's own anchor point on the view's, `offsetPixels` inwards.
// `inset` is how far the view's right column moves in: the dock's width and a gap.
void Place (const overlaylayers::Panel& panel, float width, float height, float scale, float fraction[2],
            float offset[2], float inset = 0.0f);

// ⚠️ HOVER MODE (the user's stage 3): what is under the pointer, read by the view's runtime
// (OverlayHover.hpp) and handed in with the pointer -- a storey slice's figures, a heatmap's
// value -- and the triangles of the item it found, in view pixels, to tint. While the mode
// is on (`HoverMode`) the HUD reads it out in its floating panel, under the page, and tints
// the item where it is (the user, 2026-10-01: in the panel, not by the pointer).
struct Hover {
    bool picks = false; // the view reads what is under the pointer: both views do (D19)
    bool active = false;
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows; // label, value
    std::vector<float> tint;                               // x, y view pixels, three corners a triangle
};

// The view a set is laid out for and the pointer over it (OverlayInput.hpp feeds it).
// ImGui numbers the buttons: 0 left, 1 right, 2 middle, 3 and 4 the side ones.
struct Input {
    float width = 0.0f; // the view, physical pixels; 0 while it is not known
    float height = 0.0f;
    bool pointer = false; // the pointer is over the view
    float x = 0.0f;
    float y = 0.0f;
    struct Button {
        int button = 0;
        bool down = false;
    };
    std::vector<Button> buttons; // since the last layout, in order
    Hover hover;
};

// A legend's colour bar as it is on the view (drawn by the scene, OverlaySceneScreen),
// hovered for the value under the pointer.
struct LegendBar {
    const overlaylayers::Legend* legend = nullptr;
    std::string layer;                          // whose heatmaps it describes
    float rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // view pixels: left, top, right, bottom
};

// What the user changed on the HUD in a layout: the host opened or closed, its tab, a
// section folded, the text size, its position reset, the overlay or a layer shown or
// hidden, a control's value. For Python (OverlayHudEvents.hpp); `key` splits into the
// panel's layer and place.
struct Change {
    // "hud", "panel", "section", "fontScale", "position", "overlay", "layer", "hover"; or a control's:
    // "checkbox", "slider", "combo", "tab", "button"
    std::string kind;
    std::string key;   // the panel's; empty for the HUD's own (the text size, Settings)
    std::string title; // the panel's
    std::string id;    // the control's id, the tab bar's; a section's title; "textSize"; a layer"
    int32_t item = -1; // its place among the panel's items
    double value = 0.0;
    std::string text; // what the value says
    bool final = true;
};

// A set laid out: one per panel, and what floats over them -- the tooltips -- in view
// pixels from its top-left (`overlay.fraction` 0, 0).
struct Layout {
    std::vector<Built> panels; // a titled panel's (a tab of the host) or a hidden layer's is empty
    Built overlay;
    // The dock's one tab, at the view's right edge half-way down, and the host -- the
    // floating panel the titled panels are tabs of, and the key of the one it shows. Both
    // empty without a layer in the view; the host empty while it is closed or hidden. A titled panel's
    // own `panels` entry is always empty: it is drawn as the host's tab.
    Built dock;
    Built host;
    std::string hostKey;
    // The ramp or legend the pointer is on: its layer, and the band of values under the
    // pointer -- that band of the layer's heatmaps is shown, the rest dimmed
    // (OverlayScene.hpp `Highlight`). A band is a colormap's band where it has them,
    // otherwise a twentieth of the range either side of the value.
    struct Highlight {
        bool active = false;
        std::string layer;
        double low = 0.0;
        double high = 0.0;
    };
    Highlight highlight;
    // The pointer is on something it can press, or pressing one: the input shows a hand
    // over it (OverlayInput.hpp), an arrow over the rest of the HUD.
    bool hand = false;
    // What the user changed in this layout, in the order they did it.
    std::vector<Change> changes;
    // A dropdown's list or the HUD's menu is open: the whole view is the HUD's until it
    // closes, so the click that closes it never reaches Archicad.
    bool popup = false;
};

// Where each change goes: the views' engines hand them to the event ring with their view.
using ChangeSink = std::function<void (const Change&)>;

struct Stats {
    uint32_t builds = 0;        // sets of panels laid out
    uint32_t frames = 0;        // ImGui frames they took
    uint32_t atlasVersions = 0; // font atlas versions made into pages
    uint32_t lastMilliseconds = 0;
    uint32_t fonts = 0; // in the atlas, the bundled one included
};

// What the user did to the panels, by key: shared by the views' engines (`UseState`).
struct State;
std::shared_ptr<State> NewState ();
// Forgets it all: the project whose layers it named closed (§8).
void ClearState (State& state);
// Its text size (Engine::FontScale), set to the nearest step; whether the host is open --
// closed until a titled panel says how it starts, or the user or Python opens it -- and the
// key of the panel it shows (empty before one was laid out).
float FontScaleOf (const State& state);
void SetFontScale (State& state, float scale);
bool HudOpen (const State& state);
std::string SelectedKey (const State& state);
void SetHudOpen (State& state, bool open);
void SelectKey (State& state, const std::string& key);
// The whole overlay shown (the dock's circle) and each layer (Settings): hidden, nothing of
// it is drawn -- but nothing is destroyed. `Revision` moves on every change, for the
// renderers to follow.
bool ContentShown (const State& state);
bool LayerShown (const State& state, const std::string& layer);
std::vector<std::string> HiddenLayers (const State& state);
void SetContentShown (State& state, bool shown);
void SetLayerShown (State& state, const std::string& layer, bool shown);
// Hover mode: off until the user turns it on in Settings or the HUD's menu, or Python does;
// both views. `Revision` moves with it.
bool HoverMode (const State& state);
void SetHoverMode (State& state, bool on);
uint64_t Revision (const State& state);
// The values a panel's controls hold, by id -- the tab bar's too -- in id order.
std::vector<std::pair<std::string, double>> Values (const State& state, const std::string& key);

// Reads the font file at a path a panel names (overlayfonts::Read).
using FontLoader = std::function<bool (const std::string& path, std::vector<uint8_t>& bytes, std::string& error)>;

class Engine final {
  public:
    Engine ();
    ~Engine ();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    // The font is the overlays' own (SceneTextFont.hpp); ImGui rasterises from it.
    bool Init (std::vector<uint8_t> fontBytes, std::string& error);
    bool Ready () const;

    // How a panel's `font` is read; without one every panel is in the bundled font.
    void SetFontLoader (FontLoader loader);
    // The state to keep what the user does in, instead of the engine's own.
    void UseState (std::shared_ptr<State> state);
    // Where the changes go, after each `Build`, outside ImGui's lock.
    void SetChangeSink (ChangeSink sink);
    // The layers drawn in the view the next `Build` is for, shown or hidden: what Settings
    // lists for the user to show and hide.
    void SetLayers (std::vector<std::string> names);

    // The HUD's text size, a factor on every size but the distances from the view's edges:
    // one of a few steps (0.8 to 2), chosen in Settings or the HUD's menu. Set, the step
    // nearest.
    float FontScale () const;
    void SetFontScale (float scale);

    // Every panel of a set, laid out at `scale` (the view's DPI scale) for `input`'s view
    // and pointer: `out.panels[i]` is `panels[i]`, whose state is kept under `keys[i]`.
    // ⚠️ BUILT TOGETHER, AND AGAIN UNTIL THE ATLAS HOLDS STILL: a panel that meets a new
    // glyph grows the atlas, and a panel laid out before it grew would sample where its
    // glyphs used to be.
    bool Build (const std::vector<const overlaylayers::Panel*>& panels, const std::vector<std::string>& keys,
                float scale, const Input& input, const std::vector<LegendBar>& legends, Layout& out,
                std::string& error);
    // With no pointer, keyed by place: the panels as they stand.
    bool Build (const std::vector<const overlaylayers::Panel*>& panels, float scale, std::vector<Built>& out,
                std::string& error);

    // The atlas as pages, current after the last `Build`: `Vertex::page` indexes it.
    const std::vector<std::shared_ptr<const overlaytext::Page>>& Pages () const;

    // Whether the host is open, and the key of the panel it shows.
    bool Open () const;
    std::string Selected () const;
    // Each section's open state, by its panel's key and its item's index. False for a key
    // never laid out.
    bool SectionOpen (const std::string& key, uint32_t item, bool& open) const;

    Stats GetStats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv

#endif
