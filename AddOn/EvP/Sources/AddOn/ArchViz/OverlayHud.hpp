#ifndef EVP_ARCHVIZ_OVERLAYHUD_HPP
#define EVP_ARCHVIZ_OVERLAYHUD_HPP

// ArchViz/OverlayHud -- the overlays' HUD panels (OverlayLayers.hpp `Panel`), laid out by
// Dear ImGui: a title bar whose close button sends the panel to the dock, text, rows of
// figures, collapsible sections, rules, progress bars, colour keys, colour ramps, plots
// and tables, in a panel fixed to a point of the view -- the tooltips over them and over
// the legends -- and the dock: a tab per titled panel down the view's right edge. Never
// drawn by ImGui's own renderer.
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
// ⚠️ THE ADD-ON HOLDS THE STATE, NOT ONLY IMGUI. Whether a panel is in the dock and which
// of its sections are open are kept here by the panel's key (its layer and its place
// there), set into ImGui every frame and read back after it: a layer set again keeps what
// the user did to it. And the two views' engines share it (`State`): a panel docked in
// the 3D window is docked in the plan.
//
// ⚠️ A PANEL CLOSES TO THE DOCK, NOT IN PLACE (the user, 2026-09-29: the collapsed panel
// is a tab in a list down the view's right edge). A titled panel's close button puts it
// there; its tab -- filled while the panel is open -- opens and closes it. A panel on the
// view's right column moves in beside the dock.
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
};

// A legend's colour bar as it is on the view (drawn by the scene, OverlaySceneScreen),
// hovered for the value under the pointer.
struct LegendBar {
    const overlaylayers::Legend* legend = nullptr;
    std::string layer;                          // whose heatmaps it describes
    float rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // view pixels: left, top, right, bottom
};

// A set laid out: one per panel, and what floats over them -- the tooltips -- in view
// pixels from its top-left (`overlay.fraction` 0, 0).
struct Layout {
    std::vector<Built> panels; // a panel in the dock is empty: no size, no triangles
    Built overlay;
    // The dock, at the view's right edge half-way down; empty without a titled panel.
    Built dock;
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
};

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

    // What the user did to a panel, by its key: in the dock, and each section's open state
    // by its item's index. False for a key never laid out.
    bool Collapsed (const std::string& key) const;
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
