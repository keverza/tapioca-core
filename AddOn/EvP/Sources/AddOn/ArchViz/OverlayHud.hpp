#ifndef EVP_ARCHVIZ_OVERLAYHUD_HPP
#define EVP_ARCHVIZ_OVERLAYHUD_HPP

// ArchViz/OverlayHud -- the overlays' HUD panels (OverlayLayers.hpp `Panel`), laid out by
// Dear ImGui: a title, text, rows of figures, rules, progress bars, colour keys, colour
// ramps, plots and tables, in a panel fixed to a point of the view. Never clickable --
// the overlays take no input -- and never drawn by ImGui's own renderer.
//
// ⚠️ IMGUI LAYS OUT ON THE MAIN THREAD; THE OVERLAYS DRAW TRIANGLES. The 3D overlay draws
// on Archicad's render thread inside a Present hook, where §11 allows no locks and no
// allocation, and ImGui's current context is one process-wide global (ImGuiContextLock.hpp).
// So a panel is laid out here, whenever the layers change, under that lock, into plain
// triangles over ImGui's font atlas -- and the guest draws those like any other glyph
// quads (OverlayScene.hpp, `kPlainTexture`), anchored to the view by the vertex shader.
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
};

// Where a panel's top-left goes: a fraction of the view, and view pixels from it -- the
// panel's own anchor point on the view's, `offsetPixels` inwards.
void Place (const overlaylayers::Panel& panel, float width, float height, float scale, float fraction[2],
            float offset[2]);

struct Stats {
    uint32_t builds = 0;        // sets of panels laid out
    uint32_t frames = 0;        // ImGui frames they took
    uint32_t atlasVersions = 0; // font atlas versions made into pages
    uint32_t lastMilliseconds = 0;
};

class Engine final {
  public:
    Engine ();
    ~Engine ();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    // The font is the overlays' own (SceneTextFont.hpp); ImGui rasterises from it.
    bool Init (std::vector<uint8_t> fontBytes, std::string& error);
    bool Ready () const;

    // Every panel of a set, laid out at `scale` (the view's DPI scale): `out[i]` is
    // `panels[i]`. ⚠️ BUILT TOGETHER, AND AGAIN UNTIL THE ATLAS HOLDS STILL: a panel
    // that meets a new glyph grows the atlas, and a panel laid out before it grew would
    // sample where its glyphs used to be.
    bool Build (const std::vector<const overlaylayers::Panel*>& panels, float scale, std::vector<Built>& out,
                std::string& error);

    // The atlas as pages, current after the last `Build`: `Vertex::page` indexes it.
    const std::vector<std::shared_ptr<const overlaytext::Page>>& Pages () const;

    Stats GetStats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv

#endif
